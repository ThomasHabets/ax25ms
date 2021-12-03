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
#include "seqpacket_con.h"
#include "util.h"

#include <google/protobuf/util/message_differencer.h>

namespace {

std::string client = "M0THC-1";
std::string tester = "M0THC-2";

void assert_eq(const ax25::Packet& got, const ax25::Packet& want)
{
    if (!google::protobuf::util::MessageDifferencer::Equals(got, want)) {
        std::cerr << "Protos differ. Got: " << ax25ms::proto2string(got)
                  << "\nwant: " << ax25ms::proto2string(want) << "\n";
        exit(1);
    }
}

ax25::Packet packet_base(bool cli, bool cr)
{
    ax25::Packet p;
    if (cli) {
        p.set_src(client);
        p.set_dst(tester);
    } else {
        p.set_src(tester);
        p.set_dst(client);
    }
    p.set_command_response(cr);
    p.set_command_response_la(!cr);
    return p;
}

ax25::Packet packet_rr(bool cli, int nr, bool pf, int cr)
{
    auto p = packet_base(cli, cr);
    p.mutable_rr()->set_nr(nr);
    p.mutable_rr()->set_poll(pf);
    return p;
}

ax25::Packet packet_sabm(bool cli)
{
    auto p = packet_base(cli, false);
    p.mutable_sabm()->set_poll(true);
    return p;
}

ax25::Packet packet_ua(bool cli, bool poll)
{
    auto p = packet_base(cli, false);
    p.mutable_ua()->set_poll(poll);
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

} // namespace

void test_server()
{
    std::vector<ax25::Packet> sent;
    std::vector<std::string> received;
    seqpacket::con::Connection con(
        [&sent](const ax25::Packet& p) {
            sent.push_back(p);
            std::cout << "Sending packet: " << ax25ms::proto2string(p) << "\n";
            return grpc::Status::OK;
        },
        [&received](std::string_view p) {
            received.push_back(std::string(p));
            std::cout << "Received data: <" << p << ">\n";
        });
    con.set_state_change_cb([](seqpacket::con::ConnectionState* s) {
        std::cout << ">>> State change to " << s->name() << "\n";
    });

    std::cout << "Connecting…\n";
    {
        con.sabm(packet_sabm(true));
    }
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_ua(false, true));
    sent.clear();

    int ns = 0;
    std::cout << "Receive data…\n";
    {
        ax25::Packet p;
        p.set_command_response(true);
        p.mutable_iframe()->set_payload("blah");
        p.mutable_iframe()->set_ns(ns++);
        con.iframe(p);
    }
    assert(received.size() == 1);
    assert(received[0] == "blah");
    received.clear();

    std::cout << "Receive data…\n";
    {
        ax25::Packet p;
        p.set_command_response(true);
        p.mutable_iframe()->set_payload("blah2");
        p.mutable_iframe()->set_ns(ns++);
        con.iframe(p);
    }
    assert(received.size() == 1);
    assert(received[0] == "blah2");
    received.clear();
    assert(sent.empty());

    std::cout << "Ticking timer: " << con.data().t1.running() << "\n";
    con.timer1_tick();
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_rr(false, 2, true, false));
    sent.clear();

    std::cout << "Send data…\n";
    {
        con.dl_data("hello");
    }
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_iframe(false, 2, 0, false, 0, "hello"));
    sent.clear();

    std::cout << "Send data…\n";
    {
        con.dl_data("world");
    }
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_iframe(false, 2, 1, false, 0, "world"));
    sent.clear();

    std::cout << "Disconnecting…\n";
    {
        ax25::Packet p;
        con.disc(p);
    }
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_ua(false, false));
    sent.clear();
}

void test_client()
{
    std::vector<ax25::Packet> sent;
    std::vector<std::string> received;
    seqpacket::con::Connection con(
        [&sent](const ax25::Packet& p) {
            sent.push_back(p);
            std::cout << "Sending packet: " << ax25ms::proto2string(p) << "\n";
            return grpc::Status::OK;
        },
        [&received](std::string_view p) {
            received.push_back(std::string(p));
            std::cout << "Received data: <" << p << ">\n";
        });
    con.set_state_change_cb([](seqpacket::con::ConnectionState* s) {
        std::cout << ">>> State change to " << s->name() << "\n";
    });

    std::cout << "Connecting…\n";
    {
        con.dl_connect(client, tester);
    }
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_sabm(false));
    sent.clear();

    std::cout << "Connection accepted\n";
    {
        con.ua(packet_ua(false, true));
    }
    assert(sent.empty());
    sent.clear();

    std::cout << "--- Send data1…\n";
    con.dl_data("hello");
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_iframe(false, 0, 0, false, true, "hello"));
    sent.clear();

    std::cout << "--- Send data2…\n";
    con.dl_data("big");
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_iframe(false, 0, 1, false, true, "big"));
    sent.clear();

    std::cout << "--- Send data3…\n";
    con.dl_data("world");
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_iframe(false, 0, 2, false, true, "world"));
    sent.clear();

    std::cout << "--- Put into timer recovery…\n";
    con.timer1_tick();
    assert(sent.size() == 1);
    assert_eq(sent[0], packet_rr(false, 0, true, true));
    sent.clear();

    std::cout << "--- Tester replies that it only got first packet…\n";
    con.rr(packet_rr(false, 1, true, false));
    assert_eq(sent[0], packet_iframe(false, 0, 1, false, true, "big"));
    assert_eq(sent[1], packet_iframe(false, 0, 2, false, true, "world"));
    assert(sent.size() == 2);
    sent.clear();

    std::cout << "--- Tester replies that it only got first two packets…\n";
    con.rr(packet_rr(false, 2, true, false));
    assert(sent.size() == 1);
    sent.clear();
}

int main()
{
    // test_server();
    test_client();
}
