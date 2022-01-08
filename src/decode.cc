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
 * Tool:
 *
 * Decode AX.25 packet deeply (including APRS, if it's APRS), and print it.
 *
 */
#include "aprs.h"
#include "mic-e.h"
#include "parse.h"
#include "util.h"

#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"
#include "proto/gen/ax25.pb.h"
#include <grpcpp/grpcpp.h>

#include <unistd.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace {

bool fcs = true; // -f to set false

[[noreturn]] void usage(const char* av0, int err)
{
    std::cout << "Usage: " << av0 << ": [ -fh ] <input files...>\n";
    exit(err);
}


std::string printable(std::string_view sv)
{
    std::stringstream ss;
    for (auto ch : sv) {
        if (std::isprint(ch)) {
            ss << ch;
        } else {
            ss << "…";
        }
    }
    return ss.str();
}

std::string aprs_software_dst(std::string_view dst)
{
    // APRS101.pdf section 4.
    std::map<std::string, std::string> prefixes{
        { "APC", "APRS/CE, Windows CE" },         //
        { "APD", "Linux aprsd server" },          //
        { "APE", "PIC-Encoder" },                 //
        { "API", "Icom radios (future)" },        //
        { "APIC", "ICQ messaging" },              //
        { "APK", "Kenwood radios" },              //
        { "APM", "MacAPRS" },                     //
        { "APP", "pocketAPRS" },                  //
        { "APR", "APRSdos" },                     //
        { "APRS", "older versions of APRSdos" },  //
        { "APRSM", "older versions of MacAPRS" }, //
        { "APRSW", "older versions of WinAPRS" }, //
        { "APS", "APRS+SA" },                     //
        { "APW", "WinAPRS" },                     //
        { "APX", "X-APRS" },                      //
        { "APY", "Yaesu radios (future)" },       //
        { "APZ", "Experimental" },                //
    };

    // TODO: return version numbers, making sure only version number
    // follows
    for (auto& [k, v] : prefixes) {
        if (dst.substr(0, k.length()) == k) {
            return v;
        }
    }
    return "";
}

std::string stringify(const ax25::Packet& packet)
{
    std::stringstream ss;

    if (true) {
        return ax25ms::proto2string(packet) +
               "-----------------------------------------------------\n";
    }

    ss << "AX25\n"
       << "  From: " << packet.src() << "\n"
       << "  To: " << packet.dst();
    if (auto sw = aprs_software_dst(packet.dst()); !sw.empty()) {
        ss << " (" << sw << ")";
    }
    ss << "\n";
    for (const auto& digi : packet.repeater()) {
        ss << "  Repeater: " << digi.address() << (digi.has_been_repeated() ? "*" : "")
           << "\n";
    }
    if (packet.has_ui()) {
        ss << "  UI\n"
           << "  Pid: " << std::hex << packet.ui().pid() << "\n"
           << "  Payload (" << std::dec << packet.ui().payload().size()
           << "): " << printable(packet.ui().payload()) << "\n";

        if (packet.has_aprs()) {
            const auto& aprs = packet.aprs();
            ss << "  APRS\n";
            if (aprs.has_position()) {
                ss << "    Lat:  " << aprs.position().lat() << "\n"
                   << "    Long: " << aprs.position().lng() << "\n"
                   << "    Symbol: " << printable(aprs.position().symbol()) << "\n";
            }
            ss << "    Status: " << printable(aprs.status()) << "\n";
            if (aprs.has_mic_e()) {
                ss << "    Mic-E\n"
                   << "      Position current: " << aprs.mic_e().position_current()
                   << "\n"
                   << "      Mic-E message: " << aprs.mic_e().msg() << "\n";
            }
            if (aprs.has_msg()) {
                auto& msg = aprs.msg();
                ss << "    MSG\n"
                   << "      Dst: <" << msg.dst() << ">\n"
                   << "      MsgNo: <" << msg.msg_number() << ">\n"
                   << "      Msg: <" << msg.msg() << ">\n";
            }
        }
    }
    if (packet.has_sabm()) {
        ss << "  SABM\n";
    }
    if (packet.has_disc()) {
        ss << "  DISC\n";
    }
    return ss.str();
}
} // namespace

int wrapmain(int argc, char** argv)
{
    {
        int opt;
        while ((opt = getopt(argc, argv, "hf")) != -1) {
            switch (opt) {
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
            case 'f':
                fcs = false;
                break;
            default:
                usage(argv[0], EXIT_FAILURE);
            }
        }
    }

    for (int i = optind; i < argc; i++) {
        const std::string fn = argv[i];
        const auto [payload, ok] = [fn]() -> std::pair<std::string, bool> {
            std::ifstream f(fn);
            if (!f.good()) {
                std::cerr << "Failed to open " << fn << ": " << strerror(errno) << "\n";
                return { "", false };
            }
            std::stringstream buf;
            buf << f.rdbuf();
            return { buf.str(), true };
        }();
        if (!ok) {
            continue;
        }
        auto [packet, status1] = ax25::parse(payload, fcs);
        if (!status1.ok()) {
            std::cerr << "Failed to parse packet in " << fn << ": "
                      << status1.error_message() << "\n";
            continue;
        }

        // Try to parse as Mic-E.
        auto [me, status] = mic_e::parse(packet);
        if (status.ok()) {
            *packet.mutable_aprs() = me;
        } else if (packet.has_ui() && packet.ui().pid() == 0xf0) {
            const auto [ap, status] = aprs::parse(packet.ui().payload());
            if (status.ok()) {
                *packet.mutable_aprs() = ap;
            } else {
                std::cout << "APRS error: " << status.error_message() << "\n";
            }
        }
        std::cout << "// file: " << fn << "\n" << stringify(packet);
    }
    return 0;
}
