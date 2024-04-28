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
#include "seqpacket.h"
#include "seqpacket_con.h"

#include "fdwrap.h"
#include "parse.h"
#include "scheduler.h"
#include "serialize.h"
#include "util.h"

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
#include <grpcpp/grpcpp.h>

namespace {

// Default: FCS neither in nor out.
// Change with -f <in|out|both>
bool fcs_in = false;
bool fcs_out = false;

template <typename Func>
class Defer
{
public:
    Defer(Func&& func) : func_(std::move(func)) {}

    // No move of copy.
    Defer(const Defer&) = delete;
    Defer(Defer&&) = delete;
    Defer& operator=(const Defer&) = delete;
    Defer& operator=(Defer&&) = delete;

    ~Defer() { func_(); }

private:
    Func func_;
};
} // namespace

using seqpacket::con::Connection;


namespace ax25ms {

constexpr auto immediately = std::chrono::milliseconds{ 0 };
// Defaults.

// 3000 according to 6.3.2. 10s in Linux.
// How long to wait before retransmitting an unacked frame.
constexpr Timer::duration_t default_t1 = std::chrono::milliseconds{ 5000 };

// Linux: 3s.
// The minimum amount of time to wait for another frame to be
// received before transmitting an acknowledgement.
constexpr Timer::duration_t default_t2 = std::chrono::seconds{ 3 };

// Linux: 300s.
// The period of time we wait between sending a check that the link is still active.
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

class ConnEntry
{
public:
    using time_point_t = std::chrono::time_point<std::chrono::steady_clock>;
    using duration_t = std::chrono::duration<int, std::milli>;

    ConnEntry(int id, Connection::send_func_t cb)
        : id_(id), con_(id, cb, [this](std::string_view p) { deliver(p); })
    {
        con_.set_state_change_cb([this](seqpacket::con::ConnectionState* s) {
            log() << id_ << " >>> State => " << s->name();
            // Lock is already held at this point, because everything con_ does is
            // protected.
            // std::unique_lock<std::mutex> lk(mu_);
            cv_.notify_all();
        });
    }
    void await_state_not(std::string_view st)
    {
        std::unique_lock<std::mutex> lk(mu_);
        wait(lk, [this, st] { return st != con_.state().name(); });
    }

    template <typename T>
    auto apply(T func)
    {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.notify_all();
        return func(con_);
    }

    // Return false for second arg if connection has died.
    std::pair<std::string, bool> await_data()
    {
        for (;;) {
            std::unique_lock<std::mutex> lk(mu_);
            // log() << id_ << " await_data() (post lock)";
            wait(lk, [this] {
                if (!con_.state().can_receive_data()) {
                    return true;
                }
                if (!received_.empty()) {
                    return true;
                }
                return false;
            });
            if (!con_.state().can_receive_data()) {
                return { "", false };
            }

            if (received_.empty()) {
                continue;
            }

            // We have data. Deliver it.
            assert(con_.state().can_receive_data());
            auto ret = received_.front();
            received_.pop_front();
            return { ret, true };
        }
    }

    int id() const { return id_; }

private:
    void deliver(std::string_view sv);

    // Waits until pred is true, while triggering timers when needed.
    //
    // Very very ugly code because the timer can change *while we are
    // waiting*.
    //
    // Any timer change will cause cv wakeup, but not necessarily pred()
    // success.
    //
    // TODO: surely there's a cleaner way to do this.
    template <typename Pred>
    void wait(std::unique_lock<std::mutex>& lk, Pred pred)
    {
        bool done = false;
        while (!done) {
            // log() << id_ << " wait() outer";
            const auto [until, timer_running] = next_timer();

            if (!timer_running) {
                cv_.wait(lk, [this, &done, &pred, &until, &timer_running] {
                    // log() << id_ << " inner loop waiting with no timer running";
                    if (pred()) {
                        done = true;
                        return true;
                    }
                    const auto [n2, t2] = next_timer();
                    if (until != n2) {
                        return true;
                    }
                    if (timer_running != t2) {
                        return true;
                    }
                    return false;
                });
                continue;
            }
            const bool notimeout =
                cv_.wait_until(lk, until, [this, &done, &pred, &until, &timer_running] {
                    // log() << id_ << " inner loop waiting with timer running";
                    if (pred()) {
                        done = true;
                        return true;
                    }
                    const auto [n2, t2] = next_timer();
                    if (until != n2) {
                        return true;
                    }
                    if (timer_running != t2) {
                        return true;
                    }
                    return false;
                });
            trigger_timers();
            if (notimeout) {
                return;
            }
        }
    }

