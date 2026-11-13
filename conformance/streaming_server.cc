#include <asio/as_tuple.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <string>
#include <utility>

#include "conformance/adapter.h"

namespace connectrpc::conformance_adapter {
namespace {
namespace pb = conformance::v1;
using Info = pb::ConformancePayload::RequestInfo;
void SetContext(const CallContext& context, Info& info) {
  WriteHeaders(context.headers, *info.mutable_request_headers());
  if (context.deadline != std::chrono::steady_clock::time_point::max())
    info.set_timeout_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                            context.deadline - std::chrono::steady_clock::now())
                            .count());
}
Status Error(const pb::Error& error, const Info* info = nullptr) {
  Status status{static_cast<Code>(error.code()), error.message(), {}};
  for (const auto& detail : error.details()) {
    std::string type = detail.type_url();
    type.erase(0, type.find_last_of('/') + 1);
    status.details.push_back({std::move(type), detail.value()});
  }
  if (info) {
    std::string bytes;
    if (!info->SerializeToString(&bytes))
      return {Code::kInternal, "cannot encode request info", {}};
    status.details.push_back(
        {std::string(info->GetTypeName()), std::move(bytes)});
  }
  return status;
}
asio::awaitable<Status> Delay(uint32_t milliseconds) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    co_return Status{Code::kCanceled, "handler canceled", {}};
  if (!milliseconds) co_return Status{};
  asio::steady_timer timer(co_await asio::this_coro::executor);
  timer.expires_after(std::chrono::milliseconds(milliseconds));
  auto [error] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
  if (error) co_return Status{Code::kCanceled, error.message(), {}};
  co_return Status{};
}
asio::awaitable<StreamCompletion> ServerStream(
    Request<pb::ServerStreamRequest> request,
    TypedStream<pb::ServerStreamResponse, pb::ServerStreamRequest> stream) {
  co_await asio::this_coro::throw_if_cancelled(false);
  const auto& definition = request.message.response_definition();
  Info info;
  SetContext({request.headers, request.deadline}, info);
  if (!info.add_requests()->PackFrom(request.message))
    co_return StreamCompletion{{Code::kInternal, "cannot pack request", {}},
                               {}};
  StreamCompletion result;
  result.trailers = ReadHeaders(definition.response_trailers());
  result.status =
      co_await stream.SendHeaders(ReadHeaders(definition.response_headers()));
  if (!result.status.ok()) co_return result;
  for (int i = 0; i < definition.response_data_size(); ++i) {
    pb::ServerStreamResponse response;
    response.mutable_payload()->set_data(definition.response_data(i));
    if (i == 0) *response.mutable_payload()->mutable_request_info() = info;
    result.status = co_await Delay(definition.response_delay_ms());
    if (result.status.ok()) result.status = co_await stream.Write(response);
    if (!result.status.ok()) co_return result;
  }
  if (definition.has_error())
    result.status =
        Error(definition.error(),
              definition.response_data_size() == 0 ? &info : nullptr);
  co_return result;
}
asio::awaitable<Response<pb::ClientStreamResponse>> ClientStream(
    CallContext context,
    TypedStream<pb::ClientStreamResponse, pb::ClientStreamRequest> stream) {
  co_await asio::this_coro::throw_if_cancelled(false);
  Response<pb::ClientStreamResponse> result;
  pb::UnaryResponseDefinition definition;
  Info info;
  for (;;) {
    auto next = co_await stream.Read();
    if (!next.ok()) {
      result.status = next.status();
      co_return result;
    }
    if (!*next.value()) break;
    if (info.requests_size() == 0)
      definition = (**next.value()).response_definition();
    if (!info.add_requests()->PackFrom(**next.value())) {
      result.status = {Code::kInternal, "cannot pack request", {}};
      co_return result;
    }
  }
  SetContext(context, info);
  result.headers = ReadHeaders(definition.response_headers());
  result.trailers = ReadHeaders(definition.response_trailers());
  result.status = co_await Delay(definition.response_delay_ms());
  if (!result.status.ok()) co_return result;
  if (definition.has_error()) {
    result.status = Error(definition.error(), &info);
  } else {
    result.message.emplace();
    result.message->mutable_payload()->set_data(definition.response_data());
    *result.message->mutable_payload()->mutable_request_info() =
        std::move(info);
  }
  co_return result;
}
asio::awaitable<StreamCompletion> BidiStream(
    CallContext context,
    TypedStream<pb::BidiStreamResponse, pb::BidiStreamRequest> stream) {
  co_await asio::this_coro::throw_if_cancelled(false);
  pb::StreamResponseDefinition definition;
  Info info;
  bool first = true;
  bool full_duplex = false;
  int sent = 0;
  StreamCompletion result;
  auto send = [&](bool include_requests) -> asio::awaitable<Status> {
    pb::BidiStreamResponse response;
    response.mutable_payload()->set_data(definition.response_data(sent));
    if (sent == 0) SetContext(context, info);
    if (sent == 0 || include_requests)
      *response.mutable_payload()->mutable_request_info() = info;
    Status status = co_await Delay(definition.response_delay_ms());
    if (status.ok()) status = co_await stream.Write(response);
    if (status.ok()) {
      ++sent;
      info.Clear();
    }
    co_return status;
  };
  for (;;) {
    auto next = co_await stream.Read();
    if (!next.ok()) {
      result.status = next.status();
      co_return result;
    }
    if (!*next.value()) break;
    const auto& request = **next.value();
    if (first) {
      first = false;
      definition = request.response_definition();
      full_duplex = request.full_duplex();
      result.trailers = ReadHeaders(definition.response_trailers());
      if (full_duplex) {
        result.status = co_await stream.SendHeaders(
            ReadHeaders(definition.response_headers()));
        if (!result.status.ok()) co_return result;
      }
    }
    if (!info.add_requests()->PackFrom(request))
      co_return StreamCompletion{{Code::kInternal, "cannot pack request", {}},
                                 {}};
    if (full_duplex) {
      if (sent == definition.response_data_size()) break;
      result.status = co_await send(true);
      if (!result.status.ok()) co_return result;
    }
  }
  if (!full_duplex) {
    result.status =
        co_await stream.SendHeaders(ReadHeaders(definition.response_headers()));
    if (!result.status.ok()) co_return result;
  }
  while (sent < definition.response_data_size()) {
    result.status = co_await send(false);
    if (!result.status.ok()) co_return result;
  }
  if (definition.has_error()) {
    if (sent == 0) SetContext(context, info);
    result.status = Error(definition.error(), sent == 0 ? &info : nullptr);
  }
  co_return result;
}
}  // namespace
Status RegisterStreamingService(Server& server) {
  Status status = server.RegisterServerStream<pb::ServerStreamRequest,
                                              pb::ServerStreamResponse>(
      "/connectrpc.conformance.v1.ConformanceService/ServerStream",
      ServerStream);
  if (!status.ok()) return status;
  status = server.RegisterClientStream<pb::ClientStreamRequest,
                                       pb::ClientStreamResponse>(
      "/connectrpc.conformance.v1.ConformanceService/ClientStream",
      ClientStream);
  if (!status.ok()) return status;
  return server
      .RegisterBidiStream<pb::BidiStreamRequest, pb::BidiStreamResponse>(
          "/connectrpc.conformance.v1.ConformanceService/BidiStream",
          BidiStream);
}
}  // namespace connectrpc::conformance_adapter
