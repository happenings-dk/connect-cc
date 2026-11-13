#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <memory>
#include <string_view>
#include <utility>

#include "connectrpc/callback.h"
#include "connectrpc/cors.h"
#include "connectrpc/envelope.h"
#include "connectrpc/grpc_protocol.h"
#include "connectrpc/grpc_web_protocol.h"
#include "connectrpc/http1.h"
#include "connectrpc/http1_connection.h"
#include "connectrpc/http2_connection.h"
#include "connectrpc/http_connection.h"
#include "connectrpc/message_stream.h"
#include "connectrpc/rpc.h"
#include "google/protobuf/wrappers.pb.h"

int main() {
  auto encoded = connectrpc::EncodeEnvelope({false, false, "hello"},
                                            connectrpc::Protocol::kConnect);
  if (!encoded.ok()) return 1;
  auto terminal = connectrpc::EncodeEndStream({});
  if (!terminal.ok()) return 1;
  connectrpc::StreamCompletion grpc_error;
  grpc_error.status = {connectrpc::Code::kPermissionDenied,
                       "no access",
                       {{"example.Detail", "detail"}}};
  auto grpc_trailers = connectrpc::EncodeGrpcTrailers(grpc_error);
  if (!grpc_trailers.ok()) return 1;
  auto decoded_grpc = connectrpc::DecodeGrpcTrailers(*grpc_trailers.value());
  if (!decoded_grpc.ok() || decoded_grpc.value()->status.details.size() != 1)
    return 1;
  auto web_trailers = connectrpc::EncodeGrpcWebTrailers(grpc_error);
  if (!web_trailers.ok()) return 1;
  connectrpc::StreamingDecoder web_decoder(true, false, {},
                                           connectrpc::Protocol::kGrpcWeb);
  std::string_view web_bytes = *web_trailers.value();
  if (!web_decoder.ReadOne(web_bytes).ok()) return 1;
  auto web_end = web_decoder.Finish();
  if (!web_end.ok() || web_end.value()->status.code != grpc_error.status.code)
    return 1;
  connectrpc::StreamingDecoder decoder(true, false);
  std::string_view terminal_bytes = *terminal.value();
  if (!decoder.ReadOne(terminal_bytes).ok() || !decoder.Finish().ok()) return 1;
  connectrpc::Http1Encoder http1(connectrpc::Http1Role::kServer);
  auto http1_headers =
      http1.Headers({{":status", "200"}, {"content-length", "0"}});
  if (!http1_headers.ok() || !http1.End().ok()) return 1;
  auto http1_parser =
      connectrpc::Http1Parser::Create(connectrpc::Http1Role::kClient);
  if (!http1_parser.ok()) return 1;
  std::string_view http1_bytes = *http1_headers.value();
  auto http1_event = (*http1_parser.value())->ReadOne(http1_bytes);
  if (!http1_event.ok() || !*http1_event.value() ||
      (**http1_event.value()).kind != connectrpc::Http1Event::Kind::kHeaders)
    return 1;
  http1_event = (*http1_parser.value())->ReadOne(http1_bytes);
  if (!http1_event.ok() || !*http1_event.value() ||
      (**http1_event.value()).kind != connectrpc::Http1Event::Kind::kEnd)
    return 1;
  // Compile and link the installed streaming API without starting network I/O.

  asio::io_context loop;
  google::protobuf::StringValue get_input;
  get_input.set_value("cached value");
  auto get_payload = connectrpc::EncodeDeterministicMessage(
      get_input, connectrpc::Codec::kProtobuf);
  if (!get_payload.ok()) return 1;
  connectrpc::CallOptions get_options;
  get_options.use_get = true;
  auto get_request = connectrpc::EncodeUnaryRequest(
      "/test.Service/Read", *get_payload.value(), get_options,
      {connectrpc::Idempotency::kNoSideEffects});
  if (!get_request.ok()) return 1;
  auto get_decoded = connectrpc::DecodeUnaryRequest(*get_request.value(), 1024);
  if (!get_decoded.ok() || !get_decoded.value()->is_get ||
      get_decoded.value()->payload != *get_payload.value())
    return 1;
  auto http1_connection = connectrpc::Http1Connection::Create(
      asio::ip::tcp::socket(loop), connectrpc::Http1Role::kClient);
  if (!http1_connection.ok()) return 1;
  auto connection = connectrpc::HttpConnection::Create(
      asio::ip::tcp::socket(loop), connectrpc::HttpConnection::Role::kClient,
      connectrpc::HttpVersion::kHttp2);
  connectrpc::Client client({});
  using Message = google::protobuf::StringValue;
  connectrpc::Server server;
  auto registered = server.RegisterBidiStream<Message, Message>(
      "/test.Service/Stream",
      [](connectrpc::CallContext, connectrpc::TypedStream<Message, Message>)
          -> asio::awaitable<connectrpc::StreamCompletion> {
        co_return connectrpc::StreamCompletion{};
      });
  if (!registered.ok()) return 1;
  connectrpc::CorsOptions cors_options;
  cors_options.allowed_origins = {"https://app.example"};
  cors_options.exposed_headers = {"trailer-request-id"};
  auto cors = connectrpc::CorsPolicy::Create(std::move(cors_options));
  if (!cors.ok()) return 1;
  auto preflight = cors.value()->Preflight(
      {{":method", "OPTIONS"},
       {"origin", "https://app.example"},
       {"access-control-request-method", "POST"},
       {"access-control-request-headers", "content-type"}});
  if (!preflight || preflight->headers.front().second != "204") return 1;
  // Retain and link the public shared serving/router operations without
  // starting I/O on the deliberately unconnected socket.
  if (connection.ok()) {
    auto serving = connectrpc::ServeHttpConnection(
        *connection.value(),
        cors.value()->Wrap(
            [&server](std::shared_ptr<connectrpc::ByteStream> stream) {
              return server.HandleHttpStream(std::move(stream));
            }));
    (void)serving;
  }
  bool missing_transport_reported = false;
  bool missing_unary_transport_reported = false;
  connectrpc::Async(
      loop.get_executor(),
      client.Unary<Message, Message>("/test.Service/Unary", Message{}),
      [&missing_unary_transport_reported](auto result) {
        missing_unary_transport_reported =
            result.status.code == connectrpc::Code::kFailedPrecondition;
      });
  asio::co_spawn(loop,
                 client.BidiStream<Message, Message>("/test.Service/Stream"),
                 [&missing_transport_reported](auto exception, auto result) {
                   missing_transport_reported =
                       !exception && result.status().code ==
                                         connectrpc::Code::kFailedPrecondition;
                 });
  loop.run();
  return connection.ok() && missing_transport_reported &&
                 missing_unary_transport_reported
             ? 0
             : 1;
}