    std::pair<time_point_t, bool> next_timer() const;
    void trigger_timers();

    const int id_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> received_;
    Connection con_;
};

void ConnEntry::deliver(std::string_view sv)
{
    // Always called with mu_ held, because it's called from the Connection.
    received_.push_back(std::string(sv));
    cv_.notify_all();
}

std::pair<ConnEntry::time_point_t, bool> ConnEntry::next_timer() const
{
    auto& t1 = con_.data().t1;
    auto& t3 = con_.data().t3;
    if (!t1.running() && t3.running()) {
        return { t3.deadline(), true };
    }
    if (t1.running() && !t3.running()) {
        return { t1.deadline(), true };
    }
    if (!t1.running() && !t3.running()) {
        return { time_point_t{}, false };
    }
    return { std::min(t1.deadline(), t3.deadline()), true };
}

void ConnEntry::trigger_timers()
{
    if (con_.data().t1.expired()) {
        con_.data().t1.stop();
        con_.timer1_tick();
    }
    if (con_.data().t3.expired()) {
        con_.data().t3.stop();
        con_.timer3_tick();
    }
}

class SeqPacketImpl final : public ax25ms::SeqPacketService::Service
{
public:
    SeqPacketImpl(ax25ms::RouterService::Stub* router) : router_(router) {}
    void packet_callback(const ax25ms::Frame& frame)
    {
        const auto [packet, status] = ax25::parse(frame.payload(), fcs_in);
        if (!status.ok()) {
            log() << "Parse error for frame: " << status.error_message();
            return;
        }

        auto src = packet.src();
        if (packet.has_sabm() || packet.has_sabme()) {
            src = "";
        }

        // TODO: slightly cleaner to not do lookup on packets that are not seqpackets.
        auto connitr = connections_.find({ packet.dst(), src });
        if (connitr == connections_.end()) {
            log() << "Unknown connection src=" << packet.src() << " dst=" << packet.dst();
            return;
        }
        auto& conn = connitr->second;
        if (packet.has_ua()) {
            conn->apply([&packet](auto& con) { con.ua(packet); });
        } else if (packet.has_iframe()) {
            conn->apply([&packet](auto& con) { con.iframe(packet); });
        } else if (packet.has_rr()) {
            conn->apply([&packet](auto& con) { con.rr(packet); });
        } else if (packet.has_disc()) {
            conn->apply([&packet](auto& con) { con.disc(packet); });
            // TODO: report connection down.
        } else if (packet.has_sabm()) {
            conn->apply([&packet](auto& con) { con.sabm(packet); });
        } else {
            // Not a seqpacket-related frame.
            log() << "Not a seqpacket-related frame:\n" << ax25ms::proto2string(packet);
            return;
        }
        // log() << "Got seqpacket frame of size " << frame.payload().size();

        // TODO: conn->();
        //  find the connection this is for.
    }

    grpc::Status
    Connect(grpc::ServerContext* ctx,
            grpc::ServerReaderWriter<ax25ms::SeqConnectAcceptResponse,
                                     ax25ms::SeqConnectAcceptRequest>* stream) override
    {
        try {
            auto ret = Connect2(ctx, stream);
            log() << "Connect() returning";
            return ret;
        } catch (const std::exception& e) {
            log() << "Connect() exception: " << e.what();
        } catch (...) {
            log() << "Connect() unknown exception";
        }
        return grpc::Status(grpc::INTERNAL, "exception");
    }
    grpc::Status
    Accept(grpc::ServerContext* ctx,
           grpc::ServerReaderWriter<ax25ms::SeqConnectAcceptResponse,
                                    ax25ms::SeqConnectAcceptRequest>* stream) override
    {
        try {
            return Accept2(ctx, stream);
        } catch (const std::exception& e) {
            log() << "Accept() exception: " << e.what();
        } catch (...) {
            log() << "Accept() unknown exception";
        }
        return grpc::Status(grpc::INTERNAL, "exception");
    }

