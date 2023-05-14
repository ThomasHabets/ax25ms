/*
   Copyright 2023 Google LLC

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
#include "parse_service.h"

#include "parse.h"
#include "serialize.h"

namespace {
class Parser final : public ax25::AX25Parser::Service
{
public:
    grpc::Status Parse(grpc::ServerContext* ctx,
                       const ax25::ParseRequest* req,
                       ax25::ParseResponse* out) override;
    grpc::Status Serialize(grpc::ServerContext* ctx,
                           const ax25::SerializeRequest* req,
                           ax25::SerializeResponse* out) override;
};

grpc::Status Parser::Parse(grpc::ServerContext* ctx,
                           const ax25::ParseRequest* req,
                           ax25::ParseResponse* out)
{
    // TODO: support FCS.
    auto [packet, status] = ax25::parse(req->payload(), false);
    *out->mutable_packet() = packet;
    return status;
}

grpc::Status Parser::Serialize(grpc::ServerContext* ctx,
                               const ax25::SerializeRequest* req,
                               ax25::SerializeResponse* out)
{
    // TODO: support FCS.
    *out->mutable_payload() = ax25::serialize(req->packet(), false);
    return grpc::Status::OK;
}

} // namespace

std::unique_ptr<ax25::AX25Parser::Service> make_parse_service()
{
    return std::make_unique<Parser>();
}
