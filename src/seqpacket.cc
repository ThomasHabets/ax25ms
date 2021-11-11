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
#include "parse.h"
#include "scheduler.h"
#include "serialize.h"

#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"

// System headers.
#include <fcntl.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Third party.
#include <google/protobuf/text_format.h>
#include <grpcpp/grpcpp.h>

namespace {
// My compiler doesn't have std::binary_semaphore

class binary_semaphore
{
public:
    binary_semaphore()
    {
        if (sem_init(&sem_, 0, 1)) {
            throw std::runtime_error("failed to sem_init()");
        }
    }
    ~binary_semaphore() { sem_destroy(&sem_); }
    void acquire()
    {
        if (sem_wait(&sem_)) {
            throw std::runtime_error("failed to sem_wait()");
        }
    }
    void release()
    {
        if (sem_post(&sem_)) {
            throw std::runtime_error("failed to sem_post()");
        }
    }
    binary_semaphore(const binary_semaphore&) = delete;
    binary_semaphore(binary_semaphore&&) = delete;
    binary_semaphore& operator=(const binary_semaphore&) = delete;
    binary_semaphore& operator=(binary_semaphore&&) = delete;

private:
    sem_t sem_;
};

} // namespace

namespace ax25ms {

constexpr auto immediately = std::chrono::milliseconds{ 0 };
// Defaults.

// 3000 according to 6.3.2. 10s in Linux.
constexpr Timer::duration_t default_t1 = std::chrono::milliseconds{ 3000 };

// Linux: 3s.
constexpr Timer::duration_t default_t2 = std::chrono::seconds{ 3 };

// Linux: 300s.
constexpr Timer::duration_t default_t3 = std::chrono::seconds{ 300 };

// Linux: 20m.
constexpr Timer::duration_t default_idle_timeout = std::chrono::minutes{ 20 };

// Linux: default 256
constexpr int minimum_packet_length = 1;
constexpr int maximum_packet_length = 512;
constexpr int default_maximum_packet_length = 256;

// 1-7. Spec: 4. Linux: 2
constexpr int default_window_size_standard = 2;

// 1-63. Spec: 32. Linux: 32
constexpr int default_window_size_extended = 32;

// Retries.
// Spec & Linux: 10.
constexpr int default_n2 = 3;

// int modulo = 8;
// int window_size = 7;
// int retries = 10;
// Although see https://tldp.org/HOWTO/AX25-HOWTO/x235.html, where linux defaults
// to: T1: 10s t2: 3s t3: 300s window: 2

namespace {
std::string proto2string(const google::protobuf::Message& msg)
{
    std::string str;
    google::protobuf::TextFormat::PrintToString(msg, &str);
    return str;
}
} // namespace

class Connection
{
public:
    Connection(std::string_view mycall,
               std::string_view peer,
               ax25ms::RouterService::Stub* router,
               Timer* scheduler)
        : router_(router), scheduler_(scheduler), mycall_(mycall), peer_(peer)
    {
    }
    ~Connection() {}
    enum class State {
        IDLE = 0,
        CONNECTING = 1,
        CONNECTED = 2,
    };


    // No copy.
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Move ok.
    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    struct Entry {
        ax25::Packet packet;
        Timer::time_point_t next_tx;
    };

    std::mutex send_mu_;
    std::deque<Entry> send_queue_;

    grpc::Status write(std::string_view payload);

