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
/*
 * Tool
 *
 * Relay messages between a KISS TNC and the GRPC interface.
 * Implements the Router service.
 */
#include "fdwrap.h"
#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"
#include "util.h"
#include <condition_variable>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <cstdint>
#include <iomanip>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace ax25ms::serial {
constexpr uint8_t FEND = 0xC0;
constexpr uint8_t FESC = 0xDB;
constexpr uint8_t TFEND = 0xDC;
constexpr uint8_t TFESC = 0xDD;


[[noreturn]] void usage(const char* av0, int err)
{
    std::cout << av0 << ": Usage [ -h ] [ -l <listen addr> -p </dev/...>\n";
    exit(err);
}

std::string kiss_escape(std::string_view sv)
{
    std::vector<uint8_t> tmp;
    for (const auto& ch : sv) {
        const auto i8 = static_cast<uint8_t>(ch);
        switch (i8) {
        case FESC:
            tmp.push_back(FESC);
            tmp.push_back(TFESC);
            break;
        case FEND:
            tmp.push_back(FESC);
            tmp.push_back(TFEND);
            break;
        default:
            tmp.push_back(i8);
        }
    }
    return std::string(tmp.begin(), tmp.end());
}

std::string kiss_unescape(std::string_view sv)
{
    std::vector<uint8_t> tmp;
    for (auto itr = sv.begin(); itr != sv.end(); ++itr) {
        const auto i8 = static_cast<uint8_t>(*itr);

        if (i8 == FEND) {
            // TODO: log error
            return "";
        }

        if (itr + 1 == sv.end()) {
            if (i8 == FESC) {
                // TODO: log esc error.
                return "";
            }
            tmp.push_back(i8);
            continue;
        }

        if (i8 != FESC) {
            tmp.push_back(i8);
            continue;
        }

        itr++;
        const auto i8e = static_cast<uint8_t>(*itr);

        switch (i8e) {
        case TFESC:
            tmp.push_back(FESC);
            break;
        case TFEND:
            tmp.push_back(FEND);
            break;
        default:
            throw std::runtime_error("INVALID Escape");
        }
    }
    auto ret = std::string(tmp.begin(), tmp.end());
    return ret;
}

class Queue
{
public:
    std::shared_ptr<ax25ms::Frame> pop()
    {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return !queue_.empty(); });
        auto ret = queue_.front();
        queue_.pop();
        return ret;
    }
    void push(std::shared_ptr<ax25ms::Frame> f)
    {
        std::unique_lock<std::mutex> lk(mu_);
        queue_.push(std::move(f));
        cv_.notify_one();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::shared_ptr<ax25ms::Frame>> queue_;
};

class AX25Serial final : public ax25ms::RouterService::Service
{
public:
    AX25Serial(FDWrap&& serial)
        : serial_(std::move(serial)), reader_([this] { read_main(); })
    {
        // TODO: remove. This is debugging.
        std::clog << "Serial starting...\n";
    }

    void read_main()
    {
        std::vector<uint8_t> buf;
        enum class KISSState {
            UNKNOWN,
            READING,
        };
        KISSState state = KISSState::UNKNOWN;
        // std::vector<uint8_t> current;
        for (;;) {
            std::vector<uint8_t>::iterator fend;

            // Read until we have a FEND.
            for (;;) {
                fend = std::find(buf.begin(), buf.end(), FEND);
                if (fend != buf.end()) {
                    break;
                }
                std::array<char, 1024> tbuf;
                const auto rc = ::read(serial_.get(), tbuf.data(), tbuf.size());
                if (rc == -1) {
                    // TODO: try to reopen until success.
                    throw std::runtime_error(std::string("failed to read from serial: ") +
                                             strerror(errno));
                }
                buf.insert(buf.end(), &tbuf.data()[0], &tbuf.data()[rc]);
                if (buf.empty()) {
                    continue;
                }
            }

            if (state == KISSState::READING) {
                const auto data = kiss_unescape(std::string(buf.begin(), fend));
                if (!data.empty()) {
                    if (data[0] == 0) { // Data
                        ax25ms::Frame frame{};
                        frame.set_payload(data.data() + 1, data.size() - 1);
                        inject(frame);
                    } else {
                        std::cerr << "TODO: got frame with some L3 stuff\n";
                    }
                }
            }

            state = KISSState::READING; // We've seen a FEND, so now in sync.
            buf.erase(buf.begin(), ++fend);
        }
    }

