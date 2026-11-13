#ifndef CONNECTCPP_CONFORMANCE_ADAPTER_H_
#define CONNECTCPP_CONFORMANCE_ADAPTER_H_

#include <asio/awaitable.hpp>
#include <string>

#include "connectrpc/conformance/v1/client_compat.pb.h"
#include "connectrpc/conformance/v1/server_compat.pb.h"
#include "connectrpc/rpc.h"

namespace connectrpc::conformance_adapter {
// Four-byte big-endian framing; a clean EOF is distinct from a truncated frame.
Result<bool> ReadFrame(google::protobuf::Message& message);
Status WriteFrame(const google::protobuf::Message& message);
Metadata ReadHeaders(
    const google::protobuf::RepeatedPtrField<conformance::v1::Header>& headers);
void WriteHeaders(
    const Metadata& metadata,
    google::protobuf::RepeatedPtrField<conformance::v1::Header>& headers);
asio::awaitable<conformance::v1::ClientCompatResponse> RunClient(
    conformance::v1::ClientCompatRequest request);
asio::awaitable<conformance::v1::ClientCompatResponse> RunStreamingClient(
    conformance::v1::ClientCompatRequest request);
asio::awaitable<conformance::v1::ClientCompatResponse> InvokeUnaryClient(
    const conformance::v1::ClientCompatRequest& request, Client& client);
asio::awaitable<Status> RunServer(conformance::v1::ServerCompatRequest request);
Status RegisterStreamingService(Server& server);
}  // namespace connectrpc::conformance_adapter
#endif  // CONNECTCPP_CONFORMANCE_ADAPTER_H_