    /*
     * A packet has been added, or a timer has expired.
     * Do something, such as re-send.
     */
    void maybe_send()
    {
        // If there are unsent packets, send them.
        std::unique_lock<std::mutex> lk(send_mu_);
        int unacked = 0;
        for (auto& e : send_queue_) {
            if (unacked++ > window_size_) {
                break;
            }
            if (std::chrono::steady_clock::now() < e.next_tx) {
                continue;
            }
            e.packet.mutable_iframe()->set_nr(nr_);
            const auto data =
                ax25::serialize(e.packet, true, modulus_ == extended_modulus);

            ax25ms::SendRequest sreq;
            sreq.mutable_frame()->set_payload(data);
            std::unique_lock<std::mutex> lk(mu_);
            ax25ms::SendResponse resp;
            grpc::ClientContext ctx;
            const auto status = router_->Send(&ctx, sreq, &resp);
            if (!status.ok()) {
                std::cerr << "  Sending data failed\n";
                change_state(State::CONNECTED, State::IDLE);
                send_queue_.clear();
            } else {
                e.next_tx = std::chrono::steady_clock::now() + default_t1;
            }
        }
        if (!send_queue_.empty()) {
            scheduler_->add(send_queue_.front().next_tx, [this] { maybe_send(); });
        }
    }

    grpc::Status disconnect()
    {
        ax25::Packet packet;
        packet.set_src(mycall_);
        packet.set_dst(peer_);
        packet.mutable_disc()->set_poll(true);
        const auto data = ax25::serialize(packet, true, modulus_ == extended_modulus);
        ax25ms::SendRequest sreq;
        sreq.mutable_frame()->set_payload(data);
        ax25ms::SendResponse resp;
        grpc::ClientContext ctx;
        const auto status = router_->Send(&ctx, sreq, &resp);
        if (!status.ok()) {
            std::cerr << "  Sending DISC failed\n";
            change_state(State::CONNECTED, State::IDLE);
            return status;
        }
        return grpc::Status::OK;
    }

    // Can not call concurrently.
    grpc::Status connect(grpc::ServerContext* ctx)
    {
        assert(state_ == State::IDLE);

        state_ = State::CONNECTING;

        ax25::Packet packet;
        packet.set_src(mycall_);
        packet.set_dst(peer_);
        packet.mutable_sabm()->set_poll(true);
        const auto data = ax25::serialize(packet, true, modulus_ == extended_modulus);

        ax25ms::SendRequest sreq;
        sreq.mutable_frame()->set_payload(data);
        std::unique_lock<std::mutex> lk(mu_);
        scheduler_->add(immediately, [this, ctx, &sreq] { connect_send(ctx, sreq); });
        std::clog << "Awaiting state change\n";
        state_cv_.wait(lk, [this] {
            std::clog << "State change? " << int(state_) << "\n";
            return state_ != State::CONNECTING;
        });
        std::clog << "got state change\n";
        if (state_ == State::CONNECTED) {
            return grpc::Status::OK;
        }
        return grpc::Status(grpc::UNKNOWN, "connection timed out");
    }

    void connect_send(grpc::ServerContext* ctx, ax25ms::SendRequest& sreq, int retry = 0)
    {
        if (retry >= default_n2 || ctx->IsCancelled()) {
            change_state(State::CONNECTING, State::IDLE);
            return;
        }
        std::cerr << "Sending SABM…\n";
        // Send SABM.
        ax25ms::SendResponse resp;
        const auto status = router_->Send(
            grpc::ClientContext::FromServerContext(*ctx).get(), sreq, &resp);
        if (!status.ok()) {
            std::cerr << "  Sending SABM failed\n";
            change_state(State::CONNECTING, State::IDLE);
            return;
        }
        std::cerr << "  Sending SABM succeeded\n";

        scheduler_->add(default_t1, [this, ctx, &sreq, retry] {
            {
                std::unique_lock<std::mutex> lk(mu_);
                if (state_ != State::CONNECTING) {
                    return;
                }
            }
            connect_send(ctx, sreq, retry + 1);
        });
    }

    bool change_state(State from, State to)
    {
        std::unique_lock<std::mutex> lk(mu_);
        // std::clog << "Changing state from " << int(state_) << " to " << int(to) << "
        // currently " << int(state_) << "\n";
        if (state_ != from) {
            return false;
        }
        state_ = to;
        state_cv_.notify_all();
        return true;
    }

    void ua(const ax25::Packet& packet)
    {
        std::clog << "Received UA\n";
        change_state(State::CONNECTING, State::CONNECTED);
        // TODO: what about other states?
    }