    grpc::Status StreamFrames(grpc::ServerContext* ctx,
                              const ax25ms::StreamRequest* req,
                              grpc::ServerWriter<ax25ms::Frame>* writer) override
    {
        try {
            std::clog << "Registering listener\n";
            auto q = std::make_shared<Queue>();
            {
                std::lock_guard<std::mutex> l(mu_);
                queues_.push_back(q);
            }
            for (;;) {
                auto frame = q->pop(); // TODO: also wait for stream to end.
                std::clog << "  Sending to client\n";
                if (!writer->Write(*frame)) {
                    break;
                }
            }
            std::cerr << "Stream stop\n";
            return grpc::Status::OK;
        } catch (const std::exception& e) {
            std::cerr << "Exception: " << e.what() << "\n";
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, e.what());
        } catch (...) {
            std::cerr << "Unknown Exception\n";
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "unknown exception");
        }
    }

    grpc::Status Send(grpc::ServerContext* ctx,
                      const ax25ms::SendRequest* req,
                      ax25ms::SendResponse* out) override
    {
        const std::string data = [req] {
            std::string r = "\xC0";
            r.push_back(0);
            r.append(kiss_escape(req->frame().payload()));
            r.push_back('\xC0');
            return r;
        }();
        auto p = data.data();
        auto size = data.size();
        std::unique_lock<std::mutex> lk(serial_mu_);
        std::cerr << "Sending packet of size " << size << "\n";
        for (auto ch : data) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << (static_cast<unsigned int>(ch) & 0xff) << " ";
        }
        std::cout << "\n";
        while (size > 0) {
            const auto rc = ::write(serial_.get(), p, size);
            if (rc == -1) {
                throw std::runtime_error(std::string("write() to serial failed: ") +
                                         strerror(errno));
            }
            size -= rc;
            p += rc;
        }
        std::cerr << "Packet sent\n";
        return grpc::Status::OK;
    }

private:
    void inject(ax25ms::Frame f)
    {
        std::clog << "Injecting with size " << f.payload().size() << "\n";
        std::lock_guard<std::mutex> l(mu_);
        for (auto w = queues_.begin(); w != queues_.end();) {
            auto s = w->lock();
            if (!s) {
                std::clog << "  Dead entry\n";
                w = queues_.erase(w);
                continue;
            }
            std::clog << "  Added to queue\n";
            s->push(std::make_shared<ax25ms::Frame>(f));
            ++w;
        }
    }

    std::mutex serial_mu_;
    FDWrap serial_;


    std::mutex mu_;
    std::vector<std::weak_ptr<Queue>> queues_;

    // Put threads last to make sure other members are initialized.
    std::jthread reader_;
    std::jthread writer_;
};

FDWrap open_serial(const std::string& port)
{
    const int fd = open(port.c_str(), O_RDWR | O_NOCTTY);
    if (fd == -1) {
        throw std::runtime_error("failed to open " + port + ": " + strerror(errno));
    }
    FDWrap ret(fd);

    // Set exclusive mode.
    if (ioctl(fd, TIOCEXCL, nullptr, 0)) {
        throw std::runtime_error("failed to set TIOCEXCL for " + port + ": " +
                                 strerror(errno));
    }

    // Set serial attributes.
    struct termios tc;
    if (tcgetattr(fd, &tc)) {
        throw std::runtime_error("failed to get termios for " + port + ": " +
                                 strerror(errno));
    }
    // cfsetospeed(&tc,B57600);
    tc.c_cc[VMIN] = 0;
    tc.c_cc[VTIME] = 0;
    cfmakeraw(&tc);
    tc.c_cflag &= ~CRTSCTS; // Turn off hardware flow control.
    if (tcsetattr(fd, TCSANOW, &tc)) {
        throw std::runtime_error("failed to set termios for " + port + ": " +
                                 strerror(errno));
    }
    return ret;
}
} // namespace ax25ms::serial

int wrapmain(int argc, char** argv)
{
    using namespace ax25ms::serial;

    std::string port;
    std::string listen = "[::]:12345";
    {
        int opt;
        while ((opt = getopt(argc, argv, "hl:p:")) != -1) {
            switch (opt) {
            case 'p':
                port = optarg;
                break;
            case 'l':
                listen = optarg;
                break;
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
            default:
                usage(argv[0], EXIT_FAILURE);
            }
        }
    }
    if (port.empty()) {
        std::cerr << "Need to specify serial port (-p)\n";
        return EXIT_FAILURE;
    }
    if (optind != argc) {
        std::cerr << "Invalid extra args on the command line\n";
        return EXIT_FAILURE;
    }

    AX25Serial service(open_serial(port));

    const std::string addr(listen);
    grpc::ServerBuilder builder;
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 500);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 1000);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_BDP_PROBE, 1);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
                               500);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS,
                               1000);
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::clog << "Running…\n";
    server->Wait();
    return 0;
}
