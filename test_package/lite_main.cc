#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>

#include "connectrpc/grpc_protocol.h"
#include "connectrpc/managed_transport.h"
#include "connectrpc/streaming_protocol.h"
#include "message.pb.h"

int main() {
#ifndef CONNECTCPP_PROTOBUF_LITE
  return 1;
#else
  example::Message message;
  message.set_value("Lite-only installed consumer");
  auto bytes = connectrpc::EncodeMessage(message, connectrpc::Codec::kProtobuf);
  if (!bytes.ok()) return 1;
  example::Message decoded;
  if (!connectrpc::DecodeMessage(*bytes.value(), connectrpc::Codec::kProtobuf,
                                 decoded)
           .ok() ||
      decoded.value() != message.value())
    return 1;
  connectrpc::StreamCompletion completion;
  completion.status = {connectrpc::Code::kPermissionDenied,
                       "no access",
                       {{"example.Detail", "binary"}}};
  auto terminal = connectrpc::EncodeEndStream(completion);
  auto trailers = connectrpc::EncodeGrpcTrailers(completion);
  if (!terminal.ok() || !trailers.ok()) return 1;
  auto error = connectrpc::DecodeGrpcTrailers(*trailers.value());
  if (!error.ok() || error.value()->status.details.size() != 1) return 1;
  asio::io_context loop;
  auto transport = connectrpc::ManagedTransport::Create(loop.get_executor(),
                                                        "https://localhost");
  if (!transport.ok()) return 1;
  bool closed = false;
  asio::co_spawn(loop,
                 (*transport.value())->Close(std::chrono::steady_clock::now()),
                 [&](auto exception, connectrpc::Status status) {
                   closed = !exception && status.ok();
                 });
  loop.run();
  return closed ? 0 : 1;
#endif
}
