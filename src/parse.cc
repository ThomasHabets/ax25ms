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
 * Parse AX.25 packets.
 * Main specs: http://www.tapr.org/pdf/AX25.2.2.pdf
 */
// org/info/freqspec.txt
#include "util.h"

#include "proto/gen/ax25.pb.h"

#include <grpcpp/grpcpp.h>
#include <cinttypes>
#include <regex>
#include <string>

std::string trim(std::string_view in)
{
    if (in.empty()) {
        return "";
    }
    const std::string spaces = " ";
    const auto s = in.find_first_not_of(spaces);
    const auto e = in.find_last_not_of(spaces);
    return std::string(in.substr(s, e + 1));
}

namespace ax25 {

constexpr int modulus_normal = 8;
constexpr int modulus_extended = 128;

namespace {
// Return call, command/response or has-been-repeated, status, done.
std::tuple<std::string, bool, grpc::Status, bool> parse_call(const std::string& d)
{
    std::string call;
    for (int i = 0; i < 6; i++) {
        const char ch = (d.at(i) >> 1) & ~0x80;
        const char bit = d.at(i) & 1;
        if (bit) {
            return {
                "", false, grpc::Status(grpc::UNKNOWN, "callsign msb was not 0"), false
            };
        }
        // std::cerr << "byte:
        if (ch != ' ') {
            call.push_back(ch);
        }
    }
    const int ssid = (d.at(6) >> 1) & 15;
    const bool done = d.at(6) & 1;
    // const int rr = (d.at(66) >> 5) & 0x3;
    const bool top_bit = d.at(6) & 0x80;

    if (ssid) {
        call += "-" + std::to_string(ssid);
    }
    // std::cerr << "parsed: <" << call << ">" << call.size() << "\n";
    return { call, top_bit, grpc::Status::OK, done };
}


} // namespace

std::pair<std::string, bool> normalize_call(std::string_view in)
{
    static std::regex callRE("([A-Z0-9 ]{2,6})(?:-(\\d{1,2}))? {0,5}");
    std::smatch m;
    auto s = std::string(in);
    if (!std::regex_match(s, m, callRE)) {
        return { "", false };
    }
    const auto c = m[1].str();
    const auto ssids = m[2].str();

    char* end = nullptr;
    auto ssid = strtoul(ssids.c_str(), &end, 10);
    if (*end) {
        throw std::runtime_error("ssid is not a number: " + ssids);
    }
    if (ssid > 15) {
        throw std::runtime_error("ssid too high: " + ssids);
    }
    if (!ssid) {
        return { trim(c), true };
    }
    return { trim(c) + "-" + std::to_string(ssid), true };
}

// Return packet, success.
std::pair<ax25::Packet, grpc::Status> parse(const std::string& data)
{
    ax25::Packet ret;
    // 3.9a. 136/8-2 bytes.
    if (data.size() < 15) {
        return { ret, grpc::Status(grpc::UNKNOWN, "packet too small") };
    }

    int pos = 0;

    {
        const auto [dst, top, err, done] = parse_call(data.substr(pos));
        if (!err.ok()) {
            return { ret, err };
        }
        ret.set_dst(dst);
        ret.set_command_response(top);
        pos += 7;
        if (done) {
            // Dst must not be 'done'.
            return { ret, grpc::Status(grpc::UNKNOWN, "dst address marked with 'done'") };
        }
    }

    // Source.
    auto [src, top, err, done] = parse_call(data.substr(pos));
    if (!err.ok()) {
        return { ret, err };
    }
    ret.set_src(src);
    ret.set_command_response_la(top);
    pos += 7;

    // AX.25 spec says (3.12.4) that up to two repeaters can be added.
    // In practice packets seem to have an unbounded path.
    //
    // The spec also says that paths are being phased out. But APRS
    // relies heavily on paths, so I don't see that happening.
    while (!done) {
        const auto [digi, top, err, d] = parse_call(data.substr(pos));
        done = d;
        if (!err.ok()) {
            return { ret, err };
        }
        auto rpt = ret.add_repeater();
        rpt->set_has_been_repeated(top);
        rpt->set_address(digi);
        pos += 7;
    }
    const uint8_t control = data.at(pos++);

    if ((control & 1) == 0) {
        // I frame.

        auto& iframe = *ret.mutable_iframe();
        iframe.set_pid(data.at(pos++));
        const int modulus = modulus_normal; // TODO: detect?
        if (modulus == modulus_normal) {
            iframe.set_nr((control >> 5) & 0x7);
            iframe.set_poll(control & 0b00010000);
            iframe.set_ns((control >> 1) & 0x7);
            iframe.set_payload(data.substr(pos));
        } else {
            // iframe.set_extended();
            return { ret,
                     grpc::Status(grpc::UNIMPLEMENTED,
                                  "128-modulus frames not implemented") };
        }
        return { ret, grpc::Status::OK };

    } else if ((control & 3) == 1) {
        // S frame.
        ax25::Packet_SFrame* s;
        switch (control & 0b000'0'11'00) {
        case 0b000'0'00'00: // RR
            s = ret.mutable_rr();
            break;
        case 0b000'0'01'00: // RNR
            s = ret.mutable_rnr();
            break;
        case 0b000'0'10'00: // REJ
            s = ret.mutable_rej();
            break;
        case 0b000'0'11'00: // SREJ
            s = ret.mutable_srej();
            break;
        default:
            throw std::logic_error("can't happen");
        }
        s->set_nr((control >> 5) & 7);
        s->set_poll(control & 0b00010000);
        const auto rest = data.substr(pos);
        if (!rest.empty()) {
            std::clog << "S Frame with data? I see " << ax25ms::str2hex(data) << "\n";
        }
        return { ret, grpc::Status::OK };
    }

    // control & 3 == 3
    // U frame.
    switch (control & 0b11101111) { // 4.3.3
    case 0b000'0'0011: {            // UI
        auto ui = ret.mutable_ui();
        ui->set_pid(static_cast<unsigned int>(data.at(pos++)) & 0xFF);
        ui->set_payload(data.substr(pos));
        ui->set_push(control & 0b00010000);
        break;
    }
    case 0b011'0'11'11: // SABME
        ret.mutable_sabme()->set_poll(control & 0b00010000);
        // TODO: info field not allowed
        break;
    case 0b001'0'11'11: // SABM
        ret.mutable_sabm()->set_poll(control & 0b00010000);
        // TODO: info field not allowed
        break;
    case 0b010'0'00'11: // DISC
        // TODO: info field not allowed
        ret.mutable_disc()->set_poll(control & 0b00010000);
        break;
    case 0b000'0'11'11: // DM
        ret.mutable_dm()->set_poll(control & 0b00010000);
        // TODO: info field not allowed
        break;
    case 0b011'0'00'11: // UA
        ret.mutable_ua()->set_poll(control & 0b00010000);
        // TODO: info field not allowed
        break;
    case 0b111'0'11'11: { // TEST
        auto test = ret.mutable_test();
        test->set_push(control & 0b00010000);
        test->set_info(data.substr(pos));
        break;
    }
    case 0b100'0'01'11: // FRMR
    case 0b101'0'11'11: // XID
        return { ret, grpc::Status(grpc::UNIMPLEMENTED, "frame type not implemented") };
        break;
    default:
        // Unknown U frame.
        return { ret, grpc::Status(grpc::INVALID_ARGUMENT, "unknown U frame") };
    }
    return { ret, grpc::Status::OK };
}
} // namespace ax25