    void iframe(const ax25::Packet& packet) { std::clog << "iframe\n"; }

    void disc(const ax25::Packet& packet) { std::clog << "disc\n"; }

    void receive(ax25::Packet& packet)
    {
        // Remove acked packets from send queue.
        {
            std::unique_lock<std::mutex> lk(send_mu_);
            while (!send_queue_.empty()) {
                const auto ns = send_queue_.front().packet.iframe().ns();
                if (ns >= packet.iframe().nr()) {
                    break;
                }
                std::clog << "Packet acked with sequence " << ns << "\n";
                send_queue_.pop_front();
            }
        }
        // TODO: schedule a RR.
    }

private:
    bool connected_ = false;
    ax25ms::RouterService::Stub* router_;
    Timer* scheduler_;
    std::string mycall_;
    std::string peer_;

    std::mutex mu_;
    std::condition_variable state_cv_; // Trigger when state changes.
    State state_ = State::IDLE;

    static constexpr int normal_modulus = 8;
    static constexpr int extended_modulus = 128;

    const int modulus_ = normal_modulus;
    int window_size_ = 4; // TODO: what to default to?
    int nr_ = 0;
    int ns_ = 0;
    ax25ms::SeqMetadata metadata_;
};

grpc::Status Connection::write(std::string_view payload)
{
    ax25::Packet packet;
    packet.set_src(mycall_);
    packet.set_dst(peer_);
    auto iframe = packet.mutable_iframe();
    iframe->set_extended(modulus_ == extended_modulus);
    iframe->set_payload("\xF0" + std::string(payload));

    std::unique_lock<std::mutex> lk(send_mu_);
    packet.mutable_iframe()->set_ns(ns_++);
    ns_ = (ns_ + 1) % modulus_;
    send_queue_.push_back(Entry{
        .packet = std::move(packet),
        .next_tx = std::chrono::steady_clock::now(),
    });
    scheduler_->add(immediately, [this] { maybe_send(); });
    return grpc::Status::OK;
}


class SeqPacketImpl final : public ax25ms::SeqPacketService::Service
{
public:
    SeqPacketImpl(ax25ms::RouterService::Stub* router) : router_(router) {}
    void packet_callback(const ax25ms::Frame& frame)
    {
        const auto [packet, status] = ax25::parse(frame.payload());
        if (!status.ok()) {
            std::cerr << "Parse error for frame: " << status.error_message() << "\n";
            return;
        }

        // TODO: slightly cleaner to not do lookup on packets that are not seqpackets.
        auto connitr = connections_.find({ packet.dst(), packet.src() });
        if (connitr == connections_.end()) {
            std::cerr << "Unknown connection\n";
            return;
        }
        auto& conn = connitr->second;
        if (packet.has_ua()) {
            conn->ua(packet);
        } else if (packet.has_iframe()) {
            conn->iframe(packet);
        } else if (packet.has_disc()) {
            conn->disc(packet);
        } else {
            // Not a seqpacket-related frame.
            std::cerr << "Not a seqpacket-related frame:\n"
                      << proto2string(packet) << "\n";
            return;
        }
        std::cerr << "Got seqpacket frame of size " << frame.payload().size() << "\n";

        // TODO: conn->();
        //  find the connection this is for.
    }

