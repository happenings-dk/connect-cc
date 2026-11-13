#include "connectrpc/http2_stream.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace connectrpc {
namespace {
struct ObservedStream {
  std::string data;
  std::vector<Metadata> headers;
  Metadata trailers;
  int ends = 0;
  int closes = 0;
  int writable = 0;
  Status status;
};
Http2StreamEvents Observe(std::map<int32_t, ObservedStream>& streams) {
  Http2StreamEvents events;
  events.headers = [&streams](int32_t id, Metadata headers, bool trailers,
                              bool informational) {
    EXPECT_FALSE(informational);
    if (trailers)
      streams[id].trailers = std::move(headers);
    else
      streams[id].headers.push_back(std::move(headers));
  };
  events.data = [&streams](int32_t id, std::string_view bytes) {
    streams[id].data.append(bytes);
  };
  events.end = [&streams](int32_t id) { ++streams[id].ends; };
  events.close = [&streams](int32_t id, Status status) {
    ++streams[id].closes;
    streams[id].status = std::move(status);
  };
  events.writable = [&streams](int32_t id) { ++streams[id].writable; };
  return events;
}
class StreamingPair : public testing::Test {
 protected:
  void Initialize(Http2StreamOptions options = {}) {
    auto created_client = Http2StreamSession::Create(
        Http2Session::Role::kClient, options, Observe(client_seen_));
    auto created_server = Http2StreamSession::Create(
        Http2Session::Role::kServer, options, Observe(server_seen_));
    ASSERT_TRUE(created_client.ok());
    ASSERT_TRUE(created_server.ok());
    client_ = std::move(*created_client.value());
    server_ = std::move(*created_server.value());
    Pump();
  }
  void Pump(size_t fragment = 4096) {
    for (int iteration = 0; iteration < 1000; ++iteration) {
      bool progress = false;
      for (bool client_to_server : {true, false}) {
        Http2StreamSession& source = client_to_server ? *client_ : *server_;
        Http2StreamSession& sink = client_to_server ? *server_ : *client_;
        auto output = source.Drain();
        ASSERT_TRUE(output.ok()) << output.status().message;
        std::string_view bytes = *output.value();
        progress |= !bytes.empty();
        while (!bytes.empty()) {
          size_t count = std::min(fragment, bytes.size());
          ASSERT_TRUE(sink.Receive(bytes.substr(0, count)).ok());
          bytes.remove_prefix(count);
        }
      }
      if (!progress) return;
    }
    FAIL() << "HTTP/2 engines did not become idle";
  }
  int32_t Request() {
    auto id = client_->StartRequest({{":method", "POST"},
                                     {":scheme", "http"},
                                     {":authority", "localhost"},
                                     {":path", "/echo"}});
    EXPECT_TRUE(id.ok()) << id.status().message;
    return id.ok() ? *id.value() : 0;
  }
  std::map<int32_t, ObservedStream> client_seen_;
  std::map<int32_t, ObservedStream> server_seen_;
  std::unique_ptr<Http2StreamSession> client_;
  std::unique_ptr<Http2StreamSession> server_;
};
TEST_F(StreamingPair, IncrementalBidirectionalDataHalfCloseAndTrailers) {
  Initialize();
  int32_t id = Request();
  Pump(1);
  ASSERT_EQ(server_seen_[id].headers.size(), 1);
  EXPECT_EQ(server_seen_[id].ends, 0);
  ASSERT_TRUE(server_->StartResponse(id, {{":status", "200"}}).ok());
  for (int i = 0; i < 3; ++i) {
    auto request = client_->TryWrite(id, "request");
    auto response = server_->TryWrite(id, "response");
    ASSERT_TRUE(request.ok());
    ASSERT_TRUE(response.ok());
    EXPECT_EQ(*request.value(), 7);
    EXPECT_EQ(*response.value(), 8);
    Pump(1);
    ASSERT_TRUE(server_->Consume(id, 7).ok());
    ASSERT_TRUE(client_->Consume(id, 8).ok());
  }
  EXPECT_EQ(client_seen_[id].data, "responseresponseresponse");
  EXPECT_EQ(server_seen_[id].data, "requestrequestrequest");
  ASSERT_TRUE(client_->CloseSend(id).ok());
  Pump(1);
  EXPECT_EQ(server_seen_[id].ends, 1);
  EXPECT_EQ(client_seen_[id].ends, 0);
  ASSERT_TRUE(server_->TryWrite(id, "last").ok());
  ASSERT_TRUE(
      server_->CloseSend(id, {{"grpc-status", "0"}, {"cost", "1"}}).ok());
  Pump(1);
  EXPECT_EQ(client_seen_[id].data, "responseresponseresponselast");
  EXPECT_EQ(client_seen_[id].trailers,
            (Metadata{{"grpc-status", "0"}, {"cost", "1"}}));
  EXPECT_EQ(client_seen_[id].ends, 1);
  EXPECT_EQ(client_seen_[id].closes, 1);
  EXPECT_EQ(server_seen_[id].closes, 1);
  EXPECT_TRUE(client_seen_[id].status.ok());
  EXPECT_TRUE(server_seen_[id].status.ok());
  EXPECT_TRUE(client_->Consume(id, 4).ok());
  EXPECT_TRUE(client_->Release(id).ok());
  EXPECT_TRUE(server_->Release(id).ok());
}
TEST_F(StreamingPair, SlowReaderStopsPeerAtWindowAndBoundsWriterQueue) {
  Http2StreamOptions options;
  options.max_queued_bytes = 32768;
  Initialize(options);
  int32_t slow = Request();
  Pump();
  std::string chunk(32768, 'x');
  for (int i = 0; i < 2; ++i) {
    auto written = client_->TryWrite(slow, chunk);
    ASSERT_TRUE(written.ok());
    ASSERT_EQ(*written.value(), chunk.size());
    Pump();
  }
  EXPECT_EQ(server_seen_[slow].data.size(), 65535);
  auto partial = client_->TryWrite(slow, chunk);
  ASSERT_TRUE(partial.ok());
  EXPECT_EQ(*partial.value(), 32767);
  auto blocked = client_->TryWrite(slow, chunk);
  ASSERT_TRUE(blocked.ok());
  EXPECT_EQ(*blocked.value(), 0);
  Pump();
  EXPECT_EQ(server_seen_[slow].data.size(), 65535);
  // A different stream still progresses while the first consumer is stalled.
  int32_t fast = Request();
  ASSERT_TRUE(client_->TryWrite(fast, "fast").ok());
  ASSERT_TRUE(client_->CloseSend(fast).ok());
  Pump();
  EXPECT_EQ(server_seen_[fast].data, "fast");
  ASSERT_TRUE(server_->Consume(slow, 32768).ok());
  Pump();
  EXPECT_EQ(server_seen_[slow].data.size(), 98303);
  EXPECT_GT(client_seen_[slow].writable, 0);
  EXPECT_FALSE(server_->Consume(slow, 65536).ok());
  auto resumed = client_->TryWrite(slow, chunk);
  ASSERT_TRUE(resumed.ok());
  EXPECT_EQ(*resumed.value(), 32768);
}
TEST_F(StreamingPair, ReleasingClosedUnreadDataRestoresConnectionCredit) {
  Http2StreamOptions options;
  options.connection_window_bytes = 65535;
  Initialize(options);
  int32_t first = Request();
  ASSERT_TRUE(client_->TryWrite(first, std::string(65535, 'x')).ok());
  ASSERT_TRUE(client_->CloseSend(first).ok());
  Pump();
  ASSERT_EQ(server_seen_[first].data.size(), 65535);
  ASSERT_TRUE(server_->StartResponse(first, {{":status", "200"}}).ok());
  ASSERT_TRUE(server_->CloseSend(first).ok());
  Pump();
  ASSERT_EQ(server_seen_[first].closes, 1);
  int32_t second = Request();
  ASSERT_TRUE(client_->TryWrite(second, "after release").ok());
  Pump();
  EXPECT_TRUE(server_seen_[second].data.empty());
  ASSERT_TRUE(server_->Release(first).ok());
  Pump();
  EXPECT_EQ(server_seen_[second].data, "after release");
}
TEST_F(StreamingPair, ResetAndConnectionFailureCompleteEachStreamOnce) {
  Initialize();
  int32_t reset = Request();
  int32_t other = Request();
  Pump();
  EXPECT_FALSE(client_->Release(reset).ok());
  ASSERT_TRUE(client_->Reset(reset).ok());
  Pump();
  EXPECT_EQ(client_seen_[reset].closes, 1);
  EXPECT_EQ(server_seen_[reset].closes, 1);
  EXPECT_EQ(server_seen_[reset].status.code, Code::kCanceled);
  EXPECT_EQ(client_seen_[other].closes, 0);
  client_->Fail({Code::kUnavailable, "disconnected", {}});
  EXPECT_EQ(client_seen_[reset].closes, 1);
  EXPECT_EQ(client_seen_[other].closes, 1);
  EXPECT_EQ(client_seen_[other].status.code, Code::kUnavailable);
  EXPECT_FALSE(client_->StartRequest({}).ok());
  EXPECT_TRUE(client_->Release(reset).ok());
  EXPECT_TRUE(client_->Release(other).ok());
}
TEST_F(StreamingPair, MetadataAndRetainedStreamLimitsAreEnforced) {
  Http2StreamOptions options;
  options.max_streams = 1;
  options.max_metadata_bytes = 512;
  Initialize(options);
  int32_t first = Request();
  EXPECT_EQ(client_->StartRequest({}).status().code, Code::kResourceExhausted);
  Pump();
  EXPECT_EQ(server_
                ->StartResponse(first, {{":status", "200"},
                                        {"large", std::string(512, 'x')}})
                .code,
            Code::kResourceExhausted);
  EXPECT_FALSE(client_->CloseSend(first, {{":status", "200"}}).ok());
  ASSERT_TRUE(client_->Reset(first).ok());
  Pump();
  EXPECT_EQ(client_->StartRequest({}).status().code, Code::kResourceExhausted);
  EXPECT_TRUE(client_->Release(first).ok());
  EXPECT_TRUE(server_->Release(first).ok());
  EXPECT_GT(Request(), first);
}
TEST_F(StreamingPair, GoawayRejectsNewCallsAndPreservesAcceptedStream) {
  Initialize();
  int32_t id = Request();
  Pump();
  ASSERT_TRUE(server_->Shutdown().ok());
  Pump();
  EXPECT_EQ(client_->StartRequest({}).status().code, Code::kUnavailable);
  EXPECT_EQ(client_seen_[id].closes, 0);
  ASSERT_TRUE(client_->TryWrite(id, "finish accepted call").ok());
  ASSERT_TRUE(client_->CloseSend(id).ok());
  ASSERT_TRUE(server_->StartResponse(id, {{":status", "200"}}).ok());
  ASSERT_TRUE(server_->CloseSend(id).ok());
  Pump();
  EXPECT_EQ(client_seen_[id].closes, 1);
  EXPECT_TRUE(client_seen_[id].status.ok());
}
}  // namespace
}  // namespace connectrpc
