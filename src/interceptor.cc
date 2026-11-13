#include "connectrpc/interceptor.h"

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <utility>

#include "protocol_internal.h"

namespace connectrpc {
asio::awaitable<Status> CallInterceptor::OnRequest(Metadata&) {
  co_return Status{};
}
void CallInterceptor::OnResponseHeaders(Metadata&) {}
void CallInterceptor::OnResponseTrailers(Metadata&) {}
void CallInterceptor::OnMessage(bool, size_t) {}
void CallInterceptor::OnComplete(const Status&) {}
namespace internal_interceptor {
Call::Call(CallInfo info) : info_(std::move(info)) {}
Call::~Call() { Complete({Code::kCanceled, "call abandoned", {}}); }
Result<std::shared_ptr<Call>> Call::Create(
    const std::vector<Interceptor>& interceptors, CallInfo info) {
  auto call = std::shared_ptr<Call>(new Call(std::move(info)));
  call->self_ = call;
  for (const auto& factory : interceptors) {
    if (!factory)
      return Result<std::shared_ptr<Call>>(
          Status{Code::kInvalidArgument, "empty interceptor factory", {}});
    auto hook = factory(call->info_);
    if (hook) call->hooks_.push_back(std::move(hook));
  }
  return Result<std::shared_ptr<Call>>(std::move(call));
}
asio::awaitable<Result<Metadata>> Call::Start(Metadata headers) {
  return StartOwned(self_.lock(), std::move(headers));
}
asio::awaitable<Result<Metadata>> Call::RunRequest(std::shared_ptr<Call> call,
                                                   Metadata headers) {
  co_await asio::this_coro::throw_if_cancelled(false);
  for (auto& hook : call->hooks_) {
    if ((co_await asio::this_coro::cancellation_state).cancelled() !=
        asio::cancellation_type::none)
      co_return Result<Metadata>(Status{Code::kCanceled, "call canceled", {}});
    ++call->active_;
    Status status = co_await hook->OnRequest(headers);
    if (!status.ok()) co_return Result<Metadata>(std::move(status));
    status = internal_protocol::ValidateMetadata(headers);
    if (!status.ok()) co_return Result<Metadata>(std::move(status));
  }
  co_return Result<Metadata>(std::move(headers));
}
asio::awaitable<Result<Metadata>> Call::StartOwned(std::shared_ptr<Call> call,
                                                   Metadata headers) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (call->started_)
    co_return Result<Metadata>(
        Status{Code::kFailedPrecondition, "interceptors already started", {}});
  call->started_ = true;
  auto invoke = [call, headers = std::move(headers)]() mutable
      -> asio::awaitable<Result<Metadata>> {
    co_await asio::this_coro::throw_if_cancelled(false);
    auto parent = co_await asio::this_coro::cancellation_state;
    if (parent.cancelled() != asio::cancellation_type::none)
      co_return Result<Metadata>(Status{Code::kCanceled, "call canceled", {}});
    if (call->info_.deadline <= std::chrono::steady_clock::now())
      co_return Result<Metadata>(
          Status{Code::kDeadlineExceeded, "deadline exceeded", {}});
    struct State {
      asio::cancellation_signal signal;
      bool done = false;
      bool expired = false;
    };
    auto state = std::make_shared<State>();
    if (parent.slot().is_connected())
      parent.slot().assign(
          [state](asio::cancellation_type type) { state->signal.emit(type); });
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    if (call->info_.deadline != std::chrono::steady_clock::time_point::max()) {
      timer.expires_at(call->info_.deadline);
      timer.async_wait([state](asio::error_code error) {
        if (!error && !state->done) {
          state->expired = true;
          state->signal.emit(asio::cancellation_type::all);
        }
      });
    }
    auto [exception, result] = co_await asio::co_spawn(
        executor, RunRequest(call, std::move(headers)),
        asio::bind_cancellation_slot(state->signal.slot(),
                                     asio::as_tuple(asio::use_awaitable)));
    state->done = true;
    timer.cancel();
    if (parent.slot().is_connected()) parent.slot().clear();
    if (state->expired ||
        call->info_.deadline <= std::chrono::steady_clock::now())
      co_return Result<Metadata>(
          Status{Code::kDeadlineExceeded, "deadline exceeded", {}});
    if (parent.cancelled() != asio::cancellation_type::none)
      co_return Result<Metadata>(Status{Code::kCanceled, "call canceled", {}});
    if (exception)
      co_return Result<Metadata>(
          Status{Code::kInternal, "interceptor task failed", {}});
    co_return std::move(result);
  };
  // Preserve cancellation received before the child slot is installed.
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    co_return Result<Metadata>(Status{Code::kCanceled, "call canceled", {}});
  auto [exception, result] = co_await asio::co_spawn(
      asio::make_strand(co_await asio::this_coro::executor), invoke(),
      asio::as_tuple(asio::use_awaitable));
  if ((co_await asio::this_coro::cancellation_state).cancelled() !=
      asio::cancellation_type::none)
    co_return Result<Metadata>(Status{Code::kCanceled, "call canceled", {}});
  if (exception)
    co_return Result<Metadata>(
        Status{Code::kInternal, "interceptor dispatch failed", {}});
  co_return std::move(result);
}
void Call::ResponseHeaders(Metadata& headers) {
  if (headers_ || completed_) return;
  headers_ = true;
  for (size_t i = active_; i > 0; --i)
    hooks_[i - 1]->OnResponseHeaders(headers);
}
void Call::ResponseTrailers(Metadata& trailers) {
  if (trailers_ || completed_) return;
  trailers_ = true;
  for (size_t i = active_; i > 0; --i)
    hooks_[i - 1]->OnResponseTrailers(trailers);
}
void Call::Message(bool outbound, size_t bytes) {
  if (completed_) return;
  for (size_t i = 0; i < active_; ++i) hooks_[i]->OnMessage(outbound, bytes);
}
void Call::Complete(const Status& status) {
  if (std::exchange(completed_, true)) return;
  for (size_t i = active_; i > 0; --i) hooks_[i - 1]->OnComplete(status);
}
}  // namespace internal_interceptor
}  // namespace connectrpc