    grpc::Status
    Connect(grpc::ServerContext* ctx,
            grpc::ServerReaderWriter<ax25ms::SeqConnectResponse,
                                     ax25ms::SeqConnectRequest>* stream) override
    {
        try {
            return Connect2(ctx, stream);
        } catch (const std::exception& e) {
            std::cerr << "Connect() exception: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "Connect() unknown exception\n";
        }
        return grpc::Status(grpc::INTERNAL, "exception");
    }
    grpc::Status Connect2(grpc::ServerContext* ctx,
                          grpc::ServerReaderWriter<ax25ms::SeqConnectResponse,
                                                   ax25ms::SeqConnectRequest>* stream)
    {
        std::cout << "Starting a new connection\n";
        ax25ms::SeqConnectRequest req;
        if (!stream->Read(&req)) {
            return grpc::Status(grpc::INVALID_ARGUMENT, "no initial data received");
        }
        std::cerr << proto2string(req) << "\n";
        if (!req.has_packet()) {
            return grpc::Status(grpc::INVALID_ARGUMENT, "initial burst had no packet");
        }
        if (!req.packet().has_metadata()) {
            return grpc::Status(grpc::INVALID_ARGUMENT, "initial burst had no metadata");
        }
        const auto dst = req.packet().metadata().address().address();
        const auto src = req.packet().metadata().source_address().address();

        auto conu = std::make_unique<Connection>(src, dst, router_, &scheduler_);
        auto& con = *conu;
        connections_[{ src, dst }] = std::move(conu);
        const auto status = con.connect(ctx);
        if (!status.ok()) {
            std::clog << "Connection timed out, returning error on stream\n";
            return grpc::Status(grpc::CANCELLED, "connection timed out");
        }
        std::clog << "Connection established!\n";
        con.write("id");
        while (stream->Read(&req)) {
            std::clog << "client sent something!\n";
        }
        std::clog << "Connection ended\n";
        return con.disconnect();
        return grpc::Status::OK;
    }

private:
    Timer scheduler_;
    ax25ms::RouterService::Stub* router_;
    std::map<std::pair<std::string, std::string>, std::unique_ptr<Connection>>
        connections_;
};


} // namespace ax25ms

namespace {
[[noreturn]] void usage(const char* av0, int err)
{
    std::cout << av0 << ": Usage [ -h ] [ -l <listen address> ] -r <router host:port>\n";
    exit(err);
}
} // namespace

int main(int argc, char** argv)
{
    std::string router;
    std::string listen = "[::]:12346";
    {
        int opt;
        while ((opt = getopt(argc, argv, "hr:l:")) != -1) {
            switch (opt) {
            case 'r':
                router = optarg;
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
    if (router.empty()) {
        std::cerr << "Need to specify router (-r)\n";
        return EXIT_FAILURE;
    }
    if (optind != argc) {
        std::cerr << "Invalid extra args on the command line\n";
        return EXIT_FAILURE;
    }

    // Connect to router.
    auto channel = grpc::CreateChannel(router, grpc::InsecureChannelCredentials());
    std::unique_ptr<ax25ms::RouterService::Stub> stub{ ax25ms::RouterService::NewStub(
        channel) };

    // Send SABM
    if (false) {
        grpc::ClientContext ctx;
        ax25::Packet packet;
        packet.set_src("M0THC-1");
        packet.set_dst("M0THC-2");
        if (true) {
            packet.mutable_sabm()->set_poll(true);
        } else {
            packet.mutable_disc()->set_poll(true);
        }
        auto data = ax25::serialize(packet, true, false);


        auto parsed = ax25::parse(data);
        std::string str;
        google::protobuf::TextFormat::PrintToString(packet, &str);
        std::cout << str;

        ax25ms::SendRequest req;
        req.mutable_frame()->set_payload(data);
        ax25ms::SendResponse resp;
        auto status = stub->Send(&ctx, req, &resp);
        if (!status.ok()) {
            std::cerr << "Failed to send: " << status.error_message() << "\n";
        } else {
            std::cout << "Successfully sent a packet\n";
        }
    }

    // Start up server.
    std::cout << "Starting up a server" << std::endl;
    ax25ms::SeqPacketImpl service(stub.get());
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "Server started" << std::endl;

    // Start stream
    for (;;) {
        grpc::ClientContext ctx;
        ax25ms::StreamRequest req;
        auto reader = stub->StreamFrames(&ctx, req);
        ax25ms::Frame frame;
        while (reader->Read(&frame)) {
            service.packet_callback(frame);
        }
        const auto status = reader->Finish();
        if (!status.ok()) {
            std::cerr << "Stream ended with error\n";
        }
        std::cerr << "Stream ended. Looping\n";
    }
    // server->Wait();
}