    grpc::Status
    Accept2(grpc::ServerContext* ctx,
            grpc::ServerReaderWriter<ax25ms::SeqConnectAcceptResponse,
                                     ax25ms::SeqConnectAcceptRequest>* stream)
    {
        log() << "Accepting a new connection()";
        ax25ms::SeqConnectAcceptRequest req;
        if (!stream->Read(&req)) {
            return grpc::Status(grpc::INVALID_ARGUMENT, "no initial data received");
        }
        if (!req.has_packet()) {
            return grpc::Status(grpc::INVALID_ARGUMENT, "initial burst had no packet");
        }
        if (!req.packet().has_metadata()) {
            return grpc::Status(grpc::INVALID_ARGUMENT, "initial burst had no metadata");
        }
        auto dst = req.packet().metadata().address().address();
        const auto src = req.packet().metadata().source_address().address();

        // Lifetime of ce is until end of this function, since nothing else deletes it.
        auto& ce = *([this, &src, &dst] {
            Connection::send_func_t fs = [this](auto& p) { return send(p); };
            auto conu = std::make_unique<ConnEntry>(get_new_id(), fs);
            std::unique_lock<std::mutex> lk(mu_);
            auto [itr, ok] =
                connections_.insert({ std::make_pair(src, dst), std::move(conu) });
            if (!ok) {
                throw std::runtime_error("Failed to insert connection. Duplicate?");
            }
            return itr->second.get();
        }());

        // TODO: also await ctx cancellation.
        ce.await_state_not(seqpacket::con::StateNames::Disconnected);
        {
            dst = ce.apply([](auto& con) { return con.dst(); });
            std::unique_lock<std::mutex> lk(mu_);
            auto node = connections_.extract({ src, "" });
            log() << ce.id() << " accept changing to <" << src << "> <" << dst << ">";
            node.key() = std::make_pair(src, dst);
            connections_.insert(std::move(node));
        }

        log() << ce.id() << " Connection accepted!";

        // Send connection metadata
        {
            ax25ms::SeqConnectAcceptResponse resp;
            auto& meta = *resp.mutable_packet()->mutable_metadata();
            meta.mutable_address()->set_address(dst);
            meta.mutable_source_address()->set_address(src);
            if (!stream->Write(resp)) {
                log() << ce.id() << "Failed to write to stream";
                return grpc::Status(grpc::UNKNOWN,
                                    "failed to inform client that we were successful");
            }
        }

        // Start receiving from router.
        std::jthread receive_thread([ctx, &ce, stream] {
            pthread_setname_np(pthread_self(), "accept_receive_read");
            for (;;) {
                auto [payload, ok] = ce.await_data();
                if (!ok) {
                    log() << ce.id() << " Stopping accept reader";
                    break;
                }
                ax25ms::SeqConnectAcceptResponse data;
                data.mutable_packet()->set_payload(payload);
                if (!stream->Write(data)) {
                    log() << ce.id() << "Failed to write to stream";
                    break;
                }
            }
            ctx->TryCancel(); // TODO: defer this.
        });

        // Receive from RPC client.
        while (stream->Read(&req)) {
            ce.apply([&req](auto& con) { con.dl_data(req.packet().payload()); });
        }
        log() << ce.id() << " Accept connection ended";
        ce.apply([](auto& con) { con.dl_disconnect(); });
        receive_thread.join();
        {
            std::unique_lock<std::mutex> lk(mu_);
            log() << ce.id() << " Removing accept connection entry <" << src << "> <"
                  << dst << ">";
            connections_.erase({ src, dst });
        }
        return grpc::Status::OK;
    }

    grpc::Status send(const ax25::Packet& p)
    {
        const auto data = ax25::serialize(p, fcs_out);

        ax25ms::SendRequest sreq;
        sreq.mutable_frame()->set_payload(data);
        ax25ms::SendResponse resp;
        grpc::ClientContext ctx;
        const auto status = router_->Send(&ctx, sreq, &resp);
        if (!status.ok()) {
            log() << "  Sending data failed";
            throw std::runtime_error("Sending data failed: " + status.error_message());
        }
        return status;
    }

