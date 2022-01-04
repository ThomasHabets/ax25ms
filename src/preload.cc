/*
   Copyright 2021 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
// Local project stuff.
#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"
#include "proto/gen/ax25.grpc.pb.h"
#include "proto/gen/ax25.pb.h"

// Third party.
#include <grpcpp/grpcpp.h>


// C
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <netax25/ax25.h>
#include <netax25/axlib.h>

// C++
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace {

// Override function types.
using socket_func_t = int (*)(int, int, int);
using read_func_t = ssize_t (*)(int, void*, size_t);
using write_func_t = ssize_t (*)(int, const void*, size_t);
using bind_func_t = int (*)(int, const struct sockaddr*, socklen_t);
using connect_func_t = int (*)(int, const struct sockaddr*, socklen_t);
using getsockopt_func_t = int (*)(int, int, int, void*, socklen_t*);
using setsockopt_func_t = int (*)(int, int, int, const void*, socklen_t);
using close_func_t = int (*)(int);

// Saved original function pointers.
socket_func_t orig_socket = nullptr;
read_func_t orig_read = nullptr;
write_func_t orig_write = nullptr;
bind_func_t orig_bind = nullptr;
connect_func_t orig_connect = nullptr;
getsockopt_func_t orig_getsockopt = nullptr;
setsockopt_func_t orig_setsockopt = nullptr;
close_func_t orig_close = nullptr;

// Parameters settable via env.
char* radio_addr = nullptr;  // AX25_ADDR
char* router_addr = nullptr; // AX25_ROUTER
char* debug = nullptr;       // AX25_DEBUG

// Other consts.
const char* log_prefix = "preload: ";

std::ostream& log()
{
    if (debug) {
        static std::ofstream* f = nullptr;
        if (!f) {
            f = new std::ofstream;
            f->rdbuf()->pubsetbuf(0, 0);
            f->open(debug);
        }
        *f << log_prefix;
        return *f;
    }
    // A benefit of writing to /dev/null is that it'll still be
    // visible in strace.
    static std::ofstream null("/dev/null");
    return null;
}

// Pipe represents two fds connected together. It's actually used
// with a socketpair(), so that select() works for both read and
// write.
class Pipe
{
public:
    Pipe(std::pair<int, int> fds) : fds_(fds) {}

    // No copy.
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    // Move ok.
    Pipe(Pipe&& rhs) { fds_ = std::exchange(rhs.fds_, { -1, -1 }); }
    Pipe& operator=(Pipe&& rhs)
    {
        fds_ = std::exchange(rhs.fds_, { -1, -1 });
        return *this;
    }
    ~Pipe()
    {
        close_handler_fd();
        close_client_fd();
    }
    int client_fd() const noexcept
    {
        std::unique_lock<std::mutex> lk(mu_);
        return fds_.first;
    }
    int handler_fd() const noexcept
    {
        std::unique_lock<std::mutex> lk(mu_);
        return fds_.second;
    }
    void write_handler_fd(std::string_view sv)
    {
        const auto rc = orig_write(handler_fd(), sv.data(), sv.size());
        if (rc != static_cast<ssize_t>(sv.size())) {
            // TODO
            throw std::runtime_error("write failed: " + std::to_string(rc) + " " +
                                     strerror(errno));
        }
    }

    void close_client_fd() noexcept
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (fds_.first >= 0) {
            orig_close(fds_.first);
            fds_.first = -1;
        }
    }
    void close_handler_fd() noexcept
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (fds_.second >= 0) {
            orig_close(fds_.second);
            fds_.second = -1;
        }
    }

private:
    mutable std::mutex mu_;
    std::pair<int, int> fds_;
};


/*
 * Connection represents one socket and connection throughout its
 * lifetime.
 */
class Connection
{
public:
    Connection(int type, int protocol);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection& operator=(Connection&&) = delete;


