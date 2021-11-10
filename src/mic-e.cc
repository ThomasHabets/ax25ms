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
 * Parse APRS MicE messages.
 * Main spec doc: http://www.aprs.org/doc/APRS101.PDF
 */
#include "proto/gen/aprs.pb.h"
#include "proto/gen/ax25.pb.h"
#include <grpcpp/grpcpp.h>
#include <string_view>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

std::string trim(std::string_view in);

namespace mic_e {

// APRS101.pdf chapter 10.

struct byte1_6_data_t {
    char lat_digit;
    int message;
    bool custom;     // 0 means N/A
    char ns;         // '_' means invalid in this position
    int long_offset; // -0 means invalid in this position
    char ew;         // '_' means invalid in this position
};
std::map<char, byte1_6_data_t> byte1_6_data{
    { '0', { '0', 0, 0, 'S', 0, 'E' } },       { '1', { '1', 0, 0, 'S', 0, 'E' } },
    { '2', { '2', 0, 0, 'S', 0, 'E' } },       { '3', { '3', 0, 0, 'S', 0, 'E' } },
    { '4', { '4', 0, 0, 'S', 0, 'E' } },       { '5', { '5', 0, 0, 'S', 0, 'E' } },
    { '6', { '6', 0, 0, 'S', 0, 'E' } },       { '7', { '7', 0, 0, 'S', 0, 'E' } },
    { '8', { '8', 0, 0, 'S', 0, 'E' } },       { '9', { '9', 0, 0, 'S', 0, 'E' } },
    { 'A', { '0', 1, true, '_', -0, '_' } },   { 'B', { '1', 1, true, '_', -0, '_' } },
    { 'C', { '2', 1, true, '_', -0, '_' } },   { 'D', { '3', 1, true, '_', -0, '_' } },
    { 'E', { '4', 1, true, '_', -0, '_' } },   { 'F', { '5', 1, true, '_', -0, '_' } },
    { 'G', { '6', 1, true, '_', -0, '_' } },   { 'H', { '7', 1, true, '_', -0, '_' } },
    { 'I', { '8', 1, true, '_', -0, '_' } },   { 'J', { '9', 1, true, '_', -0, '_' } },
    { 'K', { ' ', 1, true, '_', -0, '_' } },   { 'L', { ' ', 0, 0, 'S', 0, 'E' } },
    { 'P', { '0', 1, false, 'N', 100, 'W' } }, { 'Q', { '1', 1, false, 'N', 100, 'W' } },
    { 'R', { '2', 1, false, 'N', 100, 'W' } }, { 'S', { '3', 1, false, 'N', 100, 'W' } },
    { 'T', { '4', 1, false, 'N', 100, 'W' } }, { 'U', { '5', 1, false, 'N', 100, 'W' } },
    { 'V', { '6', 1, false, 'N', 100, 'W' } }, { 'W', { '7', 1, false, 'N', 100, 'W' } },
    { 'X', { '8', 1, false, 'N', 100, 'W' } }, { 'Y', { '9', 1, false, 'N', 100, 'W' } },
    { 'Z', { ' ', 1, false, 'N', 100, 'W' } },
};

struct mice_dst {
    std::string lat;
    char ns;
    char ew;
    int long_offset;
    int message;
    bool custom;
    bool standard;
    // TODO: enum field for custom/standard/unknown-only
};

std::pair<mice_dst, grpc::Status> dst_decode(std::string_view dst)
{
    mice_dst ret{};
    for (int i = 0; i < 6; i++) {
        const auto ch = dst.at(i);
        const auto mi = byte1_6_data.find(ch);
        if (mi == byte1_6_data.end()) {
            return { ret,
                     grpc::Status(grpc::INVALID_ARGUMENT,
                                  "invalid character in mic-e digit") };
        }
        const auto& m = mi->second;

        ret.lat.push_back(m.lat_digit);
        auto cfcb = [&ret](bool c) {
            if (c) {
                ret.custom = true;
            } else {
                ret.standard = true;
            }
        };
        switch (i) {
        case 0:
            ret.message |= m.message ? 4 : 0;
            cfcb(m.custom);
            break;
        case 1:
            ret.message |= m.message ? 2 : 0;
            cfcb(m.custom);
            break;
        case 2:
            ret.message |= m.message ? 1 : 0;
            cfcb(m.custom);
            break;
        case 3:
            ret.ns = m.ns;
            break;
        case 4:
            ret.long_offset = m.long_offset;
            break;
        case 5:
            ret.ew = m.ew;
            break;
        }
    }
    return { ret, grpc::Status::OK };
}

std::string lon_decode(std::string_view in) { return "TODO"; }

std::string abc_decode(int abc, bool custom, bool standard)
{
    if (custom && standard) {
        return "unknown";
    }
    // APRS101.pdf table on page 45
    std::map<std::pair<int, bool>, std::string> mice_message_types{
        { { 7, false }, "M0: Off Duty" },   { { 6, false }, "M1: En Route" },
        { { 5, false }, "M2: In Service" }, { { 4, false }, "M3: Returning" },
        { { 3, false }, "M4: Committed" },  { { 2, false }, "M5: Special" },
        { { 1, false }, "M6: Priority" },   { { 7, true }, "C0: Custom-0" },
        { { 6, true }, "C1: Custom-1" },    { { 5, true }, "C2: Custom-2" },
        { { 4, true }, "C3: Custom-3" },    { { 3, true }, "C4: Custom-4" },
        { { 2, true }, "C5: Custom-5" },    { { 1, true }, "C6: Custom-6" },
        { { 0, false }, "Emergency" },
    };
    return mice_message_types[std::make_pair(abc, custom)];
}

int long_deg(bool long_offset, unsigned char ch)
{
    if (long_offset) {
        if (ch >= 118 && ch <= 127) {
            return ch - 118;
        }
        if (ch >= 108 && ch <= 117) {
            return 100 + (ch - 108);
        }
        if (ch >= 38 && ch <= 107) {
            return 110 + (ch - 38);
        }
    } else {
        if (ch >= 38 && ch <= 127) {
            return 10 + (ch - 38);
        }
    }
    throw std::runtime_error(std::string("long_deg out of bounds: ") +
                             std::to_string(long_offset) + " " + std::to_string(ch));
}


int long_minute(unsigned char ch)
{
    if (ch >= 88 && ch <= 97) {
        return ch - 88;
    }
    if (ch >= 38 && ch <= 87) {
        return ch - 37;
    }
    return 0;
    throw std::runtime_error(std::string("long_minute out of bounds: ") +
                             std::to_string(ch));
}

bool set_longitude(aprs::Packet::Position& pos,
                   bool long_offset,
                   std::string_view data,
                   char ew)
{
    int deg = long_deg(long_offset, data.at(0));
    int min = long_minute(data.at(1));
    int centimin = data.at(2) - 28;
    std::stringstream ss;
    // if (deg < 90) {
    ss << deg << "'" << std::setw(2) << std::setfill('0') << min << "." << centimin << ew;
    /*  } else {
    ss << (180-deg) << "'"  << std::setw(2) << std::setfill('0')
       << (60-min) << "." << (100-centimin) << "W";
       }*/
    pos.set_lng(ss.str());
    return true;
}

bool set_speed_course(aprs::Packet::Course& course, std::string_view data)
{
    return true;
}

std::pair<aprs::Packet, grpc::Status> parse(const ax25::Packet& packet)
{
    aprs::Packet ret;
    if (!packet.has_ui()) {
        return {
            ret, grpc::Status(grpc::INVALID_ARGUMENT, "mic-e can't parse non-ui frames")
        };
    }
    if (packet.dst().size() != 6) {
        return { ret, grpc::Status(grpc::INVALID_ARGUMENT, "dst too short to be mic-e") };
    }
    // Destination:
    // * 6 lat digits
    // * mic-e message ID
    // * N/S, E/W
    // * long offs indicator
    // * Digipeater path code
    const auto [dst, status] = dst_decode(packet.dst());
    if (!status.ok()) {
        return { ret, status };
    }

    // Info field
    // * encoded long
    // * encoded course and speed
    // * optionally: mic-e telemetry or status text string
    auto& position = *ret.mutable_position();
    ret.mutable_mic_e()->set_msg(abc_decode(dst.message, dst.custom, dst.standard));
    auto lat = trim(dst.lat);
    if (lat.size() > 4) {
        lat.insert(4, 1, '.');
    }
    if (lat.size() > 2) {
        lat.insert(2, 1, '\'');
    }
    position.set_lat(lat);
    position.mutable_lat()->push_back(dst.ns);
    position.mutable_lng()->push_back(dst.ew);
    auto& payload = packet.ui().payload();

    if (packet.ui().pid() != 0xF0) {
        return { ret,
                 grpc::Status(grpc::INVALID_ARGUMENT, "payload was not 0xF0 (no L3)") };
    }

    int pos = 0;
    switch (payload.at(pos++)) {
    case 0x60:
        ret.mutable_mic_e()->set_position_current(true);
        break;
    case '\'':
        break;
    default:
        return { ret, grpc::Status(grpc::INVALID_ARGUMENT, "not a mic-e packet") };
    }

    // TODO:
    // payload.substr(2,5) // longitude
    // payload.substr(5,8) // speed & course
    // payload.at(8) // symbol code
    // payload.at(9) // symbol table id
    if (!set_longitude(*ret.mutable_position(),
                       dst.long_offset == 100,
                       payload.substr(pos, 3),
                       dst.ew)) {
        return { ret, grpc::Status(grpc::INVALID_ARGUMENT, "bad longitude") };
    }
    pos += 3;
    if (!set_speed_course(*ret.mutable_course(), payload.substr(pos, 3))) {
        return { ret, grpc::Status(grpc::INVALID_ARGUMENT, "bad longitude") };
    }
    pos += 3;

    // Symbol.
    ret.mutable_position()->set_symbol(payload.substr(pos, 2));
    {
        auto& poss = *ret.mutable_position()->mutable_symbol();
        std::swap(poss[0], poss[1]);
    }
    pos += 2;

    // Telemetry
    if (payload.at(pos) == ',') {
        // Telemetry.
        ret.mutable_telemetry(); // TODO
    } else if (payload.at(pos) == '`') {
        // Telemetry.
        pos += 1 + 2 * 2;
        ret.mutable_telemetry(); // TODO

    } else if (payload.at(pos) == 0x1d) {
        // Telemetry.
        pos += 1 + 5 * 1;
        ret.mutable_telemetry(); // TODO

    } else if (payload.at(pos) == '\'') {
        // Telemetry.
        ret.mutable_telemetry(); // TODO
        pos += 1 + 5 * 2;
    }

    // Status text.
    // std::cout << payload << "\n";
    ret.set_status(payload.substr(pos));

    return { ret, grpc::Status::OK };
}

} // namespace mic_e
