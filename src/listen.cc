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
 * Tool
 *
 * Listen to all traffic that goes through a service that implements the Router
 * Service. This includes the serial port tool.
 */
#include "mic-e.h"

#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"
#include "proto/gen/ax25.pb.h"
#include <grpcpp/grpcpp.h>
#include <unistd.h>
#include <fstream>
#include <string>

namespace ax25 {
std::pair<ax25::Packet, grpc::Status> parse(const std::string& data);
}

namespace {
[[noreturn]] void usage(const char* av0, int err)
{
    std::cout << av0 << ": Usage [ -h ] -r <router host:port>\n";
    exit(err);
}

void run(grpc::ClientReader<ax25ms::Frame>* reader)
{
    ax25ms::Frame frame;
    while (reader->Read(&frame)) {
        auto& payload = frame.payload();
        std::clog << "Got frame size " << payload.size() << ": " << payload << "\n";

        auto [packet, status] = ax25::parse(frame.payload());
        if (!status.ok()) {
            std::cerr << "Failed to parse packet: " << status.error_message() << "\n";
        } else {
            std::cout << "  src: " << packet.src() << "\n"
                      << "  dst: " << packet.dst() << "\n";
            for (const auto& digi : packet.repeater()) {
                std::cout << "  repeater: " << digi.address()
                          << (digi.has_been_repeated() ? "*" : "") << "\n";
            }
            const auto [me, status] = mic_e::parse(packet);
            if (status.ok()) {
                std::cout << "  mic-e message: " << me.status() << "\n";
            }
        }

        std::ofstream fo("captured/" + std::to_string(time(nullptr)));
        fo << payload;
    }
    std::clog << "Stream finished\n";
    auto status = reader->Finish();
    if (!status.ok()) {
        std::cerr << "stream failed: " << status.error_message() << "\n";
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::string router;
    {
        int opt;
        while ((opt = getopt(argc, argv, "hr:")) != -1) {
            switch (opt) {
            case 'r':
                router = optarg;
                break;
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
            default:
                usage(argv[0], EXIT_FAILURE);
            }
        }
    }
    if (router.empty()) {
        std::cerr << "Need to specify router (-r)\n";
        return EXIT_FAILURE;
    }
    if (optind != argc) {
        std::cerr << "Invalid extra args on the command line\n";
        return EXIT_FAILURE;
    }

    std::clog << "Starting...\n";

    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 500);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
    args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    args.SetInt(GRPC_ARG_HTTP2_BDP_PROBE, 1);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    args.SetInt(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 500);
    args.SetInt(GRPC_ARG_HTTP2_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS, 1000);

    auto channel =
        grpc::CreateCustomChannel(router, grpc::InsecureChannelCredentials(), args);

    std::unique_ptr<ax25ms::RouterService::Stub> stub{ ax25ms::RouterService::NewStub(
        channel) };
    std::clog << "Connected?\n";

    ax25ms::StreamRequest req;

    std::clog << "Starting stream...\n";
    for (;;) {
        grpc::ClientContext ctx;
        auto reader = stub->StreamFrames(&ctx, req);
        std::clog << "Streaming...\n";
        run(reader.get());
        sleep(1);
    }
}
