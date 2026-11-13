#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_INTERCEPTOR_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_INTERCEPTOR_H_

#include <asio/awaitable.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "connectrpc/status.h"
#include "connectrpc/streaming_protocol.h"

namespace connectrpc {
struct CallInfo {
  std::string procedure;
  StreamShape shape = StreamShape::kUnary;
  Protocol protocol = Protocol::kConnect;
  Codec codec = Codec::kProtobuf;
  bool client = true;
  bool use_get = false;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
};
// One instance per call. Request hooks may await cooperative asynchronous work;
// other hooks are synchronous and must not block or throw. Metadata contains
// application fields, not protocol-owned HTTP fields. Hooks for a single call
// do not overlap, but may execute on different threads. OnComplete is called
// once for entered interceptors, including canceled/abandoned calls. A dropped
// call can complete from the thread releasing its final state.
class CallInterceptor {
 public:
  CallInterceptor() = default;
  CallInterceptor(const CallInterceptor&) = delete;
  CallInterceptor& operator=(const CallInterceptor&) = delete;
  virtual ~CallInterceptor() = default;
  virtual asio::awaitable<Status> OnRequest(Metadata& headers);
  virtual void OnResponseHeaders(Metadata& headers);
  virtual void OnResponseTrailers(Metadata& trailers);
  // Counts serialized, uncompressed message bytes; does not expose payloads.
  virtual void OnMessage(bool outbound, size_t bytes);
  virtual void OnComplete(const Status& status);
};
// Factories may run concurrently for distinct calls. Return nullptr to opt out
// of a particular procedure. The CallInfo reference remains valid throughout
// the resulting interceptor's lifetime. Factories must not block or throw.
using Interceptor =
    std::function<std::unique_ptr<CallInterceptor>(const CallInfo&)>;

// Internal execution state shared with RPC templates and the message engine.
namespace internal_interceptor {
class Call {
 public:
  static Result<std::shared_ptr<Call>> Create(
      const std::vector<Interceptor>& interceptors, CallInfo info);
  ~Call();
  Call(const Call&) = delete;
  Call& operator=(const Call&) = delete;
  asio::awaitable<Result<Metadata>> Start(Metadata headers);
  void ResponseHeaders(Metadata& headers);
  void ResponseTrailers(Metadata& trailers);
  void Message(bool outbound, size_t bytes);
  void Complete(const Status& status);

 private:
  explicit Call(CallInfo info);
  static asio::awaitable<Result<Metadata>> StartOwned(
      std::shared_ptr<Call> call, Metadata headers);
  static asio::awaitable<Result<Metadata>> RunRequest(
      std::shared_ptr<Call> call, Metadata headers);
  // Declared before hooks so factory-borrowed references outlive hooks.
  CallInfo info_;
  std::vector<std::unique_ptr<CallInterceptor>> hooks_;
  size_t active_ = 0;
  bool started_ = false;
  bool headers_ = false;
  bool trailers_ = false;
  bool completed_ = false;
  // Start retains the same shared state without requiring inheritance.
  std::weak_ptr<Call> self_;
};
}  // namespace internal_interceptor
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_INTERCEPTOR_H_
