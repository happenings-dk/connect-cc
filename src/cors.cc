#include "connectrpc/cors.h"

#include <algorithm>
#include <asio/this_coro.hpp>
#include <memory>
#include <string_view>
#include <utility>

namespace connectrpc {
namespace {
std::string Lower(std::string_view input) {
  std::string output(input);
  for (char& c : output)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return output;
}
std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    value.remove_suffix(1);
  return value;
}
bool Token(std::string_view value) {
  if (value.empty()) return false;
  for (char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)
      continue;
    return false;
  }
  return true;
}
bool Contains(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}
void Add(std::vector<std::string>& values, std::string value) {
  if (!Contains(values, value)) values.push_back(std::move(value));
}
std::string Join(const std::vector<std::string>& values) {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) result.append(", ");
    result.append(value);
  }
  return result;
}
// Missing, empty, and duplicate singleton fields all fail closed.
std::string_view Single(const Metadata& headers, std::string_view name) {
  std::string_view result;
  bool found = false;
  for (const auto& [key, value] : headers) {
    if (key != name) continue;
    if (found) return {};
    result = value;
    found = true;
  }
  return result;
}
bool Has(const Metadata& headers, std::string_view name) {
  return std::any_of(headers.begin(), headers.end(),
                     [name](const auto& item) { return item.first == name; });
}
void Vary(Metadata& headers, std::string_view name) {
  for (const auto& [key, value] : headers) {
    if (key != "vary") continue;
    std::string_view rest = value;
    for (;;) {
      size_t comma = rest.find(',');
      std::string token = Lower(Trim(rest.substr(0, comma)));
      if (token == "*" || token == name) return;
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1);
    }
  }
  headers.emplace_back("vary", name);
}
void RemoveControlHeaders(Metadata& headers) {
  std::erase_if(headers, [](const auto& item) {
    return Lower(item.first).starts_with("access-control-");
  });
}
bool Origin(std::string_view value) {
  if (value == "null") return true;
  if (value.starts_with("https://"))
    value.remove_prefix(8);
  else if (value.starts_with("http://"))
    value.remove_prefix(7);
  else
    return false;
  if (value.empty()) return false;
  for (unsigned char c : value)
    if (c <= 32 || c >= 127 ||
        std::string_view("/\\?#@,*").find(c) != std::string_view::npos)
      return false;
  return true;
}
bool AllowedRequestHeaders(const Metadata& headers,
                           const std::vector<std::string>& allowed) {
  for (const auto& [name, value] : headers) {
    if (name != "access-control-request-headers") continue;
    std::string_view rest = Trim(value);
    if (rest.empty()) continue;
    for (;;) {
      size_t comma = rest.find(',');
      std::string token = Lower(Trim(rest.substr(0, comma)));
      if (!Token(token) || !Contains(allowed, token)) return false;
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1);
    }
  }
  return true;
}

// Shares the underlying transport because its read, write, and close observer
// can be pending concurrently. Only response-header emission is intercepted.
class CorsByteStream final : public ByteStream {
 public:
  CorsByteStream(std::shared_ptr<ByteStream> stream,
                 std::shared_ptr<const CorsPolicy> policy, Metadata request)
      : stream_(std::move(stream)),
        policy_(std::move(policy)),
        request_(std::move(request)) {}
  asio::any_io_executor executor() const override {
    return stream_->executor();
  }
  HttpVersion http_version() const override { return stream_->http_version(); }
  asio::awaitable<Result<Metadata>> ReadHeaders() override {
    return stream_->ReadHeaders();
  }
  asio::awaitable<Result<std::optional<std::string>>> Read() override {
    return stream_->Read();
  }
  asio::awaitable<Status> SendHeaders(Metadata headers) override {
    policy_->ApplyResponseHeaders(request_, headers);
    return stream_->SendHeaders(std::move(headers));
  }
  asio::awaitable<Status> SendResponse(HttpMessage response,
                                       Metadata trailers) override {
    policy_->ApplyResponseHeaders(request_, response.headers);
    return stream_->SendResponse(std::move(response), std::move(trailers));
  }
  asio::awaitable<Status> Write(std::string bytes) override {
    return stream_->Write(std::move(bytes));
  }
  asio::awaitable<Status> CloseSend(Metadata trailers) override {
    return stream_->CloseSend(std::move(trailers));
  }
  asio::awaitable<Status> FinishSend() override {
    return stream_->FinishSend();
  }
  asio::awaitable<Status> DiscardRead() override {
    return stream_->DiscardRead();
  }
  asio::awaitable<Result<Metadata>> Finish() override {
    return stream_->Finish();
  }
  asio::awaitable<Result<Metadata>> ReadTrailers() override {
    return stream_->ReadTrailers();
  }
  asio::awaitable<Status> WaitClosed() override {
    return stream_->WaitClosed();
  }
  asio::awaitable<Status> SetDeadline(
      std::chrono::steady_clock::time_point deadline) override {
    return stream_->SetDeadline(deadline);
  }
  asio::awaitable<Status> Cancel() override { return stream_->Cancel(); }

