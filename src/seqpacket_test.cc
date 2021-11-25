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

#include <grpcpp/grpcpp.h>

using namespace ax25ms;

namespace ax25ms {
class ConnectionTest
{
public:
    void run();
};

std::string client = "M0THC-1";
std::string server = "M0THC-2";

ax25::Packet packet_rr(int nr)
{
    ax25::Packet p;
    p.mutable_rr()->set_nr(nr);
    return p;
}

ax25::Packet packet_iframe(bool cli, int nr, int ns, bool poll, std::string_view payload)
{
    ax25::Packet p;
    if (cli) {
        p.set_src(client);
        p.set_dst(server);
    } else {
        p.set_src(server);
        p.set_dst(client);
    }

    auto& iframe = *p.mutable_iframe();
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


class FakeRouter final : public ax25ms::RouterService::Service
{
public:
    grpc::Status Send(grpc::ServerContext* ctx,
                      const ax25ms::SendRequest* req,
                      ax25ms::SendResponse* out) override;

    int packet_count_ = 0;
};

grpc::Status FakeRouter::Send(grpc::ServerContext* ctx,
                              const ax25ms::SendRequest* req,
                              ax25ms::SendResponse* out)
{
    std::cerr << "Fake::Send\n";
    packet_count_++;
    const auto [packet, st] = ax25::parse(req->frame().payload());
    if (!st.ok()) {
        throw std::runtime_error("implementation sent packet that doesn't parse: " +
                                 st.error_message());
    }
    std::cerr << ax25ms::proto2string(packet) << "\n";
    return grpc::Status::OK;
}

void ConnectionTest::run()
{
    // Start server.
    // std::jthread srvth;
    //{
    FakeRouter srv;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("[::]:13001", grpc::InsecureServerCredentials());
    builder.RegisterService(&srv);
    std::unique_ptr<grpc::Server> routersrv(builder.BuildAndStart());
    std::clog << "Running…\n";
    // srvth = std::jthread([server]{server->Wait();});
    //}

    // Connect to fake
    auto channel =
        grpc::CreateChannel("localhost:13001", grpc::InsecureChannelCredentials());
    std::unique_ptr<ax25ms::RouterService::Stub> router{ ax25ms::RouterService::NewStub(
        channel) };


    Timer timer;
    timer.stop_for_test();

    Connection con("M0THC-1", "M0THC-2", router.get(), &timer);
    // Verify initial state.
    assert(con.nrm() == 0);
    assert(con.nsm() == 0);
    assert(srv.packet_count_ == 0);

    // Receive first packet.
    con.iframe(packet_iframe(false, 0, 0, true, "hello"));
    assert(con.nrm() == 1);
    assert(con.nsm() == 0);
    assert(srv.packet_count_ == 0);

    // Receive second packet.
    timer.tick_for_test(std::chrono::seconds{ 1 });
    con.iframe(packet_iframe(false, 0, 1, true, "world"));
    assert(con.nrm() == 2);
    assert(con.nsm() == 0);
    assert(srv.packet_count_ == 0);

    // Receive dup.
    con.iframe(packet_iframe(false, 0, 1, true, "world"));
    assert(con.nrm() == 2);
    assert(con.nsm() == 0);
    assert(srv.packet_count_ == 1);

    std::cerr << "Triggering timer\n";
    timer.tick_for_test(std::chrono::seconds{ 5 });
    timer.drain();
    // TODO: actually there should be no RR, that was triggered by the
    // first packet. We already sent an RR because of the dup.
    assert(srv.packet_count_ == 2);
}

} // namespace ax25ms


int main()
{
    ConnectionTest test;
    test.run();
}
