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
#include "fdwrap.h"
#include "parse_service.h"
#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"
#include "util.h"
#include <condition_variable>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <cstdint>
#include <iomanip>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace ax25ms::router::hub {

[[noreturn]] void usage(const char* av0, int err)
{
    std::cout << av0 << ": Usage [ -h ] [ -l <listen addr>\n";
    exit(err);
}

class Hub final : public ax25ms::RouterService::Service
{
public:
    Hub() { log() << "Hub starting..."; }

    grpc::Status StreamFrames(grpc::ServerContext* ctx,
                              const ax25ms::StreamRequest* req,
                              grpc::ServerWriter<ax25ms::Frame>* writer) override
    {
        try {
            log() << "Registering listener";
            {
                std::lock_guard<std::mutex> l(mu_);
                clients_.insert(writer);
            }
            for (;;) {
                {
                    std::lock_guard<std::mutex> l(mu_);
                    if (ctx->IsCancelled()) {
                        break;
                    }
                }
                sleep(1);
            }
            clients_.erase(writer);
            log() << "Stream stop";
            return grpc::Status::OK;
        } catch (const std::exception& e) {
            log() << "Exception: " << e.what();
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, e.what());
        } catch (...) {
            log() << "Unknown Exception";
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "unknown exception");
        }
    }

    grpc::Status Send(grpc::ServerContext* ctx,
                      const ax25ms::SendRequest* req,
                      ax25ms::SendResponse* out) override
    {
        std::unique_lock<std::mutex> lk(mu_);
        log() << "Packet being sent to " << clients_.size() << " clients";
        for (auto& c : clients_) {
            ax25ms::SendResponse resp;
            if (!c->Write(req->frame())) {
                log() << "failed to send to a client";
            }
        }
        log() << "… done";
        return grpc::Status::OK;
    }

private:
    std::mutex mu_;
    std::set<grpc::ServerWriter<ax25ms::Frame>*> clients_;
};

} // namespace ax25ms::router::hub

int wrapmain(int argc, char** argv)
{
    using namespace ax25ms::router::hub;
    using ax25ms::log;

    std::string listen = "[::]:12345";
    {
        int opt;
        while ((opt = getopt(argc, argv, "hl:")) != -1) {
            switch (opt) {
            case 'l':
                listen = optarg;
                break;
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
            default:
                usage(argv[0], EXIT_FAILURE);
            }
        }
    }
    if (optind != argc) {
        log() << "Invalid extra args on the command line";
        return EXIT_FAILURE;
    }

    Hub service;

    const std::string addr(listen);
    grpc::ServerBuilder builder;
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 500);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 1000);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_BDP_PROBE, 1);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
                               500);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_SENT_PING_INTERVAL_WITHOUT_DATA_MS,
                               1000);
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto parser = make_parse_service();
    builder.RegisterService(parser.get());
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    log() << "Running…";
    server->Wait();
    return 0;
}
