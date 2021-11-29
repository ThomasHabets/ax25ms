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
// Local headers.
#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"
#include "proto/gen/ax25.grpc.pb.h"
#include "proto/gen/ax25.pb.h"

#include <grpcpp/grpcpp.h>

#include <unistd.h>
#include <thread>

namespace {
[[noreturn]] void usage(const char* av0, int err)
{
    std::cout << av0 << ": Usage [ -h ] -s <mycall> -r <router host:port>\n";
    exit(err);
}
} // namespace

int wrapmain(int argc, char** argv)
{
    std::string router;
    std::string src;

    {
        int opt;
        while ((opt = getopt(argc, argv, "hr:s:")) != -1) {
            switch (opt) {
            case 'r':
                router = optarg;
                break;
            case 's':
                src = optarg;
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
    if (src.empty()) {
        std::cerr << "Need to specify src call (-s)\n";
        return EXIT_FAILURE;
    }
    if (optind != argc) {
        std::cerr << "Extra args on cmdline\n";
        return EXIT_FAILURE;
    }

    // Connect to service.
    auto channel = grpc::CreateChannel(router, grpc::InsecureChannelCredentials());
    auto seq = ax25ms::SeqPacketService::NewStub(channel);

    std::clog << "Accepting…\n";
    grpc::ClientContext ctx;
    ax25ms::SeqAcceptRequest req;
    req.mutable_packet()->mutable_metadata()->mutable_source_address()->set_address(src);
    ax25ms::SeqAcceptResponse resp;
    auto stream = seq->Accept(&ctx);
    if (!stream->Write(req)) {
        std::clog << "Failed to start Accept RPC: " << stream->Finish().error_message()
                  << "\n";
        return 1;
    }
    bool connected = false;

    // Send command.
    std::jthread th([&stream] {
        for (;;) {
            std::string cmd;
            std::getline(std::cin, cmd);
            if (cmd.empty()) {
                return;
            }

            grpc::ClientContext ctx;
            ax25ms::SeqAcceptRequest req;
            req.mutable_packet()->set_payload(cmd);
            if (!stream->Write(req)) {
                std::cerr << "Failed to write command\n";
                exit(1);
            }
        }
    });

    while (stream->Read(&resp)) {
        // First frame.
        if (!connected) {
            // TODO: check connected.
            connected = true;
            std::clog << "Accepted!\n";
        }
        if (resp.has_packet()) {
            std::cout << "Got data: " << resp.packet().payload() << "\n";
        }
    }
    const auto st = stream->Finish();
    if (!st.ok()) {
        std::cerr << "Streaming failed: " << st.error_message() << "\n";
        exit(1);
    }
    std::cerr << "Stream exit success\n";
    return 0;
}