    int fd() const noexcept { return fds_.client_fd(); }
    ssize_t read(void* buf, size_t count);
    ssize_t write(const void* buf, size_t count);
    int getsockopt(int level, int optname, void* optval, socklen_t* optlen);
    int setsockopt(int level, int optname, const void* optval, socklen_t optlen);
    int bind(const struct sockaddr* addr, socklen_t addrlen);
    int connect(const struct sockaddr* addr, socklen_t addrlen);

private:
    void read_thread_main();

    std::mutex read_queue_mu_;
    std::condition_variable read_queue_cv_;
    std::deque<ax25ms::SeqConnectResponse> read_queue_;
    bool read_ready_ = false;
    bool read_stop_ = false; // There is no stream_ctx_.IsCancelled()
                             // (?), so need this bool.

    Pipe fds_;
    int type_;
    int protocol_;
    bool extseq_ = false;
    int paclen_ = 200; // TODO
    grpc::ClientContext stream_ctx_;

    std::unique_ptr<
        grpc::ClientReaderWriter<ax25ms::SeqConnectRequest, ax25ms::SeqConnectResponse>>
        stream_;

    std::string src_;

    // Put read_thread_ last so that all other member variables are
    // initialized. And on destruction no other member function will
    // be destroyed until the thread has ended.
    std::jthread read_thread_;
};

/*
 * Connections keeps a map of all the Connection instances, thread
 * safe.
 */
class Connections
{
public:
    using key_t = int;
    using val_t = std::unique_ptr<Connection>;
    using map_t = std::map<key_t, val_t>;

    // Get connection by fd.
    Connection* get(int fd);

    // Insert a new connection keyed by its fd.
    // Caller does not
    bool insert(val_t&& con);
    map_t::node_type extract(key_t val);

private:
    std::mutex mu_;
    map_t connections_;
};

Connections connections;

Connection* Connections::get(int fd)
{
    std::unique_lock<std::mutex> lk(mu_);
    auto itr = connections_.find(fd);
    if (itr == connections_.end()) {
        return nullptr;
    }
    return itr->second.get();
}

Connections::map_t::node_type Connections::extract(key_t val)
{
    std::unique_lock<std::mutex> lk(mu_);
    return connections_.extract(val);
}

bool Connections::insert(val_t&& con)
{
    std::unique_lock<std::mutex> lk(mu_);
    const auto fd = con->fd();
    const auto [_, ok] = connections_.insert({ fd, std::move(con) });
    return ok;
}

__attribute__((constructor)) void init()
{
    fprintf(stderr, "%sinit\n", log_prefix);
    orig_socket = reinterpret_cast<socket_func_t>(dlsym(RTLD_NEXT, "socket"));
    orig_read = reinterpret_cast<read_func_t>(dlsym(RTLD_NEXT, "read"));
    orig_write = reinterpret_cast<write_func_t>(dlsym(RTLD_NEXT, "write"));
    orig_bind = reinterpret_cast<bind_func_t>(dlsym(RTLD_NEXT, "bind"));
    orig_connect = reinterpret_cast<connect_func_t>(dlsym(RTLD_NEXT, "connect"));
    orig_getsockopt = reinterpret_cast<getsockopt_func_t>(dlsym(RTLD_NEXT, "getsockopt"));
    orig_setsockopt = reinterpret_cast<setsockopt_func_t>(dlsym(RTLD_NEXT, "setsockopt"));
    orig_close = reinterpret_cast<close_func_t>(dlsym(RTLD_NEXT, "close"));

    if (auto e = getenv("AX25_ADDR"); e == nullptr) {
        fprintf(stderr, "%sError: AX25_ADDR not set. Setting 'INVALID'\n", log_prefix);
        radio_addr = strdup("INVALID");
    } else {
        radio_addr = strdup(e);
    }

    if (auto e = getenv("AX25_ROUTER"); e == nullptr) {
        fprintf(stderr,
                "%sError: AX25_ROUTER not set. Setting 'localhost:12345'\n",
                log_prefix);
        router_addr = strdup("localhost:12345");
    } else {
        router_addr = strdup(e);
    }

    debug = getenv("AX25_DEBUG");
}

