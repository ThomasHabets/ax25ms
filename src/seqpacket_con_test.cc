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

#include <google/protobuf/text_format.h>
#include <google/protobuf/util/message_differencer.h>

namespace {

std::string client = "M0THC-1";
std::string tester = "M0THC-2";

void assert_eq(size_t got, size_t want)
{
    if (got != want) {
        std::cerr << "Got " << got << " want " << want << "\n";
        exit(1);
    }
}

void assert_eq(const ax25::Packet& got, const ax25::Packet& want)
{
    if (!google::protobuf::util::MessageDifferencer::Equals(got, want)) {
        std::cerr << "--------------------\n"
                  << "Protos differ. Got:\n"
                  << ax25ms::proto2string(got) << "\nwant:\n"
                  << ax25ms::proto2string(want) << "\n";
        exit(1);
    }
}

void assert_eq(const std::vector<ax25::Packet>& got,
               const std::vector<ax25::Packet>& want)
{
    if (got.size() != want.size()) {
        std::cerr << "Got size " << got.size() << " want size " << want.size() << "\n";
        exit(1);
    }
    for (size_t i = 0; i < got.size(); i++) {
        assert_eq(got[i], want[i]);
    }
}

ax25::Packet mkpacket(const std::string& s);
void assert_eq(const std::vector<ax25::Packet>& got, const std::vector<std::string>& want)
{
    std::vector<ax25::Packet> w2;
    for (auto& w : want) {
        w2.push_back(mkpacket(w));
    }
    assert_eq(got, w2);
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

ax25::Packet mkpacket(const std::string& s)
{
    google::protobuf::io::ArrayInputStream arr(s.data(), s.size());
    ax25::Packet ret;
    google::protobuf::TextFormat::Parse(&arr, &ret);
    return ret;
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
    std::cerr << "=========== Test Server ===============\n";
    std::vector<ax25::Packet> sent;
    std::vector<std::string> received;
    seqpacket::con::Connection con(
        1,
        [&sent](const ax25::Packet& p) {
            sent.push_back(p);
            std::cout << "Sending packet:\n" << ax25ms::proto2string(p) << "\n";
            return grpc::Status::OK;
        },
        [&received](std::string_view p) {
            received.push_back(std::string(p));
            std::cout << "Received data: <" << p << ">\n";
        });
    con.set_state_change_cb([](seqpacket::con::ConnectionState* s) {
        std::cout << ">>> State change to " << s->name() << "\n";
    });

    std::clog << "---------------- Connecting…\n";
    {
        con.sabm(packet_sabm(true));
    }
    assert_eq(sent, { packet_ua(false, true) });
    sent.clear();

    int ns = 0;
    std::clog << "----------------- Receive data…\n";
    {
        con.iframe(mkpacket("iframe {payload: 'blah' ns: " + std::to_string(ns++) +
                            "} command_response: true"));
    }
    assert_eq(received.size(), 1);
    assert(received[0] == "blah");
    received.clear();

    std::clog << "-------- Receive data2…\n";
    {
        con.iframe(mkpacket("iframe {payload: 'blah2' ns: " + std::to_string(ns++) +
                            "} command_response: true"));
    }
    assert_eq(received.size(), 1);
    assert(received[0] == "blah2");
    received.clear();
    assert(sent.empty());

    std::clog << "----------- Ticking timer: " << con.data().t1.running() << "\n";
    con.timer1_tick();
    assert_eq(sent,
              { "src: 'M0THC-2' dst: 'M0THC-1' rr { nr: 2 poll: true } command_response: "
                "true" });
    sent.clear();

    std::clog << "------------ Send data…\n";
    {
        con.dl_data("hello");
    }
    assert_eq(sent,
              { "src: 'M0THC-2' dst: 'M0THC-1' iframe { nr: 2 pid: 240 payload: "
                "'hello' } command_response: true" });
    sent.clear();

    std::clog << "------------ Send data…\n";
    {
        con.dl_data("world");
    }
    assert_eq(sent,
              {
                  "src: 'M0THC-2' dst: 'M0THC-1' iframe { ns: 1 nr: 2 pid: 240 payload: "
                  "'world' } command_response: true",
              });
    sent.clear();

    std::clog << "Disconnecting…\n";
    {
        ax25::Packet p;
        con.disc(p);
    }
    assert_eq(sent, { packet_ua(false, false) });
    sent.clear();
}

void test_client()
{
    std::cerr << "=========== Test Client ===============\n";
    std::vector<ax25::Packet> sent;
    std::vector<std::string> received;
    seqpacket::con::Connection con(
        1,
        [&sent](const ax25::Packet& p) {
            sent.push_back(p);
            std::clog << "Sending packet:\n" << ax25ms::proto2string(p) << "\n";
            return grpc::Status::OK;
        },
        [&received](std::string_view p) {
            received.push_back(std::string(p));
            std::clog << "Received data: <" << p << ">\n";
        });
    con.set_state_change_cb([](seqpacket::con::ConnectionState* s) {
        std::clog << ">>> State change to " << s->name() << "\n";
    });

    std::clog << "---------- Connecting…\n";
    {
        con.dl_connect(client, tester);
    }
    assert_eq(sent, { packet_sabm(false) });
    sent.clear();

    std::clog << "------------ Connection accepted\n";
    {
        con.ua(packet_ua(false, true));
    }
    assert(sent.empty());
    sent.clear();

    std::clog << "--- Send data1…\n";
    con.dl_data("hello");
    assert_eq(sent,
              {
                  packet_iframe(false, 0, 0, false, true, "hello"),
              });
    sent.clear();

    std::clog << "--- Send data2…\n";
    con.dl_data("big");
    assert_eq(sent,
              {
                  packet_iframe(false, 0, 1, false, true, "big"),
              });
    sent.clear();

    std::clog << "--- Send data3…\n";
    con.dl_data("world");
    assert_eq(sent,
              {
                  packet_iframe(false, 0, 2, false, true, "world"),
              });
    sent.clear();

    std::clog << "--- Put into timer recovery…\n";
    con.timer1_tick();
    assert_eq(sent,
              {
                  packet_rr(false, 0, true, true),
              });
    sent.clear();

    std::clog << "--- Tester replies that it only got first packet…\n";
    con.rr(packet_rr(false, 1, true, false));
    assert_eq(sent,
              {
                  packet_iframe(false, 0, 1, false, true, "big"),
                  packet_iframe(false, 0, 2, false, true, "world"),
              });
    sent.clear();

    std::clog << "--- Tester replies that it only got first two packets…\n";
    con.rr(packet_rr(false, 2, true, false));
    assert_eq(sent,
              {
                  packet_iframe(false, 0, 2, false, true, "world"),
              });
    sent.clear();

    std::clog << "--- Send data to fill window…\n";
    con.dl_data("data3"); // 3
    con.dl_data("data4"); // 4
    con.dl_data("data5"); // 5
    con.dl_data("data6"); // 6
    con.dl_data("data7"); // 7
    con.dl_data("data8"); // 0
    assert_eq(sent,
              {
                  packet_iframe(false, 0, 3, false, true, "data3"),
                  packet_iframe(false, 0, 4, false, true, "data4"),
                  packet_iframe(false, 0, 5, false, true, "data5"),
                  packet_iframe(false, 0, 6, false, true, "data6"),
                  packet_iframe(false, 0, 7, false, true, "data7"),
                  packet_iframe(false, 0, 0, true, true, "data8"), // Poll!
              });
    sent.clear();

    std::clog << "--- Send data one past window…\n";
    con.dl_data("data9"); // 1
    assert(sent.size() == 0);

    std::clog << "--- Send four more past window…\n";
    con.dl_data("data10"); // 2
    con.dl_data("data11"); // 3
    con.dl_data("data12"); // 4
    con.dl_data("data13"); // 5
    assert(sent.size() == 0);

    std::clog << "--- Ack one packet…\n";
    con.rr(packet_rr(false, 3, true, false)); // acking 'world'
    assert_eq(sent,
              {
                  packet_iframe(false, 0, 3, false, true, "data3"),
                  packet_iframe(false, 0, 4, false, true, "data4"),
                  packet_iframe(false, 0, 5, false, true, "data5"),
                  packet_iframe(false, 0, 6, false, true, "data6"),
                  packet_iframe(false, 0, 7, false, true, "data7"),
                  packet_iframe(false, 0, 0, false, true, "data8"),
                  packet_iframe(false, 0, 1, true, true, "data9"), // Poll!
              });
    sent.clear();

    std::clog << "--- Ack two more packet…\n";
    con.rr(packet_rr(false, 5, true, false));
    assert_eq(sent,
              {
                  packet_iframe(false, 0, 5, false, true, "data5"),
                  packet_iframe(false, 0, 6, false, true, "data6"),
                  packet_iframe(false, 0, 7, false, true, "data7"),
                  packet_iframe(false, 0, 0, false, true, "data8"),
                  packet_iframe(false, 0, 1, false, true, "data9"),
                  packet_iframe(false, 0, 2, false, true, "data10"),
                  packet_iframe(false, 0, 3, true, true, "data11"), // Poll!
              });
    sent.clear();

    std::clog << "--- Ack all but one outstanding packet…\n";
    con.rr(packet_rr(false, 3, true, false));
    assert_eq(sent,
              {
                  packet_iframe(false, 0, 3, false, true, "data11"),
                  packet_iframe(false, 0, 4, false, true, "data12"),
                  packet_iframe(false, 0, 5, false, true, "data13"),
              });
    sent.clear();

    std::clog << "--- Ack but one outstanding packet (final)…\n";
    con.rr(packet_rr(false, 5, true, false));
    assert_eq(sent, { packet_iframe(false, 0, 5, false, true, "data13") });
    sent.clear();

    std::clog << "--- Ack final packet…\n";
    con.rr(packet_rr(false, 6, true, false));
    assert(sent.size() == 0);
    sent.clear();

    std::clog << "--- Probe for more…\n";
    con.rr(packet_rr(false, 6, true, true));
    assert_eq(sent,
              {
                  packet_rr(false, 0, true, false),
              });
    sent.clear();

    std::clog << "--- Close…\n";
    con.disc(mkpacket("src: 'M0THC-1'"));
    assert_eq(sent,
              {
                  "dst: 'M0THC-1' src: 'M0THC-2' ua {} command_response_la: true",
              });
}

int main()
{
    test_server();
    test_client();
    std::clog << "OK\n";
}
