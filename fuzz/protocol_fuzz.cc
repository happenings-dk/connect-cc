#include <stddef.h>
#include <stdint.h>

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "connectrpc/envelope.h"
#include "connectrpc/grpc_protocol.h"
#include "connectrpc/grpc_web_protocol.h"
#include "connectrpc/http1.h"
#include "connectrpc/streaming_protocol.h"
#include "connectrpc/unary.h"

namespace {
constexpr size_t kLimit = 64 * 1024;
void Stream(std::string_view input, size_t chunk_size,
            connectrpc::Protocol protocol, bool response, bool gzip) {
  connectrpc::StreamingDecoder decoder(response, gzip, {kLimit, kLimit, kLimit},
                                       protocol);
  while (!input.empty()) {
    auto chunk = input.substr(0, chunk_size);
    input.remove_prefix(chunk.size());
    // Leave room for a buffered zero-length envelope, but catch a parser that
    // repeatedly produces events without consuming its input.
    size_t steps = chunk.size() + 4;
    while (!chunk.empty()) {
      if (steps-- == 0) std::abort();
      auto next = decoder.ReadOne(chunk);
      if (!next.ok()) return;
    }
  }
  (void)decoder.Finish();
}
void Http(std::string_view input, size_t chunk_size, bool response) {
  auto created =
      connectrpc::Http1Parser::Create(response ? connectrpc::Http1Role::kClient
                                               : connectrpc::Http1Role::kServer,
                                      {kLimit, 1024});
  if (!created.ok()) std::abort();
  auto& parser = *created.value();
  while (!input.empty()) {
    auto chunk = input.substr(0, chunk_size);
    input.remove_prefix(chunk.size());
    size_t steps = chunk.size() * 3 + 8;
    for (;;) {
      if (steps-- == 0) std::abort();
      auto next = parser->ReadOne(chunk);
      if (!next.ok()) return;
      if (!*next.value()) break;
      if ((**next.value()).kind == connectrpc::Http1Event::Kind::kEnd &&
          !parser->NextMessage().ok())
        return;
    }
  }
  (void)parser->EndInput();
}
}  // namespace
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2 || size > kLimit) return 0;
  const uint8_t mode = data[0];
  const size_t chunk_size = 1 + data[1];
  std::string_view input(reinterpret_cast<const char*>(data + 2), size - 2);
  const auto protocol = static_cast<connectrpc::Protocol>((mode >> 3) % 3);
  switch (mode % 8) {
    case 0: {
      connectrpc::EnvelopeDecoder decoder(protocol, kLimit);
      while (!input.empty()) {
        auto chunk = input.substr(0, chunk_size);
        input.remove_prefix(chunk.size());
        if (!decoder
                 .Feed(
                     chunk,
                     [](connectrpc::Envelope) { return connectrpc::Status{}; })
                 .ok())
          break;
      }
      (void)decoder.Finish();
      break;
    }
    case 1:
      Stream(input, chunk_size, protocol, mode & 32, mode & 64);
      break;
    case 2:
      Http(input, chunk_size, mode & 32);
      break;
    case 3: {
      connectrpc::GrpcWebTextDecoder decoder;
      while (!input.empty()) {
        auto chunk = input.substr(0, chunk_size);
        input.remove_prefix(chunk.size());
        if (!decoder.Read(chunk).ok()) break;
      }
      (void)decoder.Finish();
      break;
    }
    case 4:
      (void)connectrpc::DecodeError(input);
      (void)connectrpc::DecodeEndStream(input, {kLimit, kLimit, kLimit});
      (void)connectrpc::DecodeGrpcWebTrailers(input, {kLimit, kLimit, kLimit});
      break;
    case 5: {
      connectrpc::HttpMessage request;
      request.headers = {{":method", "GET"},
                         {":path", "/test.Service/Echo?" + std::string(input)}};
      (void)connectrpc::DecodeUnaryRequest(std::move(request), kLimit);
      break;
    }
    case 6: {
      const std::string value(input);
      (void)connectrpc::DecodeGrpcTimeout(value);
      (void)connectrpc::DecodeGrpcTrailers({{"grpc-status", value}},
                                           {kLimit, kLimit, kLimit});
      (void)connectrpc::DecodeGrpcTrailers({{"grpc-status", "13"},
                                            {"grpc-message", value},
                                            {"grpc-status-details-bin", value}},
                                           {kLimit, kLimit, kLimit});
      (void)connectrpc::ParseUnaryContentType(value);
      (void)connectrpc::ParseStreamingContentType(value);
      (void)connectrpc::ParseGrpcContentType(value);
      (void)connectrpc::ParseGrpcWebContentType(value);
      (void)connectrpc::DecodeStreamingRequest(
          {{":method", "POST"},
           {":path", "/test.Service/Stream"},
           {"content-type", "application/connect+proto"},
           {"connect-accept-encoding", value}});
      break;
    }
    case 7: {
      connectrpc::HttpMessage response;
      response.headers = {{":status", "200"},
                          {"content-type", "application/proto"}};
      if (mode & 32) response.headers.emplace_back("content-encoding", "gzip");
      response.body = std::string(input);
      (void)connectrpc::DecodeUnaryResponse(
          std::move(response), connectrpc::Codec::kProtobuf, kLimit);
      break;
    }
  }
  return 0;
}