std::pair<int, int> make_fds()
{
    // Create a signalling fd pair for things like select().
    int fds[2];
    if (-1 == socketpair(PF_LOCAL, SOCK_STREAM, 0, fds)) {
        throw std::runtime_error(std::string("socketpair(): ") + strerror(errno));
    }
    return { fds[0], fds[1] };
}

ax25ms::SeqPacketService::Stub* router()
{
    static auto ret = []() -> auto
    {
        auto channel =
            grpc::CreateChannel(router_addr, grpc::InsecureChannelCredentials());
        auto seq = ax25ms::SeqPacketService::NewStub(channel);
        return seq;
    }
    ();
    return ret.get();
}

Connection::Connection(int type, int protocol)
    : fds_(make_fds()),
      type_(type),
      protocol_(protocol),
      read_thread_([this] { read_thread_main(); })
{
}

Connection::~Connection()
{
    std::unique_lock<std::mutex> lk(read_queue_mu_);
    read_stop_ = true;
    stream_ctx_.TryCancel(); // End ongoing and future stream_->Read().
    read_queue_cv_.notify_all();
    // read_thread_ is a jthread, so no need to wait for it to exit.
}

void Connection::read_thread_main()
{
    {
        std::unique_lock<std::mutex> lk(read_queue_mu_);
        read_queue_cv_.wait(lk, [this] { return read_ready_; });
    }

    // Connection failed.
    if (!stream_) {
        return;
    }

    ax25ms::SeqConnectResponse resp;

    while (stream_->Read(&resp)) {
        {
            std::unique_lock<std::mutex> lk(read_queue_mu_);
            read_queue_cv_.wait(lk, [this] {
                return read_queue_.size() < 10 || read_stop_; // TODO: max value
            });
            if (read_stop_) {
                log() << "Read queue ending\n";
                break;
            }
            read_queue_.push_back(resp);
            read_queue_cv_.notify_one();
        }
        fds_.write_handler_fd("x");
    }
    // TODO: Finish() and stuff.
    log() << "Stream ended\n";
    fds_.close_handler_fd();
}

ssize_t Connection::read(void* buf, size_t count)
{
    for (;;) {
        ax25ms::SeqConnectResponse resp;
        {
            std::unique_lock<std::mutex> lk(read_queue_mu_);
            read_queue_cv_.wait(lk, [this] { return !read_queue_.empty(); });
            resp = read_queue_.front();
            read_queue_.pop_front();
            read_queue_cv_.notify_one();
        }
        // Flush token.
        {
            char buf[1];
            const auto rc = orig_read(fds_.client_fd(), buf, 1);
            if (rc != 1) {
                // TODO
                throw std::runtime_error("couldn't read the token: " +
                                         std::to_string(rc) + " " + strerror(errno));
            }
        }
        if (!resp.has_packet()) {
            continue;
        }
        const auto& payload = resp.packet().payload();
        // TODO: don't truncate.
        const auto size = std::min(count, payload.size());
        memcpy(buf, payload.data(), size);
        return size;
    }
}

ssize_t Connection::write(const void* buf, size_t count)
{
    ax25ms::SeqConnectRequest req;
    req.mutable_packet()->set_payload(buf, count);
    if (!stream_->Write(req)) {
        // TODO: log more info.
        errno = ECONNRESET;
        return -1;
    }
    return count;
}

int Connection::getsockopt(int level, int optname, void* optval, socklen_t* optlen)
{
    if (level != SOL_AX25) {
        errno = ENOSYS;
        return -1;
    }
    if (*optlen < sizeof(int)) {
        errno = EINVAL;
        return -1;
    }
    switch (optname) {
    case AX25_EXTSEQ: {
        const int ex = extseq_ ? 1 : 0;
        memcpy(optval, &ex, sizeof(int));
        *optlen = sizeof(int);
        return 0;
    }
    case AX25_PACLEN:
        memcpy(optval, &paclen_, sizeof(int));
        *optlen = sizeof(int);
        return 0;
        // TODO: support the other options.
    }

    errno = EINVAL;
    return -1;
}

