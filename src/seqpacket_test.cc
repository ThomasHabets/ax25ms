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
#include "parse.h"
#include "proto/gen/ax25.pb.h"
#include "seqpacket.h"
#include "serialize.h"
#include "util.h"

#include <unistd.h>

#include <google/protobuf/util/message_differencer.h>
#include <grpcpp/grpcpp.h>

using namespace ax25ms;

void assert_eq(const ax25::Packet& got, const ax25::Packet& want)
{
    if (!google::protobuf::util::MessageDifferencer::Equals(got, want)) {
        std::cerr << "Protos differ. Got: " << proto2string(got)
                  << "\nwant: " << proto2string(want) << "\n";
        exit(1);
    }
}


namespace ax25ms {

class FakeRouter final : public ax25ms::RouterService::Service
{
public:
    FakeRouter(Timer* timer) : timer_(timer), st_(timer->now()) {}
    grpc::Status Send(grpc::ServerContext* ctx,
                      const ax25ms::SendRequest* req,
                      ax25ms::SendResponse* out) override;

    std::mutex mu_;
    std::vector<ax25::Packet> received_;

private:
    Timer* timer_;
    Timer::time_point_t st_;
};

class ConnectionTest
{
public:
    void run();
    void test_connect(FakeRouter& fake,
                      Timer& timer,
                      Connection& con,
                      ax25ms::RouterService::Stub* router);
    void test_receive(FakeRouter& fake,
                      Timer& timer,
                      Connection& con,
                      ax25ms::RouterService::Stub* router);
    void test_send_failing(FakeRouter& fake,
                           Timer& timer,
                           Connection& con,
                           ax25ms::RouterService::Stub* router);
};

std::string client = "M0THC-1";
std::string server = "M0THC-2";

ax25::Packet packet_base(bool cli, bool cr)
{
    ax25::Packet p;
    if (cli) {
        p.set_src(client);
        p.set_dst(server);
    } else {
        p.set_src(server);
        p.set_dst(client);
    }
    p.set_command_response(cr);
    return p;
}

ax25::Packet packet_rr(bool cli, int nr)
{
    auto p = packet_base(cli, false);
    p.mutable_rr()->set_nr(nr);
    p.mutable_rr()->set_poll(true);
    return p;
}

ax25::Packet packet_rej(bool cli, int nr)
{
    auto p = packet_base(cli, false);
    p.mutable_rej()->set_nr(nr);
    p.mutable_rej()->set_poll(true);
    return p;
}

ax25::Packet packet_sabm(bool cli)
{
    auto p = packet_base(cli, true);
    p.mutable_sabm()->set_poll(true);
    return p;
}

ax25::Packet packet_ua(bool cli)
{
    auto p = packet_base(cli, false);
    p.mutable_ua();
    return p;
}

ax25::Packet
packet_iframe(bool cli, int nr, int ns, bool poll, bool cr, std::string_view payload)
{
    auto p = packet_base(cli, cr);
    auto& iframe = *p.mutable_iframe();
    iframe.set_pid(0xF0);
    iframe.set_nr(nr);
    iframe.set_ns(ns);
    iframe.set_poll(poll);
    iframe.set_payload(payload.data(), payload.size());
    return p;
}

ax25ms::Frame ser(const ax25::Packet& packet)
{
    auto data = ax25::serialize(packet);
    ax25ms::Frame frame;
    frame.set_payload(data);
    return frame;
}

std::string timediff(Timer::time_point_t a, Timer::time_point_t b)
{
    std::chrono::duration<double> diff = b - a;
    return std::to_string(diff.count());
}


grpc::Status FakeRouter::Send(grpc::ServerContext* ctx,
                              const ax25ms::SendRequest* req,
                              ax25ms::SendResponse* out)
{
    std::cerr << "T=" << timediff(st_, timer_->now()) << " Fake::Send\n";
    const auto [packet, st] = ax25::parse(req->frame().payload());
    if (!st.ok()) {
        throw std::runtime_error("implementation sent packet that doesn't parse: " +
                                 st.error_message());
    }
    std::unique_lock<std::mutex> lk(mu_);
    received_.push_back(packet);
    std::cerr << ax25ms::proto2string(packet) << "\n";
    return grpc::Status::OK;
}

void ConnectionTest::run()
{
    Timer timer;
    timer.stop_for_test();

    FakeRouter srv(&timer);
    grpc::ServerBuilder builder;
    int port;
    builder.AddListeningPort("[::]:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&srv);
    std::unique_ptr<grpc::Server> routersrv(builder.BuildAndStart());
    std::clog << "Running…\n";

    // Connect to fake
    auto channel = grpc::CreateChannel("localhost:" + std::to_string(port),
                                       grpc::InsecureChannelCredentials());
    std::unique_ptr<ax25ms::RouterService::Stub> router{ ax25ms::RouterService::NewStub(
        channel) };

    using func_t = decltype(&ax25ms::ConnectionTest::test_receive);

    for (auto& func : std::vector<func_t>{
             &ax25ms::ConnectionTest::test_receive,
             &ax25ms::ConnectionTest::test_send_failing,
         }) {
        Connection con("M0THC-1", "M0THC-2", router.get(), &timer, false);
        (this->*func)(srv, timer, con, router.get());

        timer.tick_for_test(std::chrono::seconds{ 3600 });
        timer.drain();
        assert(srv.received_.empty());
    }
}

void ConnectionTest::test_send_failing(FakeRouter& srv,
                                       Timer& timer,
                                       Connection& con,
                                       ax25ms::RouterService::Stub* router)
{
    std::cout << "--------------------- test send failing ------------------\n";
    test_connect(srv, timer, con, router);

    // Send.
    con.write("hello");
    assert(con.nrm() == 0);
    assert(con.nsm() == 1);
    assert(srv.received_.empty());

    // Trigger all the resends.
    for (int c = 0; c < 10; c++) {
        timer.tick_for_test(std::chrono::seconds{ 10 });
    }
    assert(srv.received_.size() == 3);
    for (auto& p : srv.received_) {
        assert_eq(p, packet_iframe(true, 0, 0, false, true, "hello"));
    }
    assert(con.state_ == Connection::State::FAILED);
    srv.received_.clear();
}

void ConnectionTest::test_connect(FakeRouter& srv,
                                  Timer& timer,
                                  Connection& con,
                                  ax25ms::RouterService::Stub* router)
{
    // Verify initial state.
    assert(con.nrm() == 0);
    assert(con.nsm() == 0);
    assert(srv.received_.empty());
    assert(con.state_ == Connection::State::IDLE);

    // Send SABM.
    std::jthread th([&] {
        for (;;) {
            timer.tick_for_test(std::chrono::milliseconds{ 10 });
            std::unique_lock<std::mutex> lk(srv.mu_);
            if (srv.received_.empty()) {
                continue;
            }
            assert(con.get_state() == Connection::State::CONNECTING);
            break;
        }
        con.ua(packet_ua(false));
    });
    grpc::ServerContext ctx;
    con.connect(&ctx);
    assert(con.state_ == Connection::State::CONNECTED);
    assert(con.nrm() == 0);
    assert(con.nsm() == 0);
    assert(srv.received_.size() == 1);
    assert_eq(srv.received_[0], packet_sabm(true));
    srv.received_.clear();
}

void ConnectionTest::test_receive(FakeRouter& srv,
                                  Timer& timer,
                                  Connection& con,
                                  ax25ms::RouterService::Stub* router)
{
    std::cout << "--------------------- test receive ------------------\n";
    test_connect(srv, timer, con, router);

    // Receive first packet.
    std::cout << "  Receive first packet\n";
    con.iframe(packet_iframe(false, 0, 0, true, false, "hello"));
    assert(con.nrm() == 1);
    assert(con.nsm() == 0);
    assert(srv.received_.empty());

    // Make sure reply was sent.
    timer.tick_for_test(std::chrono::seconds{ 4 });
    assert(srv.received_.size() == 1);
    assert_eq(srv.received_[0], packet_rr(true, 1));
    srv.received_.clear();

    // Receive second packet.
    std::cout << "  Receive second packet\n";
    con.iframe(packet_iframe(false, 0, 1, true, false, "world"));
    assert(con.nrm() == 2);
    assert(con.nsm() == 0);
    assert(srv.received_.empty());

    // Receive third packet.
    std::cout << "  Receive third packet\n";
    timer.tick_for_test(std::chrono::milliseconds{ 100 });
    con.iframe(packet_iframe(false, 0, 2, true, false, "of dups"));
    assert(con.nrm() == 3);
    assert(con.nsm() == 0);
    assert(srv.received_.empty());

    // Receive dup, send REJ.
    std::cout << "  Receive third packet (dup)\n";
    timer.tick_for_test(std::chrono::milliseconds{ 100 });
    con.iframe(packet_iframe(false, 0, 2, true, false, "of dups"));
    assert(con.nrm() == 3);
    assert(con.nsm() == 0);
    assert(srv.received_.size() == 1);
    assert_eq(srv.received_[0], packet_rej(true, 3));
    srv.received_.clear();
    assert(con.state_ == Connection::State::CONNECTED);

    // Receive another dup. Hold off on the REJ.
    std::cout << "  Receive fourth packet (dup)\n";
    timer.tick_for_test(std::chrono::milliseconds{ 100 });
    con.iframe(packet_iframe(false, 0, 2, true, false, "of dups"));
    assert(con.nrm() == 3);
    assert(con.nsm() == 0);
    assert(srv.received_.empty());
    assert(con.state_ == Connection::State::CONNECTED);

    timer.tick_for_test(std::chrono::seconds{ 3600 });
}

} // namespace ax25ms


int main()
{
    for (int i = 0; i < 10; i++) {
        std::cerr << "======================\n";
        ConnectionTest test;
        test.run();
        // sleep(1);
    }
    std::cout << "======== Success ============\n";
}
