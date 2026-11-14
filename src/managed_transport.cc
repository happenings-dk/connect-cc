#include "connectrpc/managed_transport.h"

#include <algorithm>
#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <charconv>
#include <utility>
#include <vector>

namespace connectrpc {
namespace {
using Time = std::chrono::steady_clock::time_point;
using Opened = Result<std::shared_ptr<ByteStream>>;
using Connected = Result<std::shared_ptr<HttpConnection>>;
Status Closed() { return {Code::kUnavailable, "managed transport closed", {}}; }
Status Canceled() { return {Code::kCanceled, "operation canceled", {}}; }
Status Expired() {
  return {Code::kDeadlineExceeded, "connection deadline exceeded", {}};
}
Status IoError(asio::error_code error) {
  return {error == asio::error::operation_aborted ? Code::kCanceled
                                                  : Code::kUnavailable,
          error.message(),
          {}};
}
struct Origin {
  std::string scheme, authority, host, port, prefix;
};
Result<Origin> ParseUrl(std::string_view url) {
  auto invalid = [] {
    return Result<Origin>(
        {Code::kInvalidArgument,
         "expected an http(s) base URL without credentials, query or fragment",
         {}});
  };
  Origin origin;
  if (url.starts_with("https://")) {
    origin.scheme = "https";
    origin.port = "443";
    url.remove_prefix(8);
  } else if (url.starts_with("http://")) {
    origin.scheme = "http";
    origin.port = "80";
    url.remove_prefix(7);
  } else
    return invalid();
  for (unsigned char c : url)
    if (c <= 32 || c >= 127 || c == '\\' || c == '?' || c == '#' || c == '@')
      return invalid();
  size_t slash = url.find('/');
  auto authority = url.substr(0, slash);
  if (authority.empty()) return invalid();
  origin.authority = authority;
  if (slash != std::string_view::npos) origin.prefix = url.substr(slash);
  while (!origin.prefix.empty() && origin.prefix.back() == '/')
    origin.prefix.pop_back();
  std::string_view port;
  if (authority.front() == '[') {
    auto end = authority.find(']');
    if (end == std::string_view::npos) return invalid();
    origin.host = authority.substr(1, end - 1);
    asio::error_code error;
    asio::ip::make_address_v6(origin.host, error);
    if (error || origin.host.find('%') != std::string::npos) return invalid();
    if (end + 1 != authority.size()) {
      if (authority[end + 1] != ':') return invalid();
      port = authority.substr(end + 2);
      if (port.empty()) return invalid();
    }
  } else {
    auto colon = authority.find(':');
    origin.host = authority.substr(0, colon);
    if (origin.host.empty()) return invalid();
    for (unsigned char c : origin.host)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.'))
        return invalid();
    if (colon != std::string_view::npos) {
      port = authority.substr(colon + 1);
      if (port.empty()) return invalid();
    }
  }
  if (!port.empty()) {
    unsigned number;
    auto [end, error] =
        std::from_chars(port.data(), port.data() + port.size(), number);
    if (error != std::errc{} || end != port.data() + port.size() ||
        number == 0 || number > 65535)
      return invalid();
    origin.port = port;
  }
  return Result<Origin>(std::move(origin));
}
// Each in-flight operation keeps its lease, not just its underlying stream.
// Destroying a handle cannot return a still-running HTTP/1 call to the pool.
class Lease final : public ByteStream,
                    public std::enable_shared_from_this<Lease> {
 public:
  Lease(std::shared_ptr<ByteStream> stream, std::function<void()> release)
      : stream_(std::move(stream)), release_(std::move(release)) {}
  ~Lease() override {
    stream_.reset();
    release_();
  }
  asio::any_io_executor executor() const override {
    return stream_->executor();
  }
  HttpVersion http_version() const override { return stream_->http_version(); }
  asio::awaitable<Result<Metadata>> ReadHeaders() override {
    return Keep(shared_from_this(), stream_->ReadHeaders());
  }
  asio::awaitable<Result<std::optional<std::string>>> Read() override {
    return Keep(shared_from_this(), stream_->Read());
  }
  asio::awaitable<Status> SendHeaders(Metadata headers) override {
    return Keep(shared_from_this(), stream_->SendHeaders(std::move(headers)));
  }
  asio::awaitable<Status> SendResponse(HttpMessage response,
                                       Metadata trailers) override {
    return Keep(shared_from_this(), stream_->SendResponse(std::move(response),
                                                          std::move(trailers)));
  }
  asio::awaitable<Status> Write(std::string bytes) override {
    return Keep(shared_from_this(), stream_->Write(std::move(bytes)));
  }
  asio::awaitable<Status> CloseSend(Metadata trailers) override {
    return Keep(shared_from_this(), stream_->CloseSend(std::move(trailers)));
  }
  asio::awaitable<Status> FinishSend() override {
    return Keep(shared_from_this(), stream_->FinishSend());
  }
  asio::awaitable<Status> DiscardRead() override {
    return Keep(shared_from_this(), stream_->DiscardRead());
  }
  asio::awaitable<Result<Metadata>> Finish() override {
    return Keep(shared_from_this(), stream_->Finish());
  }
  asio::awaitable<Result<Metadata>> ReadTrailers() override {
    return Keep(shared_from_this(), stream_->ReadTrailers());
  }
  asio::awaitable<Status> WaitClosed() override {
    return Keep(shared_from_this(), stream_->WaitClosed());
  }
  asio::awaitable<Status> SetDeadline(Time deadline) override {
    return Keep(shared_from_this(), stream_->SetDeadline(deadline));
  }
  asio::awaitable<Status> Cancel() override {
    return Keep(shared_from_this(), stream_->Cancel());
  }

 private:
  template <typename T>
  static asio::awaitable<T> Keep(std::shared_ptr<Lease> self,
                                 asio::awaitable<T> operation) {
    (void)self;
    co_return co_await std::move(operation);
  }
  std::shared_ptr<ByteStream> stream_;
  std::function<void()> release_;
};
}  // namespace
class ManagedTransport::Impl : public std::enable_shared_from_this<Impl> {
 public:
  Impl(asio::any_io_executor executor, Origin origin,
       ManagedTransportOptions options)
      : strand_(asio::make_strand(executor)),
        origin_(std::move(origin)),
        options_(std::move(options)),
        tls_(asio::ssl::context::tls_client),
        changed_(strand_, 1) {}
  Status Initialize() {
    return origin_.scheme == "https"
               ? ConfigureTlsContext(tls_, false, options_.tls)
               : Status{};
  }
  static asio::awaitable<Opened> OpenOn(std::shared_ptr<Impl> self,
                                        Metadata headers,
                                        TransportOptions options) {
    co_await asio::this_coro::throw_if_cancelled(false);
    if ((co_await asio::this_coro::cancellation_state).cancelled() !=
        asio::cancellation_type::none)
      co_return Opened(Canceled());
    auto [exception, result] = co_await asio::co_spawn(
        self->strand_, self->Open(std::move(headers), options),
        asio::as_tuple(asio::use_awaitable));
    if (exception)
      co_return Opened({Code::kInternal, "managed connection task failed", {}});
    co_return std::move(result);
  }
  static asio::awaitable<Status> CloseOn(std::shared_ptr<Impl> self,
                                         Time deadline) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    auto [exception, result] =
        co_await asio::co_spawn(self->strand_, self->Close(deadline),
                                asio::as_tuple(asio::use_awaitable));
    co_return exception ? Status{Code::kInternal, "managed shutdown failed", {}}
                        : result;
  }
  void RequestCancel() {
    asio::post(strand_, [self = shared_from_this()] {
      self->stopping_ = true;
      for (auto& attempt : self->attempts_) attempt->Cancel();
      for (auto& entry : self->entries_) {
        if (!entry->done) {
          ++self->cancels_;
          asio::co_spawn(self->strand_, entry->connection->Cancel(),
                         [self, entry](auto, Status) {
                           --self->cancels_;
                           self->Notify();
                         });
        }
      }
    });
  }