int Connection::setsockopt(int level, int optname, const void* optval, socklen_t optlen)
{
    if (level != SOL_AX25) {
        log() << "setsockopt(UNKNOWN)\n";
        errno = ENOSYS;
        return -1;
    }

    if (optlen == 0) {
        log() << "setsockopt(SOL_AX25, , , zero length)\n";
        errno = EINVAL;
        return -1;
    }
    switch (optname) {
    case AX25_EXTSEQ: {
        // TODO: set it.
        std::vector<uint8_t> zero(optlen, 0);
        if (!memcmp(optval, zero.data(), optlen)) {
            extseq_ = false;
        } else {
            extseq_ = true;
        }
        log() << "setsockopt(SOL_AX25, AX25_EXTSEQ, " << extseq_ << ")\n";
        return 0;
    }
    case AX25_PACLEN:
        // TODO: set it.
        log() << "setsockopt(SOL_AX25, AX25_PACLEN)\n";
        return 0;
    case AX25_WINDOW:
        // TODO: set it.
        log() << "setsockopt(SOL_AX25, AX25_WINDOW)\n";
        return 0;
    case AX25_BACKOFF:
        // TODO: set it.
        log() << "setsockopt(SOL_AX25, AX25_BACKOFF)\n";
        return 0;
    case AX25_T1:
        // TODO: set it.
        log() << "setsockopt(SOL_AX25, AX25_T1)\n";
        return 0;
    case AX25_T2:
        // TODO: set it.
        log() << "setsockopt(SOL_AX25, AX25_T2)\n";
        return 0;
    case AX25_T3:
        // TODO: set it.
        log() << "setsockopt(SOL_AX25, AX25_T3)\n";
        return 0;
    case AX25_N2:
        // TODO: set it.
        log() << "setsockopt(SOL_AX25, AX25_N2)\n";
        return 0;
#if 0
    case AX25_HDRINCL:
        // TODO: support this.
        errno = EINVAL;
        return -1;
#endif
    }
    log() << "setsockopt(SOL_AX25, UNKNOWN)\n";
    errno = EINVAL;
    return -1;
}

int Connection::bind(const struct sockaddr* addr, socklen_t addrlen)
{
    if (addr->sa_family != AF_AX25) {
        errno = EINVAL;
        return -1;
    }
    auto sa = reinterpret_cast<const struct full_sockaddr_ax25*>(addr);
    src_ = ax25_ntoa(&sa->fsa_ax25.sax25_call);
    return 0;
}

int Connection::connect(const struct sockaddr* addr, socklen_t addrlen)
{
    if (addr->sa_family != AF_AX25) {
        errno = EINVAL;
        return -1;
    }
    auto sa = reinterpret_cast<const struct full_sockaddr_ax25*>(addr);
    const std::string dst = ax25_ntoa(&sa->fsa_ax25.sax25_call);

    // Send RPC.
    ax25ms::SeqConnectRequest req;
    req.mutable_packet()->mutable_metadata()->mutable_source_address()->set_address(src_);
    req.mutable_packet()->mutable_metadata()->mutable_address()->set_address(dst);
    req.mutable_packet()->mutable_metadata()->mutable_connection_settings()->set_extended(
        extseq_);
    stream_ = router()->Connect(&stream_ctx_);
    if (!stream_->Write(req)) {
        std::clog << "Failed to start connect RPC: " << stream_->Finish().error_message()
                  << "\n";
        errno = ECOMM;
        return -1;
    }

    // Wait for the connection metadata.
    ax25ms::SeqConnectResponse resp;
    bool ok = stream_->Read(&resp);
    std::unique_lock<std::mutex> lk(read_queue_mu_);
    read_ready_ = true;
    read_queue_cv_.notify_one();
    if (!ok) {
        stream_ = nullptr;
        errno = ECONNREFUSED;
        return -1;
    }
    return 0;
}

} // namespace