 private:
  std::shared_ptr<ByteStream> stream_;
  std::shared_ptr<const CorsPolicy> policy_;
  Metadata request_;
};
asio::awaitable<Status> ServeCors(std::shared_ptr<const CorsPolicy> policy,
                                  HttpStreamHandler handler,
                                  std::chrono::milliseconds timeout,
                                  std::shared_ptr<ByteStream> stream) {
  co_await asio::this_coro::throw_if_cancelled(false);
  if (!stream || !handler)
    co_return Status{
        Code::kInvalidArgument, "missing CORS stream or handler", {}};
  auto headers = co_await stream->ReadHeaders();
  if (!headers.ok()) co_return headers.status();
  auto preflight = policy->Preflight(*headers.value());
  if (!preflight) {
    co_return co_await handler(std::make_shared<CorsByteStream>(
        std::move(stream), std::move(policy), std::move(*headers.value())));
  }
  Status status =
      co_await stream->SetDeadline(std::chrono::steady_clock::now() + timeout);
  if (status.ok())
    status = co_await stream->SendHeaders(std::move(preflight->headers));
  if (status.ok()) status = co_await stream->CloseSend({});
  if (status.ok()) status = co_await stream->FinishSend();
  if (status.ok()) status = co_await stream->DiscardRead();
  if (!status.ok()) co_await stream->Cancel();
  co_return status;
}
}  // namespace

CorsPolicy::CorsPolicy(CorsOptions options) : options_(std::move(options)) {}
Result<CorsPolicy> CorsPolicy::Create(CorsOptions options) {
  if (options.max_age < std::chrono::seconds::zero() ||
      options.completion_timeout <= std::chrono::milliseconds::zero())
    return Result<CorsPolicy>(
        Status{Code::kInvalidArgument, "invalid CORS duration", {}});
  for (const auto& origin : options.allowed_origins)
    if (!Origin(origin))
      return Result<CorsPolicy>(
          Status{Code::kInvalidArgument, "invalid CORS origin", {}});
  for (auto* names : {&options.allowed_headers, &options.exposed_headers}) {
    for (auto& name : *names) {
      name = Lower(name);
      if (!Token(name) || name == "*" || name == "set-cookie" ||
          name == "set-cookie2" || name.starts_with("access-control-"))
        return Result<CorsPolicy>(
            Status{Code::kInvalidArgument, "invalid CORS header name", {}});
    }
    std::sort(names->begin(), names->end());
    names->erase(std::unique(names->begin(), names->end()), names->end());
  }
  for (std::string_view name :
       {"content-type", "connect-protocol-version", "connect-timeout-ms",
        "connect-content-encoding", "connect-accept-encoding",
        "content-encoding", "grpc-timeout", "grpc-encoding",
        "grpc-accept-encoding", "x-grpc-web", "x-user-agent"})
    Add(options.allowed_headers, std::string(name));
  for (std::string_view name :
       {"grpc-status", "grpc-message", "grpc-status-details-bin",
        "connect-content-encoding", "connect-accept-encoding", "grpc-encoding",
        "grpc-accept-encoding"})
    Add(options.exposed_headers, std::string(name));
  return Result<CorsPolicy>(CorsPolicy(std::move(options)));
}
std::optional<HttpMessage> CorsPolicy::Preflight(
    const Metadata& headers) const {
  if (Single(headers, ":method") != "OPTIONS" || !Has(headers, "origin") ||
      !Has(headers, "access-control-request-method"))
    return std::nullopt;
  HttpMessage response;
  response.headers = {{":status", "403"},
                      {"vary",
                       "origin, access-control-request-method, "
                       "access-control-request-headers"}};
  std::string_view origin = Single(headers, "origin");
  std::string_view method = Single(headers, "access-control-request-method");
  if (origin.empty() || !Contains(options_.allowed_origins, origin) ||
      (method != "GET" && method != "POST") ||
      !AllowedRequestHeaders(headers, options_.allowed_headers))
    return response;
  response.headers.front().second = "204";
  response.headers.emplace_back("access-control-allow-origin", origin);
  response.headers.emplace_back("access-control-allow-methods", "GET, POST");
  response.headers.emplace_back("access-control-allow-headers",
                                Join(options_.allowed_headers));
  response.headers.emplace_back("access-control-max-age",
                                std::to_string(options_.max_age.count()));
  if (options_.allow_credentials)
    response.headers.emplace_back("access-control-allow-credentials", "true");
  return response;
}
void CorsPolicy::ApplyResponseHeaders(const Metadata& request,
                                      Metadata& response) const {
  RemoveControlHeaders(response);
  // Even a response to a request without Origin must not be reused for one
  // with Origin (or vice versa) by an intermediate cache.
  Vary(response, "origin");
  std::string_view origin = Single(request, "origin");
  if (origin.empty() || !Contains(options_.allowed_origins, origin)) return;
  response.emplace_back("access-control-allow-origin", origin);
  response.emplace_back("access-control-expose-headers",
                        Join(options_.exposed_headers));
  if (options_.allow_credentials)
    response.emplace_back("access-control-allow-credentials", "true");
}
HttpStreamHandler CorsPolicy::Wrap(HttpStreamHandler handler) const {
  // The operation takes copies before suspension, so neither the wrapper nor
  // the original policy needs to outlive a call that has already been started.
  return [policy = std::make_shared<const CorsPolicy>(*this),
          handler = std::move(handler), timeout = options_.completion_timeout](
             std::shared_ptr<ByteStream> s) {
    return ServeCors(policy, handler, timeout, std::move(s));
  };
}
}  // namespace connectrpc
