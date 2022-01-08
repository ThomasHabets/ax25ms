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
#include "fcs.h"
#include "util.h"

#include "proto/gen/ax25.pb.h"

#include <grpcpp/grpcpp.h>

#include <cinttypes>
#include <regex>
#include <string>

using ax25ms::log;

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
std::tuple<std::string, bool, bool, bool, grpc::Status, bool>
parse_call(std::string_view d)
{
    std::string call;
    for (int i = 0; i < 6; i++) {
        const char ch = (d.at(i) >> 1) & ~0x80;
        const char bit = d.at(i) & 1;
        if (bit) {
            if (false) {
                // For debugging it can be worth trying to continue even
                // with this error.
                log() << "Callsign msb was not 0 at pos " << i << ": "
                      << ax25ms::str2hex(d);
            } else {
                return { "",
                         false,
                         false,
                         false,
                         grpc::Status(grpc::UNKNOWN,
                                      "callsign msb was not 0: " + ax25ms::str2hex(d)),
                         false };
            }
        }
        if (ch != ' ') {
            call.push_back(ch);
        }
    }
    const int ssid = (d.at(6) >> 1) & 15;
    const bool done = d.at(6) & 1;
    const bool rr1 = d.at(6) & 0b01000000;
    const bool rr2 = d.at(6) & 0b00100000;
    const bool top_bit = d.at(6) & 0x80;

    if (ssid) {
        call += "-" + std::to_string(ssid);
    }
    // log() << "parsed: <" << call << ">" << call.size();
    return { call, top_bit, rr1, rr2, grpc::Status::OK, done };
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

unsigned long reflect(unsigned long crc, int bitnum)
{

    // reflects the lower 'bitnum' bits of 'crc'
    unsigned long i, j = 1, crcout = 0;
    for (i = (unsigned long)1 << (bitnum - 1); i; i >>= 1) {
        if (crc & i)
            crcout |= j;
        j <<= 1;
    }
    return (crcout);
}

// Return packet, success.
std::pair<ax25::Packet, grpc::Status> parse(std::string_view fulldata, bool fcs)
{
    ax25::Packet ret;
    // 3.9a. 136/8-2 bytes.
    if (fulldata.size() < 15) {
        return { ret, grpc::Status(grpc::UNKNOWN, "packet too small") };
    }
    std::string_view data = fulldata;
    if (fcs) {
        const auto [data2, crc16, crcok] = ax25ms::check_fcs(fulldata);
        if (!crcok) {
            return { ret, grpc::Status(grpc::UNKNOWN, "CRC error") };
        }
        data = data2;
        ret.set_fcs(crc16);
    }

    int pos = 0;

    {
        const auto [dst, top, rr1, rr2, err, done] = parse_call(data.substr(pos, 7));
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
    auto [src, top, rr1, rr2, err, done] = parse_call(data.substr(pos, 7));
    if (!err.ok()) {
        return { ret, err };
    }
    ret.set_rr_extseq(!rr1);
    ret.set_src(src);
    ret.set_command_response_la(top);
    pos += 7;

    // AX.25 spec says (3.12.4) that up to two repeaters can be added.
    // In practice packets seem to have an unbounded path.
    //
    // The spec also says that paths are being phased out. But APRS
    // relies heavily on paths, so I don't see that happening.
    while (!done) {
        const auto [digi, top, rr1, rr2, err, d] = parse_call(data.substr(pos, 7));
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
        if (!ret.rr_extseq()) {
            iframe.set_nr((control >> 5) & 0x7);
            iframe.set_poll(control & 0b00010000);
            iframe.set_ns((control >> 1) & 0x7);
        } else {
            const uint8_t econtrol = data.at(pos++);
            iframe.set_extended(true);
            iframe.set_nr((econtrol >> 1) & 0x7f);
            iframe.set_poll(econtrol & 1);
            iframe.set_ns((control >> 1) & 0x7f);
        }
        iframe.set_pid(static_cast<uint8_t>(data.at(pos++)));
        const auto t = data.substr(pos);
        iframe.set_payload(t.data(), t.size());
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
            log() << "S Frame with data? I see " << ax25ms::str2hex(data);
        }
        return { ret, grpc::Status::OK };
    }

    // control & 3 == 3
    // U frame.
    switch (control & 0b11101111) { // 4.3.3
    case 0b000'0'0011: {            // UI
        auto ui = ret.mutable_ui();
        ui->set_pid(static_cast<unsigned int>(data.at(pos++)) & 0xFF);
        const auto t = data.substr(pos);
        ui->set_payload(t.data(), t.size());
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
        const auto info = data.substr(pos);
        test->set_info(info.data(), info.size());
        break;
    }
    case 0b100'0'01'11: // FRMR
    case 0b101'0'11'11: // XID
        return { ret, grpc::Status(grpc::UNIMPLEMENTED, "frame type not implemented") };
        break;
    default:
        // Unknown U frame.
        return { ret,
                 grpc::Status(grpc::INVALID_ARGUMENT,
                              "unknown U frame control " + std::to_string(control)) };
    }
    return { ret, grpc::Status::OK };
}
} // namespace ax25
