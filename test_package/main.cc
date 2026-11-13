#include <asio/io_context.hpp>
#include <string_view>

#include "connectrpc/callback.h"
#include "connectrpc/codec.h"
#include "connectrpc/http1.h"
#include "connectrpc/http2.h"
#include "connectrpc/rpc.h"
#include "connectrpc/status.h"
#include "google/protobuf/wrappers.pb.h"

int main() {
  using Message = google::protobuf::StringValue;
  Message message;
  message.set_value("package consumer");
  auto encoded = connectrpc::EncodeMessage(message, connectrpc::Codec::kJson);
  if (!encoded.ok()) return 1;
  Message decoded;
  if (!connectrpc::DecodeMessage(*encoded.value(), connectrpc::Codec::kJson,
                                 decoded)
           .ok() ||
      decoded.value() != message.value())
    return 1;
  auto parser = connectrpc::Http1Parser::Create(connectrpc::Http1Role::kClient);
  if (!parser.ok()) return 1;
  std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  if (!(*parser.value())->ReadOne(response).ok()) return 1;
  auto connection = connectrpc::Http2Session::Create(
      connectrpc::Http2Session::Role::kClient, 1024, 1024,
      [](connectrpc::HttpMessage) {});
  if (!connection.ok()) return 1;
  asio::io_context loop;
  connectrpc::Client client({});
  bool completed = false;
  connectrpc::Async(
      loop.get_executor(),
      client.Unary<Message, Message>("/test.Service/Unary", message),
      [&completed](auto result) {
        completed = result.status.code == connectrpc::Code::kFailedPrecondition;
      });
  loop.run();
  return completed ? 0 : 1;
}
