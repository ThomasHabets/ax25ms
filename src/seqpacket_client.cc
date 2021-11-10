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

#include <google/protobuf/text_format.h>
#include <grpcpp/grpcpp.h>

#include <unistd.h>

namespace {
[[noreturn]] void usage(const char* av0, int err)
{
    std::cout << av0 << ": Usage [ -h ] -s <mycall> -r <router host:port> dst\n";
    exit(err);
}
} // namespace

int main(int argc, char** argv)
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
    if (optind + 1 != argc) {
        std::cerr << "Want exactly one non-opt arg: the DST call\n";
        return EXIT_FAILURE;
    }
    const std::string dst = argv[optind];

    // Connect to service.
    auto channel = grpc::CreateChannel(router, grpc::InsecureChannelCredentials());
    auto seq = ax25ms::SeqPacketService::NewStub(channel);

    grpc::ClientContext ctx;
    ax25ms::SeqConnectRequest req;
    req.mutable_packet()->mutable_metadata()->mutable_source_address()->set_address(src);
    req.mutable_packet()->mutable_metadata()->mutable_address()->set_address(dst);
    ax25ms::SeqConnectResponse resp;
    auto stream = seq->Connect(&ctx);
    if (!stream->Write(req)) {
        std::clog << "Failed to start connect RPC: " << stream->Finish().error_message()
                  << "\n";
        return 1;
    }
    bool connected = false;
    while (stream->Read(&resp)) {
        // First frame.
        if (!connected) {
            // TODO: check connected.
            connected = true;
            std::clog << "Connected!\n";
            continue;
        }
        // Data.
    }
    const auto st = stream->Finish();
    if (st.ok()) {
        std::cerr << "Streaming failed: " << st.error_message() << "\n";
        return 1;
    }
    std::cerr << "Exit success\n";
}
