/*
   Copyright 2022 Google LLC

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
#include "aprs.h"
#include "serialize.h"
#include "util.h"

#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"

#include <iostream>
#include <string>

#include <unistd.h>

namespace {
const char* argv0 = nullptr;

using subcommand_t = std::function<int(ax25::Packet&, int, char**)>;

void usage(int err)
{
    std::cout << "Usage: " << argv0 << " [ -r <rpt,rpt> ] <src> <command>…\n"
              << "  Commands\n"
              << "    aprs           Create APRS messages\n"
              << "  Options:\n"
              << "    -r <rpt,rpt>   Comma separated list of digipeaters\n";
    exit(err);
}

void usage_aprs(int err)
{
    std::cout << "Usage: " << argv0 << " [ -r <rpt,rpt> ] <src> aprs <subcommand>…\n"
              << "  Subcommands\n"
              << "    msg   Create station message\n";
    exit(err);
}

void usage_aprs_msg(int err)
{
    std::cout << "Usage: " << argv0
              << " [ -r <rpt,rpt> ] <src> aprs msg [-n <num>] <dst> <msg>…\n"
              << "  Options:\n"
              << "    -n <num>   Message number\n";
    exit(err);
}

int cmd_aprs_msg(ax25::Packet& packet, int argc, char** argv)
{
    int opt;
    std::string num;
    while ((opt = getopt(argc, argv, "+hn:")) != -1) {
        switch (opt) {
        case 'h':
            usage_aprs_msg(EXIT_SUCCESS);
        case 'n':
            num = optarg;
            break;
        default:
            usage_aprs_msg(EXIT_FAILURE);
        }
    }
    if (argc < optind + 2) {
        usage_aprs_msg(EXIT_FAILURE);
    }
    const std::string dst = argv[optind];
    const std::string text = argv[optind + 1];
    auto& msg = *packet.mutable_aprs()->mutable_msg();
    msg.set_dst(dst);
    msg.set_msg(text);
    if (!num.empty()) {
        msg.set_msg_number(num);
    }
    return 0;
}

int cmd_aprs(ax25::Packet& packet, int argc, char** argv)
{
    int opt;
    optind = 0;
    while ((opt = getopt(argc, argv, "+h")) != -1) {
        switch (opt) {
        case 'h':
            usage_aprs(EXIT_SUCCESS);
        default:
            std::cerr << "Bad option to aprs\n";
            usage_aprs(EXIT_FAILURE);
        }
    }
    if (argc == optind) {
        std::cerr << "Wrong number of args to aprs\n";
        usage_aprs(EXIT_FAILURE);
    }
    const std::string cmd = argv[optind];
    const std::map<std::string, subcommand_t> cbs{
        { "msg", cmd_aprs_msg },
    };
    auto cb = cbs.find(cmd);
    if (cb == cbs.end()) {
        std::cerr << "Unknown aprs subcommand " << cmd << "\n";
        usage_aprs(EXIT_FAILURE);
    }
    packet.set_dst("APZ001"); // TODO: Per APRS101.PDF page 14, this is "experimental".
    packet.set_command_response_la(true);
    auto& ui = *packet.mutable_ui();
    ui.set_pid(0xF0);
    int ret = cb->second(packet, argc - 1, &argv[optind]);
    ui.set_payload(aprs::serialize(packet.aprs()));
    return ret;
}

std::vector<std::string_view> split(std::string_view s)
{
    std::vector<std::string_view> ret;
    const std::string delim = ",";

    auto start = 0U;
    auto end = s.find(delim);
    while (end != std::string::npos) {
        ret.push_back(s.substr(start, end - start));
        start = end + delim.length();
        end = s.find(delim, start);
    }
    if (start != end) {
        ret.push_back(s.substr(start, end - start));
    }
    return ret;
}

} // namespace

int wrapmain(int argc, char** argv)
{
    argv0 = argv[0];
    ax25::Packet packet;
    int opt;
    while ((opt = getopt(argc, argv, "+hr:")) != -1) {
        switch (opt) {
        case 'h':
            usage(EXIT_SUCCESS);
        case 'r': {
            const std::string oa = optarg;
            for (const auto& s : split(oa)) {
                auto& r = *packet.add_repeater();
                r.set_address(s.data(), s.size());
            }
            break;
        }
        default:
            std::cerr << "Bad option\n";
            usage(EXIT_FAILURE);
        }
    }
    if (argc < optind + 1) {
        usage(EXIT_FAILURE);
    }
    const std::string src = argv[optind];
    const std::string cmd = argv[optind + 1];

    const std::map<std::string, subcommand_t> cbs{
        { "aprs", cmd_aprs },
    };
    auto cb = cbs.find(cmd);
    if (cb == cbs.end()) {
        std::cerr << "Unknown command " << cmd << "\n";
        usage(EXIT_FAILURE);
    }
    packet.set_src(src);
    int ret = cb->second(packet, argc - optind - 1, &argv[optind + 1]);
    std::cout << ax25::serialize(packet, true);
    return ret;
}