    grpc::Status
    Connect2(grpc::ServerContext* ctx,
             grpc::ServerReaderWriter<ax25ms::SeqConnectAcceptResponse,
                                      ax25ms::SeqConnectAcceptRequest>* stream)
    {
        log() << "Starting a new connection";
        ax25ms::SeqConnectAcceptRequest req;
        if (!stream->Read(&req)) {
            return grpc::Status(grpc::INVALID_ARGUMENT, "no initial data received");
        }
        log() << "Received connection request: " << proto2string(req);
        if (!req.has_packet()) {
            return grpc::Status(grpc::INVALID_ARGUMENT, "initial burst had no packet");
        }
        if (!req.packet().has_metadata()) {
            return grpc::Status(grpc::INVALID_ARGUMENT, "initial burst had no metadata");
        }
        const auto dst = req.packet().metadata().address().address();
        const auto src = req.packet().metadata().source_address().address();

        log() << " connect() inserting <" << src << "> <" << dst << ">";
        auto& ce = *([this, &src, &dst] {
            Connection::send_func_t fs = [this](auto& p) { return send(p); };
            auto conu = std::make_unique<ConnEntry>(get_new_id(), fs);
            std::unique_lock<std::mutex> lk(mu_);
            auto [itr, ok] =
                connections_.insert({ std::make_pair(src, dst), std::move(conu) });
            if (!ok) {
                throw std::runtime_error(std::to_string(conu->id()) +
                                         " Failed to insert connection. Duplicate?");
            }
            return itr->second.get();
        }());
        Defer er([this, &src, &dst, &ce] {
            std::unique_lock<std::mutex> lk(mu_);
            log() << ce.id() << " Removing connect connection entry <" << src << "> <"
                  << dst << ">";
            connections_.erase({ src, dst });
        });

        ce.apply([&src, &dst](auto& con) { con.dl_connect(dst, src); });
        ce.await_state_not(seqpacket::con::StateNames::AwaitingConnection);

        // Send connection metadata
        {
            ax25ms::SeqConnectAcceptResponse metadata;
            if (!stream->Write(metadata)) {
                log() << "Failed to write to stream";
                return grpc::Status(grpc::UNKNOWN,
                                    "failed to inform client that we were successful");
            }
        }

        // Start receiving.
        std::jthread receive_thread([ctx, &ce, stream] {
            pthread_setname_np(pthread_self(), "connect_receive_read");
            for (;;) {
                const auto [payload, ok] = ce.await_data();
                if (!ok) {
                    log() << "Stopping reader";
                    break;
                }
                ax25ms::SeqConnectAcceptResponse data;
                data.mutable_packet()->set_payload(payload);
                if (!stream->Write(data)) {
                    log() << "Failed to write to stream";
                    break;
                }
            }
            ctx->TryCancel(); // TODO: defer this.
        });
        while (stream->Read(&req)) {
            ce.apply([&req](auto& con) { con.dl_data(req.packet().payload()); });
        }
        log() << ce.id() << " Client Connection ended";
        ce.apply([](auto& con) { con.dl_disconnect(); });
        receive_thread.join();
        return grpc::Status::OK;
    }

private:
    int get_new_id()
    {
        static int n = 0;
        return ++n;
    }

    // Timer scheduler_;
    ax25ms::RouterService::Stub* router_;
    std::mutex mu_;
    std::map<std::pair<std::string, std::string>, std::unique_ptr<ConnEntry>>
        connections_;
};


} // namespace ax25ms

namespace {
[[noreturn]] void usage(const char* av0, int err)
{
    std::cout << "Usage: " << av0
              << " [ -f <no|in|out|both> ] [ -h ] [ -l <listen address> ]\n"
              << "    -r <router host:port>\n";
    exit(err);
}
} // namespace

int wrapmain(int argc, char** argv)
{
    using ax25ms::log;

    std::string router;
    std::string listen = "[::]:12346";
    {
        int opt;
        while ((opt = getopt(argc, argv, "hr:l:f:")) != -1) {
            switch (opt) {
            case 'r':
                router = optarg;
                break;
            case 'l':
                listen = optarg;
                break;
            case 'f': {
                const std::string fcs = optarg;
                if (fcs == "no") {
                    fcs_in = false;
                    fcs_out = false;
                }
                if (fcs == "in") {
                    fcs_in = true;
                    fcs_out = false;
                }
                if (fcs == "out") {
                    fcs_in = false;
                    fcs_out = true;
                }
                if (fcs == "both") {
                    fcs_in = true;
                    fcs_out = true;
                }
                break;
            }
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
            default:
                usage(argv[0], EXIT_FAILURE);
            }
        }
    }
    if (router.empty()) {
        log() << "Need to specify router (-r)";
        return EXIT_FAILURE;
    }
    if (optind != argc) {
        log() << "Invalid extra args on the command line";
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
        packet.set_command_response(true);
        auto data = ax25::serialize(packet, fcs_out);


        auto parsed = ax25::parse(data, fcs_in);
        std::cout << ax25ms::proto2string(packet);

        ax25ms::SendRequest req;
        req.mutable_frame()->set_payload(data);
        ax25ms::SendResponse resp;
        auto status = stub->Send(&ctx, req, &resp);
        if (!status.ok()) {
            log() << "Failed to send: " << status.error_message();
        } else {
            log() << "Successfully sent a packet";
        }
    }

    // Start up server.
    std::cout << "Starting up a server" << std::endl;
    ax25ms::SeqPacketImpl service(stub.get());
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    log() << "Server started";

    // Start stream from router.
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
            log() << "Stream ended with error";
        }
        log() << "Stream ended. Looping";

        std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });
    }
    // server->Wait();
}
