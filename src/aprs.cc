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
 * APRS packet parsing code.
 * Main spec doc: http://www.aprs.org/doc/APRS101.PDF
 *
 * Main TODOs:
 * * compressed coords: captured/1634057455
 * * BLN
 * * Station capabilities ("<")
 */
#include "parse.h"
#include "proto/gen/api.pb.h"
#include "proto/gen/ax25.pb.h"

#include <grpcpp/grpcpp.h>

#include <iomanip>
#include <regex>
#include <sstream>
#include <string>

namespace aprs {

namespace {

// Parse int and return true on success.
bool parseint(std::string_view s, std::function<void(int)> f)
{
    char* end;
    auto ss = std::string(s);
    f(strtol(ss.c_str(), &end, 10));
    return !*end;
}

// Parse int, throw on failure.
int must_parseint(std::string_view s)
{
    int ret;
    if (!parseint(s, [&ret](int i) { ret = i; })) {
        throw std::runtime_error("can't happen: must_parseint failed");
    }
    return ret;
}

// Helper function to be able to run std regex on string_view.
std::pair<std::match_results<std::string_view::const_iterator>, bool>
svregex_match(std::string_view s, const std::regex& re)
{
    std::match_results<std::string_view::const_iterator> m;
    const bool ok = std::regex_match(s.begin(), s.end(), m, re);
    return { m, ok };
}

// Return lat, long, sym, ok
std::tuple<std::string, std::string, std::string, bool>
parse_pos_sym(std::string_view data)
{
    static const std::regex posRE("(\\d{2})(\\d{2})[.](\\d{2})([NS])([1E/\\\\])" // 1 & E?
                                  "(\\d{3})(\\d{2})[.](\\d{2})([EW])(.)");
    const auto [m, ok] = svregex_match(data, posRE);
    if (!ok) {
        return { "", "", "", false };
    }
    int pos = 1;
    auto lat_degrees = m[pos++];
    auto lat_minutes = m[pos++];
    auto lat_centiminutes = m[pos++];
    auto lat_ns = m[pos++];
    auto symbol1 = m[pos++];
    auto lng_degrees = m[pos++];
    auto lng_minutes = m[pos++];
    auto lng_centiminutes = m[pos++];
    auto lng_ew = m[pos++];
    auto symbol2 = m[pos++];
    return {
        std::to_string(must_parseint(lat_degrees.str())) + "'" + lat_minutes.str() + '.' +
            lat_centiminutes.str() + lat_ns.str(),
        std::to_string(must_parseint(lng_degrees.str())) + "'" + lng_minutes.str() + '.' +
            lng_centiminutes.str() + lng_ew.str(),
        symbol1.str() + symbol2.str(),
        true,
    };
}

// Take positional data and fill it in on the packet.
grpc::Status fill_pos(aprs::Packet& packet, std::string_view data)
{
    const auto [lat, lng, sym, ok] = parse_pos_sym(data);
    if (!ok) {
        return grpc::Status(grpc::INVALID_ARGUMENT,
                            "coord parse error for <" + std::string(data) + ">");
    }
    auto& position = *packet.mutable_position();
    position.set_lat(lat);
    position.set_lng(lng);
    position.set_symbol(sym);
    return grpc::Status::OK;
}

std::pair<aprs::Packet, grpc::Status>
parse_pos_without_timestamp_msg(std::string_view data)
{
    aprs::Packet ret;
    auto err = fill_pos(ret, data.substr(0, 19));
    if (!err.ok()) {
        return { ret, err };
    }
    const auto st = data.substr(19);
    ret.set_status(st.data(), st.size());
    return { ret, grpc::Status::OK };
}

// Return day, hour, minute, and success.
std::tuple<int, int, int, bool> parse_timestamp(std::string_view data)
{
    static const std::regex tsRE(R"((\d{2})(\d{2})(\d{2})z)");
    const auto [m, ok] = svregex_match(data, tsRE);
    if (!ok) {
        return { 0, 0, 0, false };
    }
    int dd, hh, mm;
    if (!parseint(m[1].str(), [&dd](int n) { dd = n; })) {
        return { 0, 0, 0, false };
    }
    if (!parseint(m[1].str(), [&hh](int n) { hh = n; })) {
        return { 0, 0, 0, false };
    }
    if (!parseint(m[1].str(), [&mm](int n) { mm = n; })) {
        return { 0, 0, 0, false };
    }
    return { dd, hh, mm, true };
}

std::pair<aprs::Packet, grpc::Status> parse_pos_with_timestamp_msg(std::string_view data)
{
    aprs::Packet ret;
    int pos = 0;
    auto [dd, hh, mm, ok] = parse_timestamp(data.substr(0, 7));
    if (!ok) {
        return { ret, grpc::Status(grpc::INVALID_ARGUMENT, "bad timestamp") };
    }
    pos += 7;

    auto err = fill_pos(ret, data.substr(pos, 19));
    if (!err.ok()) {
        return { ret, err };
    }
    pos += 19;
    ret.set_status(std::string(data.substr(pos)));
    return { ret, grpc::Status::OK };
}


std::pair<aprs::Packet, grpc::Status> parse_pos_without_timestamp(std::string_view data)
{
    aprs::Packet ret;
    auto err = fill_pos(ret, data.substr(0, 19));
    if (!err.ok()) {
        return { ret, err };
    }
    ret.set_status(std::string(data.substr(19)));
    return { ret, grpc::Status::OK };
}

// Chapter 11.
std::pair<aprs::Packet, grpc::Status> parse_message(std::string_view data)
{
    aprs::Packet ret;
    static const std::regex msgRE(R"(([A-Z0-9 -]{9}):([^]+?)(?:\{([^]{1,5}))?)");
    const auto [m, ok] = svregex_match(data, msgRE);
    if (!ok) {
        return { ret,
                 grpc::Status(grpc::INVALID_ARGUMENT,
                              "msg parse error for <" + std::string(data) + ">") };
    }
    const auto dst = m[1].str();
    const auto msg = m[2];
    const auto msg_number = m[3];

    auto [nc, ok_call] = ax25::normalize_call(dst);
    if (!ok_call) {
        return { ret,
                 grpc::Status(grpc::INVALID_ARGUMENT,
                              "call cannot be normalized: <" + dst + ">") };
    }
    ret.mutable_msg()->set_dst(nc);
    ret.mutable_msg()->set_msg(msg);
    ret.mutable_msg()->set_msg_number(msg_number);
    return { ret, grpc::Status::OK };
}


bool set_time(aprs::Packet::Time& ts,
              std::string_view dd,
              std::string_view hh,
              std::string_view mm)
{
    if (!parseint(dd, [&ts](int i) { ts.set_day(i); })) {
        return false;
    }
    if (!parseint(hh, [&ts](int i) { ts.set_hour(i); })) {
        return false;
    }
    if (!parseint(mm, [&ts](int i) { ts.set_minute(i); })) {
        return false;
    }
    return true;
}

// Chapter 11.
std::pair<aprs::Packet, grpc::Status> parse_object(std::string_view data)
{
    aprs::Packet ret;
    static const std::regex objRE(
        // name   live   DD     HH     MM  pos/sym  xtra  comment
        R"((.{9})([*_])(\d{2})(\d{2})(\d{2})z(.{19})(.{7})([^]*))");
    const auto [m, ok] = svregex_match(data, objRE);
    if (!ok) {
        return { ret,
                 grpc::Status(grpc::INVALID_ARGUMENT,
                              "obj parse error for <" + std::string(data) + ">") };
    }
    int pos = 1;
    const auto name = m[pos++].str();
    const auto live = m[pos++].str();
    const auto dd = m[pos++].str();
    const auto hh = m[pos++].str();
    const auto mm = m[pos++].str();
    const auto pos_sym = m[pos++].str();
    const auto xtra = m[pos++].str(); // TODO: set this.
    const auto comment = m[pos++].str();

    auto [lat, lon, sym, ok_possym] = parse_pos_sym(pos_sym);
    if (!ok_possym) {
        return { ret,
                 grpc::Status(grpc::INVALID_ARGUMENT,
                              "pos_sym parse error: <" + pos_sym + ">") };
    }
    auto& obj = *ret.mutable_object();
    obj.set_name(name);
    obj.set_live(live == "*");
    if (!set_time(*obj.mutable_time(), dd, hh, mm)) {
        return { ret,
                 grpc::Status(grpc::INVALID_ARGUMENT,
                              std::string("timestamp format incorrect: ") + "<" + mm +
                                  "> " + "<" + hh + "> " + "<" + mm + ">") };
    }
    auto& coord = *obj.mutable_position();
    coord.set_lat(lat);
    coord.set_lng(lon);
    coord.set_symbol(sym);
    obj.set_comment(comment);

    return { ret, grpc::Status::OK };
}

// Chapter 16: Status reports.
std::pair<aprs::Packet, grpc::Status> parse_status_report(std::string_view data)
{
    aprs::Packet ret;
    static const std::regex statusRE(R"((?:(\d{2})(\d{2})(\d{2})z)?([^]*))");
    const auto [m, ok] = svregex_match(data, statusRE);
    if (!ok) {
        return { ret,
                 grpc::Status(grpc::INVALID_ARGUMENT,
                              "status parse error for <" + std::string(data) + ">") };
    }
    auto& sr = *ret.mutable_status_report();
    const auto dd = m[1].str();
    const auto hh = m[2].str();
    const auto mm = m[3].str();
    if (!set_time(*sr.mutable_time(), dd, hh, mm)) {
        return { ret,
                 grpc::Status(grpc::INVALID_ARGUMENT,
                              std::string("timestamp format incorrect: ") + "<" + mm +
                                  "> " + "<" + hh + "> " + "<" + mm + ">") };
    }
    sr.set_status_report(m[4]);
    return { ret, grpc::Status::OK };
}

} // namespace

std::string serialize(const aprs::Packet& aprs)
{
    if (aprs.has_msg()) {
        auto& m = aprs.msg();
        std::stringstream ss;
        ss << ":" << std::setw(9) << std::left << m.dst() << ":" << m.msg();
        if (auto n = m.msg_number(); n.size()) {
            ss << "{" << n;
        }
        return ss.str();
    }
    throw "UNIMPLEMENTED";
}

// Decode APRS from message data only.
std::pair<aprs::Packet, grpc::Status> parse(std::string_view data)
{
    aprs::Packet ret;
    if (data.empty()) {
        return { ret, grpc::Status(grpc::INVALID_ARGUMENT, "empty data") };
    }
    switch (data.at(0)) {
    // TODO: actually scan for '!' anywhere within the first 40 characters, per
    // APRS101.PDF chapter 5, page 28.
    case '!': // Position without timestamp.
        return parse_pos_without_timestamp(data.substr(1));
    case '=': // Position without timestamp.
        return parse_pos_without_timestamp_msg(data.substr(1));
    case '@':
        return parse_pos_with_timestamp_msg(data.substr(1));
    case ':':
        return parse_message(data.substr(1));
    case ';':
        return parse_object(data.substr(1));
    case '>':
        return parse_status_report(data.substr(1));
    default:
        return { ret, grpc::Status(grpc::UNIMPLEMENTED, "data type not implemented") };
    }
    return { ret, grpc::Status::OK };
}
} // namespace aprs
