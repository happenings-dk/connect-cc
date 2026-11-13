#include "connectrpc/http2.h"

#include <memory>
#include <vector>

#include "gtest/gtest.h"
namespace connectrpc {
namespace {
TEST(Http2, MultiplexedUnaryWithFragmentedFrames) {
  std::vector<HttpMessage> responses;
  auto client_result =
      Http2Session::Create(Http2Session::Role::kClient, 1024 * 1024, 16384,
                           [&responses](HttpMessage message) {
                             responses.push_back(std::move(message));
                           });
  ASSERT_TRUE(client_result.ok());
  Http2Session* server_ptr = nullptr;
  auto server_result = Http2Session::Create(
      Http2Session::Role::kServer, 1024 * 1024, 16384,
      [&server_ptr](HttpMessage message) {
        EXPECT_TRUE(server_ptr
                        ->Respond(message.stream_id,
                                  {{":status", "200"},
                                   {"content-type", "application/proto"}},
                                  message.body)
                        .ok());
      });
  ASSERT_TRUE(server_result.ok());
  server_ptr = server_result.value()->get();
  Http2Session& client = **client_result.value();
  Http2Session& server = **server_result.value();
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(client
                    .Request({{":method", "POST"},
                              {":scheme", "http"},
                              {":authority", "localhost"},
                              {":path", "/test.Service/Echo"},
                              {"content-type", "application/proto"}},
                             std::string(i * 70000, 'a' + i))
                    .ok());
  }
  for (int iteration = 0; iteration < 100 && responses.size() < 3;
       ++iteration) {
    auto requests = client.Drain();
    ASSERT_TRUE(requests.ok());
    for (char byte : *requests.value()) {
      ASSERT_TRUE(server.Receive(std::string_view(&byte, 1)).ok());
    }
    auto replies = server.Drain();
    ASSERT_TRUE(replies.ok());
    for (char byte : *replies.value()) {
      ASSERT_TRUE(client.Receive(std::string_view(&byte, 1)).ok());
    }
  }
  ASSERT_EQ(responses.size(), 3);
  for (const auto& response : responses) {
    int index = (response.stream_id - 1) / 2;
    EXPECT_EQ(response.body, std::string(index * 70000, 'a' + index));
    EXPECT_EQ(response.headers.front(),
              std::make_pair(std::string(":status"), std::string("200")));
  }
}
TEST(Http2, InvalidRoleAndOversizedRequest) {
  auto result = Http2Session::Create(Http2Session::Role::kClient, 1, 1024,
                                     [](HttpMessage) {});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ((*result.value())->Request({}, "ab").status().code,
            Code::kResourceExhausted);
  EXPECT_EQ((*result.value())->Respond(1, {}, "").code,
            Code::kFailedPrecondition);
}
TEST(Http2, ResetNotifiesOnceAndConnectionRemainsUsable) {
  int failures = 0;
  int responses = 0;
  auto client = Http2Session::Create(
      Http2Session::Role::kClient, 1024, 1024,
      [&responses](HttpMessage) { ++responses; },
      [&failures](int32_t, Status status) {
        ++failures;
        EXPECT_EQ(status.code, Code::kCanceled);
      });
  ASSERT_TRUE(client.ok());
  Http2Session* peer = nullptr;
  auto server = Http2Session::Create(
      Http2Session::Role::kServer, 1024, 1024, [&peer](HttpMessage message) {
        if (message.stream_id == 1)
          EXPECT_TRUE(peer->Reset(1).ok());
        else
          EXPECT_TRUE(
              peer->Respond(message.stream_id, {{":status", "200"}}, "ok")
                  .ok());
      });
  ASSERT_TRUE(server.ok());
  peer = server.value()->get();
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE((*client.value())
                    ->Request({{":method", "POST"},
                               {":scheme", "http"},
                               {":authority", "localhost"},
                               {":path", "/echo"}},
                              "")
                    .ok());
  }
  for (int i = 0; i < 5; ++i) {
    auto request = (*client.value())->Drain();
    ASSERT_TRUE(request.ok());
    ASSERT_TRUE(peer->Receive(*request.value()).ok());
    auto reply = peer->Drain();
    ASSERT_TRUE(reply.ok());
    ASSERT_TRUE((*client.value())->Receive(*reply.value()).ok());
  }
  EXPECT_EQ(failures, 1);
  EXPECT_EQ(responses, 1);
}
TEST(Http2, OversizedResponseNeverDelivered) {
  int failures = 0;
  auto client = Http2Session::Create(
      Http2Session::Role::kClient, 2, 1024,
      [](HttpMessage) { ADD_FAILURE() << "oversized response delivered"; },
      [&failures](int32_t, Status status) {
        ++failures;
        EXPECT_EQ(status.code, Code::kResourceExhausted);
      });
  ASSERT_TRUE(client.ok());
  Http2Session* peer = nullptr;
  auto server = Http2Session::Create(
      Http2Session::Role::kServer, 1024, 1024, [&peer](HttpMessage message) {
        EXPECT_TRUE(
            peer->Respond(message.stream_id, {{":status", "200"}}, "big").ok());
      });
  ASSERT_TRUE(server.ok());
  peer = server.value()->get();
  ASSERT_TRUE((*client.value())
                  ->Request({{":method", "POST"},
                             {":scheme", "http"},
                             {":authority", "localhost"},
                             {":path", "/echo"}},
                            "")
                  .ok());
  for (int i = 0; i < 5; ++i) {
    auto request = (*client.value())->Drain();
    ASSERT_TRUE(request.ok());
    ASSERT_TRUE(peer->Receive(*request.value()).ok());
    auto reply = peer->Drain();
    ASSERT_TRUE(reply.ok());
    ASSERT_TRUE((*client.value())->Receive(*reply.value()).ok());
  }
  EXPECT_EQ(failures, 1);
}
TEST(Http2, GoawayRejectsNewStreams) {
  auto client = Http2Session::Create(Http2Session::Role::kClient, 1024, 1024,
                                     [](HttpMessage) {});
  auto server = Http2Session::Create(Http2Session::Role::kServer, 1024, 1024,
                                     [](HttpMessage) {});
  ASSERT_TRUE(client.ok());
  ASSERT_TRUE(server.ok());
  auto preface = (*client.value())->Drain();
  ASSERT_TRUE(preface.ok());
  ASSERT_TRUE((*server.value())->Receive(*preface.value()).ok());
  ASSERT_TRUE((*server.value())->Shutdown().ok());
  auto reply = (*server.value())->Drain();
  ASSERT_TRUE(reply.ok());
  ASSERT_TRUE((*client.value())->Receive(*reply.value()).ok());
  EXPECT_EQ((*client.value())->Request({}, "").status().code,
            Code::kUnavailable);
}
}  // namespace
}  // namespace connectrpc
