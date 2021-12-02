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
 * Serialize AX.25 from protobuf to payload bytes.
 * Inverse of parse.
 * Main specs: http://www.tapr.org/pdf/AX25.2.2.pdf
 */
#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"
#include "proto/gen/ax25.pb.h"
#include <grpcpp/grpcpp.h>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace ax25 {

namespace {


std::string serialize_call(
    std::string_view call, bool lowbit, bool highbit, bool rbit_ext, bool rbit_dama)
{
    static std::regex callRE("([A-Z0-9]{2,6})(?:-(\\d{1,2}))?");
    std::smatch m;
    auto s = std::string(call);
    if (!std::regex_match(s, m, callRE)) {
        throw std::runtime_error("tried to serialize invalid call: " + s);
    }
    auto c = std::string(m[1]);
    auto ssids = m[2].str();
    while (c.size() < 6) {
        c += ' ';
    }
    char* end = nullptr;
    auto ssid = strtoul(ssids.c_str(), &end, 10);
    if (*end) {
        throw std::runtime_error("ssid is not a number: " + ssids);
    }
    if (ssid > 15) {
        throw std::runtime_error("ssid too high: " + ssids);
    }

    std::string ret;
    for (int i = 0; i < 6; i++) {
        ret.push_back(c[i] << 1);
    }
    ret.push_back(
        (ssid << 1) |
        (rbit_ext ? 0 : 0b01000000)    // R bit 1: Used for EXTSEQ. 1 means unused, 0 on.
        | (rbit_dama ? 0 : 0b00100000) // R bit 2: USed for DAMA(?). 1 means unused, 0 on
        | (lowbit ? 1 : 0) | (highbit ? 0x80 : 0));
    return ret;
}
} // namespace

std::string serialize(const ax25::Packet& packet)
{
    std::string ret;

    ret += serialize_call(packet.dst(),
                          false,                     // Not the last call.
                          packet.command_response(), // Command/Response
                          packet.rr_dst1(),          // Not used?
                          false);                    // Possibly DAMA?
    ret += serialize_call(packet.src(),
                          packet.repeater().empty(),    // Maybe last.
                          packet.command_response_la(), // Inverse of Command/Response.
                          packet.rr_extseq(),           // De facto extseq
                          false);                       // DAMA?
    for (int i = 0; i < packet.repeater_size(); i++) {
        ret += serialize_call(packet.repeater(i).address(),
                              i == packet.repeater_size() - 1, // Maybe last.
                              packet.repeater(i).has_been_repeated(),
                              false,  // Not used?
                              false); // Not used?
    }

    // U frames.
    if (packet.has_sabm()) {
        uint8_t control = 0b001'0'11'11;
        control |= packet.sabm().poll() ? 0b00010000 : 0;
        ret.push_back(control);
    }
    if (packet.has_sabme()) {
        ret.push_back(0b011'0'11'11);
    }
    if (packet.has_disc()) {
        uint8_t control = 0b010'0'00'11;
        control |= packet.disc().poll() ? 0b00010000 : 0;
        ret.push_back(control);
    }
    if (packet.has_dm()) {
        ret.push_back(0b000'0'11'11);
    }
    if (packet.has_ua()) {
        ret.push_back(0b011'0'00'11);
    }
    if (packet.has_rr()) {
        if (!packet.rr_extseq()) {
            uint8_t control = 0b000'0'00'01;
            control |= packet.rr().poll() ? 0b000'1'00'00 : 0;
            control |= (packet.rr().nr() << 5) & 0b111'0'00'00;
            ret.push_back(control);
        } else {
            uint8_t control1 = 0b000'0'00'01;
            uint8_t control2 = 0;
            control2 |= packet.rr().poll() ? 1 : 0;
            control2 |= (packet.rr().nr() << 1) & 0xfe;
            ret.push_back(control1);
            ret.push_back(control2);
        }
    }
    if (packet.has_rej()) {
        if (!packet.rr_extseq()) {
            uint8_t control = 0b000'0'10'01;
            control |= packet.rej().poll() ? 0b000'1'00'00 : 0;
            control |= (packet.rej().nr() << 5) & 0b111'0'00'00;
            ret.push_back(control);
        } else {
            uint8_t control1 = 0b000'0'10'01;
            uint8_t control2 = 0;
            control2 |= packet.rej().poll() ? 1 : 0;
            control2 |= (packet.rej().nr() << 1) & 0xfe;
            ret.push_back(control1);
            ret.push_back(control2);
        }
    }

    // I frames.
    if (packet.has_iframe()) {
        if (!packet.rr_extseq()) {
            uint8_t control = 0;
            control |= packet.iframe().poll() ? 0b00010000 : 0;
            control |= packet.iframe().nr() << 5;
            control |= (packet.iframe().ns() << 1) & 0b00001110;
            ret.push_back(control);
        } else {
            uint8_t control1 = 0;
            uint8_t control2 = 0;
            control1 |= (packet.iframe().ns() << 1) & 0b11111110;
            control2 |= packet.iframe().poll() ? 1 : 0;
            control2 |= packet.iframe().nr() << 1;
            ret.push_back(control1);
            ret.push_back(control2);
        }
        uint8_t pid = packet.iframe().pid();
        ret.push_back(pid);
        ret.append(packet.iframe().payload());
    }
    return ret;
}

} // namespace ax25
