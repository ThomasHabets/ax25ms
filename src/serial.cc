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
#include "fdwrap.h"
#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"
#include <condition_variable>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <iomanip>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

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
        std::vector<char> buf;
        enum class KISSState {
            UNKNOWN,
            READING,
        };
        constexpr uint8_t FEND = 0xC0;
        constexpr uint8_t FESC = 0xDB;
        constexpr uint8_t TFEND = 0xDC;
        constexpr uint8_t TFESC = 0xDD;
        KISSState state = KISSState::UNKNOWN;
        std::vector<char> current;
        for (;;) {
            std::vector<char>::iterator fend;
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

            if (false) {
                std::clog << "KISS stream has FESC:\n";
                for (const auto ch : buf) {
                    std::clog << std::hex << static_cast<unsigned int>(ch) << " ";
                }
                std::clog << "\n";
            }

            // If unsynced, clear until first FEND.
            if (state == KISSState::UNKNOWN) {
                state = KISSState::READING;
                buf.erase(buf.begin(), ++fend);

            } else if (state == KISSState::READING) {
                // If reading, consume
                auto pos = buf.begin();
                for (; pos < buf.end(); pos++) {
                    if (*pos == FESC) {
                        pos++;
                        if (*pos == TFEND) {
                            current.push_back(FEND);
                        } else if (*pos == TFESC) {
                            current.push_back(FESC);
                        } else {
                            std::cerr << "Bad KISS escape\n";
                            state = KISSState::UNKNOWN;
                            current.clear();
                            break;
                        }
                    } else if (*pos == FEND) {
                        ++pos;
                        if (current.empty()) {
                            break;
                        }
                        switch (current[0]) {
                        case 0x00: // Data
                            if (current.size() > 1) {
                                ax25ms::Frame frame{};
                                frame.set_payload(current.data() + 1, current.size());
                                inject(frame);
                            }
                            break;
                        default:
                            std::clog << "Unknown kiss frame " << std::hex
                                      << static_cast<int>(current[0]) << "\n";
                        }
                        current.clear();
                        break;
                    } else {
                        current.push_back(*pos);
                    }
                }
                buf.erase(buf.begin(), pos);
            }
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
            r.append(req->frame().payload());
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

    std::thread reader_;
    std::thread writer_;

    std::mutex mu_;
    std::vector<std::weak_ptr<Queue>> queues_;
};

FDWrap open_serial(const std::string& port)
{
    FDWrap ret(open(port.c_str(), O_RDWR | O_NOCTTY));
    return ret;
}

int main(int argc, char** argv)
{
    const std::string port = argv[1];
    AX25Serial service(open_serial(port));

    const std::string addr("[::]:12345");
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

    server->Wait();
}
