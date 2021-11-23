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
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <utility>

namespace {

typedef int (*socket_func_t)(int, int, int);
typedef ssize_t (*read_func_t)(int, void*, size_t);
typedef ssize_t (*write_func_t)(int, const void*, size_t);
typedef int (*bind_func_t)(int, const struct sockaddr*, socklen_t);
typedef int (*connect_func_t)(int, const struct sockaddr*, socklen_t);
typedef int (*getsockopt_func_t)(int, int, int, void*, socklen_t*);
typedef int (*setsockopt_func_t)(int, int, int, const void*, socklen_t);
typedef int (*close_func_t)(int);

static socket_func_t orig_socket = nullptr;
static read_func_t orig_read = nullptr;
static write_func_t orig_write = nullptr;
static bind_func_t orig_bind = nullptr;
static connect_func_t orig_connect = nullptr;
static getsockopt_func_t orig_getsockopt = nullptr;
static setsockopt_func_t orig_setsockopt = nullptr;
static close_func_t orig_close = nullptr;

const char* log_prefix = "preload: ";

char* radio_addr = nullptr;  // AX25_ADDR
char* router_addr = nullptr; // AX25_ROUTER

std::ostream& log()
{
    if (true) {
        std::clog << log_prefix;
        return std::clog;
    }
    static std::ofstream null("/dev/null");
    return null;
}

class DummyFD
{
public:
    DummyFD(int fd) : fd_(fd) {}

    // No copy.
    DummyFD(const DummyFD&) = delete;
    DummyFD& operator=(const DummyFD&) = delete;

    // Move ok.
    DummyFD(DummyFD&& rhs) { fd_ = std::exchange(rhs.fd_, -1); }
    DummyFD& operator=(DummyFD&& rhs)
    {
        fd_ = std::exchange(rhs.fd_, -1);
        return *this;
    }
    ~DummyFD()
    {
        if (fd_ >= 0) {
            orig_close(fd_);
            fd_ = -1;
        }
    }
    int fd() const noexcept { return fd_; }

private:
    int fd_;
};


class Connection
{
public:
    Connection(int type, int protocol);

    Connection(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection& operator=(Connection&&) = delete;

    int fd() const noexcept { return dummy_fd_.fd(); }
    ssize_t read(void* buf, size_t count);
    ssize_t write(const void* buf, size_t count);
    int getsockopt(int level, int optname, void* optval, socklen_t* optlen);
    int setsockopt(int level, int optname, const void* optval, socklen_t optlen);
    int bind(const struct sockaddr* addr, socklen_t addrlen);
    int connect(const struct sockaddr* addr, socklen_t addrlen);

private:
    DummyFD dummy_fd_;
    int type_;
    int protocol_;
    bool extseq_ = false;
    int paclen_ = 256; // TODO
    grpc::ClientContext stream_ctx_;

    std::unique_ptr<
        grpc::ClientReaderWriter<ax25ms::SeqConnectRequest, ax25ms::SeqConnectResponse>>
        stream_;

    std::string src_;
};

std::mutex mu;
std::map<int, std::unique_ptr<Connection>> connections;
Connection* get_connection(int fd)
{
    std::unique_lock<std::mutex> lk(mu);
    auto itr = connections.find(fd);
    if (itr == connections.end()) {
        return nullptr;
    }
    return itr->second.get();
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
}


int make_fd()
{
    // Create a dummy fd just to reserve the fd.
    int fd[2];
    if (-1 == pipe(fd)) {
        throw std::runtime_error(std::string("pipe(): ") + strerror(errno));
    }
    orig_close(fd[1]);
    return fd[0];
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
    : dummy_fd_(make_fd()), type_(type), protocol_(protocol)
{
}

ssize_t Connection::read(void* buf, size_t count)
{
    for (;;) {
        ax25ms::SeqConnectResponse resp;
        assert(stream_);
        if (!stream_->Read(&resp)) {
            log() << "stream ended\n";
            // TODO: Finish() and stuff.
            return 0;
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
    errno = ENOSYS;
    return -1;
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
    }

    errno = EINVAL;
    return -1;
}

int Connection::setsockopt(int level, int optname, const void* optval, socklen_t optlen)
{
    if (level != SOL_AX25) {
        errno = ENOSYS;
        return -1;
    }

    switch (optname) {
    case AX25_EXTSEQ:
        // TODO: set it.
        return 0;
    case AX25_PACLEN:
        // TODO: set it.
        return 0;
    }

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
    stream_ = router()->Connect(&stream_ctx_);
    if (!stream_->Write(req)) {
        std::clog << "Failed to start connect RPC: " << stream_->Finish().error_message()
                  << "\n";
        errno = ECOMM;
        return -1;
    }

    // Wait for the connection metadata.
    ax25ms::SeqConnectResponse resp;
    if (!stream_->Read(&resp)) {
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

char* ax25_config_get_addr(char*) { return radio_addr; }

int socket(int domain, int type, int protocol)
{
    if (domain != AF_AX25) {
        assert(orig_socket);
        return orig_socket(domain, type, protocol);
    }
    log() << "socket(AF_AX25)\n";
    std::unique_lock<std::mutex> lk(mu);
    auto con = std::make_unique<Connection>(type, protocol);
    const auto fd = con->fd();
    connections.insert({ fd, std::move(con) });
    return fd;
}

int close(int fd)
{
    if (!get_connection(fd)) {
        assert(orig_close);
        return orig_close(fd);
    }
    log() << "close(AF_AX25)\n";
    std::unique_lock<std::mutex> lk(mu);
    connections.erase(fd);
    return 0;
}

int bind(int fd, const struct sockaddr* addr, socklen_t addrlen)
{
    auto con = get_connection(fd);
    if (!con) {
        assert(orig_bind);
        return orig_bind(fd, addr, addrlen);
    }
    log() << "bind(AF_AX25)\n";
    return con->bind(addr, addrlen);
}

int connect(int fd, const struct sockaddr* addr, socklen_t addrlen)
{
    auto con = get_connection(fd);
    if (!con) {
        assert(orig_connect);
        return orig_connect(fd, addr, addrlen);
    }
    log() << "connect(AF_AX25)\n";
    return con->connect(addr, addrlen);
}

ssize_t read(int fd, void* buf, size_t count)
{
    auto con = get_connection(fd);
    if (!con) {
        assert(orig_read);
        return orig_read(fd, buf, count);
    }
    log() << "read(AF_AX25)\n";
    return con->read(buf, count);
}

ssize_t write(int fd, const void* buf, size_t count)
{
    auto con = get_connection(fd);
    if (!con) {
        assert(orig_write);
        return orig_write(fd, buf, count);
    }
    log() << "write(AF_AX25)\n";
    return con->write(buf, count);
}

int getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen)
{
    auto con = get_connection(fd);
    if (!con) {
        assert(orig_getsockopt);
        return orig_getsockopt(fd, level, optname, optval, optlen);
    }
    log() << "getsockopt(AF_AX25)\n";
    return con->getsockopt(level, optname, optval, optlen);
}

int setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen)
{
    auto con = get_connection(fd);
    if (!con) {
        assert(orig_setsockopt);
        return orig_setsockopt(fd, level, optname, optval, optlen);
    }
    log() << "setsockopt(AF_AX25)\n";
    return con->setsockopt(level, optname, optval, optlen);
}
} // extern "C"