/*********************
 * Library overrides
 */

extern "C" {

int ax25_config_load_ports(void)
{
    // One port.
    return 1;
}

int ax25_config_get_paclen(char*)
{
    return 200; // TODO;
}

int ax25_config_get_window(char*)
{
    return 3; // TODO;
}

char* ax25_config_get_port(ax25_address*)
{
    return const_cast<char*>("radio"); // TODO;
}

char* ax25_config_get_dev(char*)
{
    return const_cast<char*>("radio"); // TODO;
}

char* ax25_config_get_next(char* p)
{
    if (p) {
        return nullptr;
    }
    return const_cast<char*>("radio"); // TODO;
}

// WARNING: not reentrant.
char* ax25_config_get_desc(char*)
{
    static char buf[1024] = { 0 };
    snprintf(buf,
             sizeof(buf),
             "ax25ms LD_PRELOAD port using radio %s, router %s",
             radio_addr,
             router_addr);
    return buf;
}

int ax25_config_get_baud(char*) { return 1200; }

char* ax25_config_get_addr(char*) { return radio_addr; }

int socket(int domain, int type, int protocol)
{
    if (domain != AF_AX25) {
        assert(orig_socket);
        return orig_socket(domain, type, protocol);
    }
    log() << "socket(AF_AX25)\n";
    auto con = std::make_unique<Connection>(type, protocol);
    const auto fd = con->fd();
    if (!connections.insert(std::move(con))) {
        log() << "Failed to insert new connection into connection map";
        errno = EBADFD;
        return -1;
    }
    return fd;
}

int close(int fd)
{
    if (!connections.get(fd)) {
        assert(orig_close);
        return orig_close(fd);
    }
    log() << "close(AF_AX25)\n";

    connections.extract(fd);
    return 0;
}

int bind(int fd, const struct sockaddr* addr, socklen_t addrlen)
{
    auto con = connections.get(fd);
    if (!con) {
        assert(orig_bind);
        return orig_bind(fd, addr, addrlen);
    }
    log() << "bind(AF_AX25)\n";
    return con->bind(addr, addrlen);
}

int connect(int fd, const struct sockaddr* addr, socklen_t addrlen)
{
    auto con = connections.get(fd);
    if (!con) {
        assert(orig_connect);
        return orig_connect(fd, addr, addrlen);
    }
    log() << "connect(AF_AX25)\n";
    return con->connect(addr, addrlen);
}

ssize_t read(int fd, void* buf, size_t count)
{
    auto con = connections.get(fd);
    if (!con) {
        assert(orig_read);
        return orig_read(fd, buf, count);
    }
    log() << "read(AF_AX25,, " << count << ")\n";
    const auto rc = con->read(buf, count);
    log() << "… read(AF_AX25) = " << rc << "\n";
    return rc;
}

ssize_t write(int fd, const void* buf, size_t count)
{
    auto con = connections.get(fd);
    if (!con) {
        assert(orig_write);
        return orig_write(fd, buf, count);
    }
    log() << "write(AF_AX25, , " << count << ")\n";
    return con->write(buf, count);
}

int getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen)
{
    auto con = connections.get(fd);
    if (!con) {
        assert(orig_getsockopt);
        return orig_getsockopt(fd, level, optname, optval, optlen);
    }
    log() << "getsockopt(AF_AX25)\n";
    return con->getsockopt(level, optname, optval, optlen);
}

int setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen)
{
    auto con = connections.get(fd);
    if (!con) {
        assert(orig_setsockopt);
        return orig_setsockopt(fd, level, optname, optval, optlen);
    }
    // log() << "setsockopt(AF_AX25)\n";
    return con->setsockopt(level, optname, optval, optlen);
}
} // extern "C"