 private:
  struct Entry {
    Entry(asio::any_io_executor executor, std::shared_ptr<HttpConnection> value)
        : connection(std::move(value)), idle(executor) {}
    std::shared_ptr<HttpConnection> connection;
    asio::steady_timer idle;
    size_t active = 0;
    bool closing = false, done = false;
  };
  struct Attempt {
    explicit Attempt(asio::any_io_executor executor)
        : resolver(executor), socket(executor), timer(executor) {}
    void Cancel() {
      canceled = true;
      signal.emit(asio::cancellation_type::all);
      resolver.cancel();
      asio::error_code ignored;
      socket.close(ignored);
    }
    asio::ip::tcp::resolver resolver;
    asio::ip::tcp::socket socket;
    asio::steady_timer timer;
    asio::cancellation_signal signal;
    bool canceled = false, expired = false;
  };
  void Notify() { changed_.try_send(asio::error_code{}); }
  asio::awaitable<Connected> Dial(std::shared_ptr<Attempt> attempt,
                                  Time deadline) {
    co_await asio::this_coro::throw_if_cancelled(false);
    if (attempt->canceled) co_return Connected(Canceled());
    auto [error, endpoints] = co_await attempt->resolver.async_resolve(
        origin_.host, origin_.port, asio::as_tuple(asio::use_awaitable));
    if (error) co_return Connected(IoError(error));
    auto [connected, endpoint] = co_await asio::async_connect(
        attempt->socket, endpoints, asio::as_tuple(asio::use_awaitable));
    (void)endpoint;
    if (connected) co_return Connected(IoError(connected));
    if (attempt->canceled) co_return Connected(Canceled());
    if (origin_.scheme == "https")
      co_return co_await HttpConnection::CreateTls(
          asio::ssl::stream<asio::ip::tcp::socket>(std::move(attempt->socket),
                                                   tls_),
          HttpConnection::Role::kClient, origin_.host, options_.connection,
          deadline);
    co_return HttpConnection::Create(
        std::move(attempt->socket), HttpConnection::Role::kClient,
        options_.cleartext_version, options_.connection);
  }
  asio::awaitable<Opened> Open(Metadata headers, TransportOptions options) {
    struct OpenGuard {
      explicit OpenGuard(Impl& owner) : owner(owner) { ++owner.opens_; }
      ~OpenGuard() {
        --owner.opens_;
        owner.Notify();
      }
      Impl& owner;
    } guard(*this);
    co_await asio::this_coro::throw_if_cancelled(false);
    co_await asio::post(strand_, asio::use_awaitable);
    if (stopping_) co_return Opened(Closed());
    if ((co_await asio::this_coro::cancellation_state).cancelled() !=
        asio::cancellation_type::none)
      co_return Opened(Canceled());
    if (options.deadline <= std::chrono::steady_clock::now())
      co_return Opened(Expired());
    bool path = false;
    for (auto& [name, value] : headers) {
      if (name == ":scheme" || name == ":authority")
        co_return Opened({Code::kInvalidArgument,
                          "managed transport owns request origin",
                          {}});
      if (name == ":path") {
        if (path || value.empty() || value.front() != '/')
          co_return Opened(
              {Code::kInvalidArgument, "invalid request path", {}});
        path = true;
        value = origin_.prefix + value;
      }
    }
    if (!path)
      co_return Opened({Code::kInvalidArgument, "missing request path", {}});
    headers.emplace(headers.begin(), ":scheme", origin_.scheme);
    headers.emplace(headers.begin(), ":authority", origin_.authority);
    std::erase_if(entries_, [](const auto& entry) {
      return entry->done && entry->active == 0;
    });
    std::shared_ptr<Entry> entry;
    for (auto& candidate : entries_) {
      size_t limit =
          candidate->connection->http_version() == HttpVersion::kHttp1
              ? 1
              : options_.connection.http2.max_streams;
      if (!candidate->done && !candidate->closing &&
          candidate->active < limit) {
        entry = candidate;
        break;
      }
    }
    if (!entry) {
      if (entries_.size() + attempts_.size() >= options_.max_connections)
        co_return Opened(
            {Code::kResourceExhausted, "connection pool is at capacity", {}});
      auto attempt = std::make_shared<Attempt>(strand_);
      attempts_.push_back(attempt);
      Time deadline =
          std::min(options.deadline,
                   std::chrono::steady_clock::now() + options_.connect_timeout);
      attempt->timer.expires_at(deadline);
      attempt->timer.async_wait(
          [weak = std::weak_ptr(attempt)](asio::error_code error) {
            if (!error)
              if (auto state = weak.lock()) {
                state->expired = true;
                state->Cancel();
              }
          });
      auto cancellation = co_await asio::this_coro::cancellation_state;
      if (cancellation.slot().is_connected())
        cancellation.slot().assign(
            [weak = std::weak_ptr(attempt)](asio::cancellation_type) {
              if (auto state = weak.lock()) state->Cancel();
            });
      auto [exception, connected] = co_await asio::co_spawn(
          strand_, Dial(attempt, deadline),
          asio::bind_cancellation_slot(attempt->signal.slot(),
                                       asio::as_tuple(asio::use_awaitable)));
      cancellation.slot().clear();
      attempt->timer.cancel();
      std::erase(attempts_, attempt);
      Notify();
      if (exception)
        co_return Opened({Code::kInternal, "dial task failed", {}});
      if (!connected.ok())
        co_return Opened(attempt->expired    ? Expired()
                         : attempt->canceled ? Canceled()
                                             : connected.status());
      if (stopping_ || attempt->canceled) {
        co_await (*connected.value())->Cancel();
        co_return Opened(stopping_          ? Closed()
                         : attempt->expired ? Expired()
                                            : Canceled());
      }
      entry = std::make_shared<Entry>(strand_, *connected.value());
      entries_.push_back(entry);
      asio::co_spawn(strand_, entry->connection->Run(),
                     [self = shared_from_this(), entry](auto, Status) {
                       entry->done = true;
                       entry->idle.cancel();
                       self->Notify();
                     });
    }
    ++entry->active;
    entry->idle.cancel();
    auto opened =
        co_await entry->connection->Open(std::move(headers), options.deadline);
    if (!opened.ok()) {
      // A draining/stale connection is retired. This call is never retried.
      if (opened.status().code == Code::kUnavailable)
        Retire(entry,
               std::chrono::steady_clock::now() + std::chrono::seconds(1));
      Release(entry);
      co_return opened;
    }
    auto release = [self = shared_from_this(), entry] {
      asio::post(self->strand_, [self, entry] { self->Release(entry); });
    };
    co_return Opened(
        std::make_shared<Lease>(*opened.value(), std::move(release)));
  }
  void Release(const std::shared_ptr<Entry>& entry) {
    --entry->active;
    if (entry->active == 0 && !entry->done && !entry->closing && !stopping_) {
      entry->idle.expires_after(options_.idle_timeout);
      entry->idle.async_wait(
          [self = shared_from_this(), entry](asio::error_code error) {
            if (!error && entry->active == 0)
              self->Retire(entry, std::chrono::steady_clock::now() +
                                      std::chrono::seconds(1));
          });
    }
    Notify();
  }
  void Retire(const std::shared_ptr<Entry>& entry, Time deadline) {
    if (entry->closing || entry->done) return;
    entry->closing = true;
    entry->idle.cancel();
    ++shutdowns_;
    asio::co_spawn(
        strand_, entry->connection->Shutdown(deadline),
        [self = shared_from_this(), entry](auto exception, Status status) {
          if ((exception || !status.ok()) && self->close_status_.ok())
            self->close_status_ =
                exception
                    ? Status{Code::kInternal, "connection shutdown failed", {}}
                    : status;
          --self->shutdowns_;
          self->Notify();
        });
  }
  asio::awaitable<Status> Close(Time deadline) {
    co_await asio::this_coro::reset_cancellation_state(
        asio::disable_cancellation());
    if (closing_)
      co_return Status{
          Code::kFailedPrecondition, "close already in progress", {}};
    closing_ = true;
    stopping_ = true;
    for (auto& attempt : attempts_) attempt->Cancel();
    for (auto& entry : entries_) Retire(entry, deadline);
    // Also bound connections already retiring because of idle expiry.
    asio::steady_timer timer(strand_);
    timer.expires_at(deadline);
    timer.async_wait([self = shared_from_this()](asio::error_code error) {
      if (!error) {
        self->close_status_ = Expired();
        self->RequestCancel();
      }
    });
    while (opens_ != 0 || cancels_ != 0 || !attempts_.empty() ||
           shutdowns_ != 0 ||
           std::any_of(entries_.begin(), entries_.end(),
                       [](const auto& entry) { return !entry->done; }))
      co_await changed_.async_receive(asio::as_tuple(asio::use_awaitable));
    timer.cancel();
    co_return close_status_;
  }
  asio::strand<asio::any_io_executor> strand_;
  Origin origin_;
  ManagedTransportOptions options_;
  asio::ssl::context tls_;
  asio::experimental::channel<void(asio::error_code)> changed_;
  std::vector<std::shared_ptr<Entry>> entries_;
  std::vector<std::shared_ptr<Attempt>> attempts_;
  size_t shutdowns_ = 0;
  size_t cancels_ = 0;
  size_t opens_ = 0;
  bool stopping_ = false, closing_ = false;
  Status close_status_;
};
Result<std::shared_ptr<ManagedTransport>> ManagedTransport::Create(
    asio::any_io_executor executor, std::string base_url,
    ManagedTransportOptions options) {
  auto origin = ParseUrl(base_url);
  if (!origin.ok())
    return Result<std::shared_ptr<ManagedTransport>>(origin.status());
  if (options.max_connections == 0 || options.max_connections > 1024 ||
      options.connect_timeout.count() <= 0 ||
      options.idle_timeout.count() <= 0 ||
      (options.cleartext_version != HttpVersion::kHttp1 &&
       options.cleartext_version != HttpVersion::kHttp2))
    return Result<std::shared_ptr<ManagedTransport>>(
        {Code::kInvalidArgument, "invalid managed transport limits", {}});
  auto impl = std::make_shared<Impl>(executor, std::move(*origin.value()),
                                     std::move(options));
  Status status = impl->Initialize();
  if (!status.ok()) return Result<std::shared_ptr<ManagedTransport>>(status);
  return Result<std::shared_ptr<ManagedTransport>>(
      std::shared_ptr<ManagedTransport>(new ManagedTransport(std::move(impl))));
}
ManagedTransport::ManagedTransport(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
ManagedTransport::~ManagedTransport() { impl_->RequestCancel(); }
Client ManagedTransport::MakeClient() const {
  return Client(
      [impl = impl_](HttpMessage request, TransportOptions options)
          -> asio::awaitable<Result<HttpMessage>> {
        auto stream =
            co_await Impl::OpenOn(impl, std::move(request.headers), options);
        if (!stream.ok()) co_return Result<HttpMessage>(stream.status());
        co_return co_await UnaryByteStream(*stream.value(),
                                           std::move(request.body), options);
      },
      [impl = impl_](Metadata headers, TransportOptions options) {
        return Impl::OpenOn(impl, std::move(headers), options);
      });
}
asio::awaitable<Opened> ManagedTransport::Open(Metadata headers,
                                               TransportOptions options) {
  return Impl::OpenOn(impl_, std::move(headers), options);
}
asio::awaitable<Status> ManagedTransport::Close(Time deadline) {
  return Impl::CloseOn(impl_, deadline);
}
}  // namespace connectrpc
