#include "connectrpc/http2_stream.h"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace connectrpc {
namespace {
Status LibraryError(int code) {
  return {Code::kInternal, nghttp2_strerror(code), {}};
}
Status Closed() { return {Code::kFailedPrecondition, "stream is closed", {}}; }
std::vector<nghttp2_nv> WireHeaders(Metadata& metadata) {
  std::vector<nghttp2_nv> result;
  result.reserve(metadata.size());
  for (auto& [name, value] : metadata)
    result.push_back({reinterpret_cast<uint8_t*>(name.data()),
                      reinterpret_cast<uint8_t*>(value.data()), name.size(),
                      value.size(), NGHTTP2_NV_FLAG_NONE});
  return result;
}
Result<size_t> HeaderBytes(const Metadata& headers, size_t available,
                           bool trailers) {
  size_t total = 0;
  for (const auto& [name, value] : headers) {
    if (available - total < 32 || name.size() > available - total - 32 ||
        value.size() > available - total - 32 - name.size())
      return Result<size_t>(
          {Code::kResourceExhausted, "metadata exceeds limit", {}});
    if ((trailers && name.starts_with(':')) ||
        !nghttp2_check_header_name(
            reinterpret_cast<const uint8_t*>(name.data()), name.size()) ||
        !nghttp2_check_header_value(
            reinterpret_cast<const uint8_t*>(value.data()), value.size()))
      return Result<size_t>(
          {Code::kInvalidArgument, "invalid HTTP/2 metadata", {}});
    total += 32 + name.size() + value.size();
  }
  return Result<size_t>(total);
}
}  // namespace

class Http2StreamSession::Impl {
 public:
  Impl(Http2Session::Role role, Http2StreamOptions options,
       Http2StreamEvents events)
      : role_(role), options_(options), events_(std::move(events)) {}
  ~Impl() {
    if (session_) nghttp2_session_del(session_);
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Status Initialize();
  Result<int32_t> StartRequest(Metadata headers);
  Status StartResponse(int32_t id, Metadata headers);
  Result<size_t> TryWrite(int32_t id, std::string_view bytes);
  Status CloseSend(int32_t id, Metadata trailers);
  Status Consume(int32_t id, size_t bytes);
  Status Release(int32_t id);
  Status Reset(int32_t id, uint32_t error_code);
  Status Shutdown();
  Status Receive(std::string_view bytes);
  Result<std::string> Drain();
  void Fail(Status status);

 private:
  struct Stream {
    Metadata headers;
    Metadata trailers;
    std::deque<std::string> outgoing;
    size_t front_offset = 0;
    size_t queued = 0;
    size_t unconsumed = 0;
    size_t received_metadata = 0;
    size_t sent_metadata = 0;
    bool got_headers = false;
    bool got_end = false;
    bool send_started = false;
    bool send_closed = false;
    bool deferred = false;
    bool closed = false;
    bool wire_closed = false;
  };
  static int BeginHeaders(nghttp2_session*, const nghttp2_frame* frame,
                          void* data);
  static int Header(nghttp2_session*, const nghttp2_frame* frame,
                    const uint8_t* name, size_t name_length,
                    const uint8_t* value, size_t value_length, uint8_t,
                    void* data);
  static int Data(nghttp2_session*, uint8_t, int32_t id, const uint8_t* bytes,
                  size_t count, void* data);
  static int Frame(nghttp2_session*, const nghttp2_frame* frame, void* data);
  static int Sent(nghttp2_session*, const nghttp2_frame* frame, void* data);
  static int Close(nghttp2_session*, int32_t id, uint32_t code, void* data);
  static nghttp2_ssize Read(nghttp2_session*, int32_t id, uint8_t* buffer,
                            size_t length, uint32_t* flags,
                            nghttp2_data_source*, void* data);
  void NotifyClosed(int32_t id, Status status);
  Status Resume(int32_t id, Stream& stream);

  nghttp2_session* session_ = nullptr;
  Http2Session::Role role_;
  Http2StreamOptions options_;
  Http2StreamEvents events_;
  std::map<int32_t, Stream> streams_;
  bool draining_ = false;
  bool pending_goaway_ = false;
  Status failure_;
};

Status Http2StreamSession::Impl::Initialize() {
  nghttp2_session_callbacks* callbacks = nullptr;
  int code = nghttp2_session_callbacks_new(&callbacks);
  if (code != 0) return LibraryError(code);
  nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks,
                                                          BeginHeaders);
  nghttp2_session_callbacks_set_on_header_callback(callbacks, Header);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, Data);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, Frame);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, Close);
  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, Sent);
  nghttp2_option* options = nullptr;
  code = nghttp2_option_new(&options);
  if (code != 0) {
    nghttp2_session_callbacks_del(callbacks);
    return LibraryError(code);
  }
  nghttp2_option_set_no_auto_window_update(options, 1);
  code = role_ == Http2Session::Role::kClient
             ? nghttp2_session_client_new2(&session_, callbacks, this, options)
             : nghttp2_session_server_new2(&session_, callbacks, this, options);
  nghttp2_option_del(options);
  nghttp2_session_callbacks_del(callbacks);
  if (code != 0) return LibraryError(code);
  nghttp2_settings_entry settings[] = {
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, options_.max_streams},
      {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
       static_cast<uint32_t>(options_.stream_window_bytes)},
      {NGHTTP2_SETTINGS_ENABLE_PUSH, 0}};
  code = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings,
                                 role_ == Http2Session::Role::kClient ? 3 : 2);
  if (code != 0) return LibraryError(code);
  code = nghttp2_session_set_local_window_size(
      session_, NGHTTP2_FLAG_NONE, 0, options_.connection_window_bytes);
  return code == 0 ? Status{} : LibraryError(code);
}
int Http2StreamSession::Impl::BeginHeaders(nghttp2_session*,
                                           const nghttp2_frame* frame,
                                           void* data) {
  auto& self = *static_cast<Impl*>(data);
  if (frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
    if (self.draining_ || self.streams_.size() >= self.options_.max_streams) {
      nghttp2_submit_rst_stream(self.session_, NGHTTP2_FLAG_NONE,
                                frame->hd.stream_id, NGHTTP2_REFUSED_STREAM);
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    self.streams_.try_emplace(frame->hd.stream_id);
  }
  auto found = self.streams_.find(frame->hd.stream_id);
  return found == self.streams_.end() || found->second.closed
             ? NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
             : 0;
}
int Http2StreamSession::Impl::Header(nghttp2_session*,
                                     const nghttp2_frame* frame,
                                     const uint8_t* name, size_t name_length,
                                     const uint8_t* value, size_t value_length,
                                     uint8_t, void* data) {
  auto& self = *static_cast<Impl*>(data);
  auto found = self.streams_.find(frame->hd.stream_id);
  if (found == self.streams_.end() || found->second.closed)
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  Stream& stream = found->second;
  size_t available =
      self.options_.max_metadata_bytes - stream.received_metadata;
  if (available < 32 || name_length > available - 32 ||
      value_length > available - 32 - name_length) {
    self.NotifyClosed(frame->hd.stream_id,
                      {Code::kResourceExhausted, "metadata exceeds limit", {}});
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  stream.received_metadata += 32 + name_length + value_length;
  stream.headers.emplace_back(
      std::string(reinterpret_cast<const char*>(name), name_length),
      std::string(reinterpret_cast<const char*>(value), value_length));
  return 0;
}
int Http2StreamSession::Impl::Data(nghttp2_session*, uint8_t, int32_t id,
                                   const uint8_t* bytes, size_t count,
                                   void* data) {
  auto& self = *static_cast<Impl*>(data);
  auto found = self.streams_.find(id);
  if (found == self.streams_.end() || found->second.closed)
    return nghttp2_session_consume_connection(self.session_, count);
  Stream& stream = found->second;
  if (count > static_cast<size_t>(self.options_.stream_window_bytes) -
                  stream.unconsumed) {
    self.Reset(id, NGHTTP2_FLOW_CONTROL_ERROR);
    return 0;
  }
  stream.unconsumed += count;
  self.events_.data(
      id, std::string_view(reinterpret_cast<const char*>(bytes), count));
  return 0;
}
int Http2StreamSession::Impl::Frame(nghttp2_session*,
                                    const nghttp2_frame* frame, void* data) {
  auto& self = *static_cast<Impl*>(data);
  if (frame->hd.type == NGHTTP2_GOAWAY) {
    self.draining_ = true;
    std::vector<int32_t> rejected;
    for (const auto& [id, stream] : self.streams_)
      if (frame->goaway.error_code != 0 ||
          (self.role_ == Http2Session::Role::kClient &&
           id > frame->goaway.last_stream_id))
        rejected.push_back(id);
    for (int32_t id : rejected)
      self.NotifyClosed(id, {Code::kUnavailable, "peer sent GOAWAY", {}});
  }
  if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA)
    return 0;
  auto found = self.streams_.find(frame->hd.stream_id);
  if (found == self.streams_.end() || found->second.closed) return 0;
  if (frame->hd.type == NGHTTP2_HEADERS) {
    Stream& stream = found->second;
    bool informational = false;
    for (const auto& [name, value] : stream.headers)
      if (name == ":status" && value.size() == 3 && value.front() == '1')
        informational = true;
    bool trailers = stream.got_headers;
    if (!informational) stream.got_headers = true;
    Metadata headers = std::move(stream.headers);
    stream.headers.clear();
    self.events_.headers(frame->hd.stream_id, std::move(headers), trailers,
                         informational);
  }
  // Callbacks may reset or release their stream; re-find before accessing it.
  found = self.streams_.find(frame->hd.stream_id);
  if (found != self.streams_.end() && !found->second.closed &&
      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
    found->second.got_end = true;
    self.events_.end(frame->hd.stream_id);
  }
  return 0;
}
int Http2StreamSession::Impl::Sent(nghttp2_session*, const nghttp2_frame* frame,
                                   void* data) {
  auto& self = *static_cast<Impl*>(data);
  if ((frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) &&
      (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) && self.events_.send_end)
    self.events_.send_end(frame->hd.stream_id);
  return 0;
}
void Http2StreamSession::Impl::NotifyClosed(int32_t id, Status status) {
  auto found = streams_.find(id);
  if (found == streams_.end() || found->second.closed) return;
  found->second.closed = true;
  found->second.outgoing.clear();
  found->second.queued = 0;
  events_.close(id, std::move(status));
}
int Http2StreamSession::Impl::Close(nghttp2_session*, int32_t id, uint32_t code,
                                    void* data) {
  auto& self = *static_cast<Impl*>(data);
  auto found = self.streams_.find(id);
  if (found == self.streams_.end()) return 0;
  found->second.wire_closed = true;
  Status status;
  if (code != NGHTTP2_NO_ERROR || !found->second.got_end) {
    Code rpc_code =
        code == NGHTTP2_CANCEL ? Code::kCanceled : Code::kUnavailable;
    if (code == NGHTTP2_INTERNAL_ERROR || code == NGHTTP2_PROTOCOL_ERROR)
      rpc_code = Code::kInternal;
    status = {rpc_code, "HTTP/2 stream reset or truncated", {}};
  }
  self.NotifyClosed(id, std::move(status));
  return 0;
}
nghttp2_ssize Http2StreamSession::Impl::Read(nghttp2_session*, int32_t id,
                                             uint8_t* buffer, size_t length,
                                             uint32_t* flags,
                                             nghttp2_data_source*, void* data) {
  auto& self = *static_cast<Impl*>(data);
  auto found = self.streams_.find(id);
  if (found == self.streams_.end() || found->second.closed)
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  Stream& stream = found->second;
  if (stream.outgoing.empty() && !stream.send_closed) {
    stream.deferred = true;
    return NGHTTP2_ERR_DEFERRED;
  }
  size_t count = 0;
  while (!stream.outgoing.empty() && count < length) {
    const std::string& front = stream.outgoing.front();
    size_t copied =
        std::min(length - count, front.size() - stream.front_offset);
    std::memcpy(buffer + count, front.data() + stream.front_offset, copied);
    stream.front_offset += copied;
    stream.queued -= copied;
    count += copied;
    if (stream.front_offset == front.size()) {
      stream.outgoing.pop_front();
      stream.front_offset = 0;
    }
  }
  if (stream.outgoing.empty() && stream.send_closed) {
    *flags |= NGHTTP2_DATA_FLAG_EOF;
    if (!stream.trailers.empty()) {
      *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
      auto trailers = WireHeaders(stream.trailers);
      int code = nghttp2_submit_trailer(self.session_, id, trailers.data(),
                                        trailers.size());
      if (code != 0) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
      stream.trailers.clear();
    }
  }
  if (count && self.events_.writable) self.events_.writable(id);
  return static_cast<nghttp2_ssize>(count);
}
Status Http2StreamSession::Impl::Resume(int32_t id, Stream& stream) {
  if (!stream.deferred) return {};
  int code = nghttp2_session_resume_data(session_, id);
  if (code != 0) return LibraryError(code);
  stream.deferred = false;
  return {};
}
Result<int32_t> Http2StreamSession::Impl::StartRequest(Metadata metadata) {
  if (!failure_.ok()) return Result<int32_t>(failure_);
  if (draining_)
    return Result<int32_t>({Code::kUnavailable, "session draining", {}});
  if (role_ != Http2Session::Role::kClient)
    return Result<int32_t>({Code::kFailedPrecondition, "not a client", {}});
  if (streams_.size() >= options_.max_streams)
    return Result<int32_t>(
        {Code::kResourceExhausted, "stream limit reached", {}});
  auto bytes = HeaderBytes(metadata, options_.max_metadata_bytes, false);
  if (!bytes.ok()) return Result<int32_t>(bytes.status());
  auto headers = WireHeaders(metadata);
  nghttp2_data_provider2 provider{};
  provider.read_callback = Read;
  int32_t id = nghttp2_submit_request2(session_, nullptr, headers.data(),
                                       headers.size(), &provider, nullptr);
  if (id < 0) return Result<int32_t>(LibraryError(id));
  Stream& stream = streams_[id];
  stream.send_started = true;
  stream.sent_metadata = *bytes.value();
  return Result<int32_t>(id);
}
Status Http2StreamSession::Impl::StartResponse(int32_t id, Metadata metadata) {
  auto found = streams_.find(id);
  if (found == streams_.end() || found->second.closed) return Closed();
  Stream& stream = found->second;
  if (role_ != Http2Session::Role::kServer || stream.send_started)
    return {Code::kFailedPrecondition,
            "response already started or not a server",
            {}};
  auto bytes = HeaderBytes(metadata, options_.max_metadata_bytes, false);
  if (!bytes.ok()) return bytes.status();
  auto headers = WireHeaders(metadata);
  nghttp2_data_provider2 provider{};
  provider.read_callback = Read;
  int code = nghttp2_submit_response2(session_, id, headers.data(),
                                      headers.size(), &provider);
  if (code != 0) return LibraryError(code);
  stream.send_started = true;
  stream.sent_metadata = *bytes.value();
  return {};
}
Result<size_t> Http2StreamSession::Impl::TryWrite(int32_t id,
                                                  std::string_view bytes) {
  auto found = streams_.find(id);
  if (found == streams_.end() || found->second.closed ||
      found->second.send_closed)
    return Result<size_t>(Closed());
  Stream& stream = found->second;
  if (!stream.send_started)
    return Result<size_t>(
        {Code::kFailedPrecondition, "send headers first", {}});
  size_t count =
      std::min(bytes.size(), options_.max_queued_bytes - stream.queued);
  if (count == 0) return Result<size_t>(size_t{0});
  // Coalesce small writes so queue bookkeeping is bounded too, even when an
  // application writes single bytes repeatedly while the peer is stalled.
  std::string_view accepted = bytes.substr(0, count);
  while (!accepted.empty()) {
    if (stream.outgoing.empty() || stream.outgoing.back().size() == 16384)
      stream.outgoing.emplace_back();
    size_t part = std::min<size_t>(16384 - stream.outgoing.back().size(),
                                   accepted.size());
    stream.outgoing.back().append(accepted.substr(0, part));
    accepted.remove_prefix(part);
  }
  stream.queued += count;
  Status status = Resume(id, stream);
  return status.ok() ? Result<size_t>(count) : Result<size_t>(status);
}
Status Http2StreamSession::Impl::CloseSend(int32_t id, Metadata trailers) {
  auto found = streams_.find(id);
  if (found == streams_.end() || found->second.closed ||
      found->second.send_closed)
    return Closed();
  Stream& stream = found->second;
  if (!stream.send_started)
    return {Code::kFailedPrecondition, "send headers first", {}};
  auto bytes = HeaderBytes(
      trailers, options_.max_metadata_bytes - stream.sent_metadata, true);
  if (!bytes.ok()) return bytes.status();
  stream.sent_metadata += *bytes.value();
  stream.trailers = std::move(trailers);
  stream.send_closed = true;
  return Resume(id, stream);
}
Status Http2StreamSession::Impl::Consume(int32_t id, size_t bytes) {
  auto found = streams_.find(id);
  if (found == streams_.end()) return {Code::kNotFound, "unknown stream", {}};
  Stream& stream = found->second;
  if (bytes > stream.unconsumed)
    return {Code::kInvalidArgument, "cannot consume unread bytes", {}};
  int code = stream.wire_closed
                 ? nghttp2_session_consume_connection(session_, bytes)
                 : nghttp2_session_consume(session_, id, bytes);
  if (code != 0) return LibraryError(code);
  stream.unconsumed -= bytes;
  return {};
}
Status Http2StreamSession::Impl::Release(int32_t id) {
  auto found = streams_.find(id);
  if (found == streams_.end()) return {Code::kNotFound, "unknown stream", {}};
  if (!found->second.closed)
    return {Code::kFailedPrecondition, "cannot release a live stream", {}};
  int code =
      nghttp2_session_consume_connection(session_, found->second.unconsumed);
  if (code != 0) return LibraryError(code);
  streams_.erase(found);
  return {};
}
Status Http2StreamSession::Impl::Reset(int32_t id, uint32_t error_code) {
  if (!streams_.contains(id)) return {Code::kNotFound, "unknown stream", {}};
  int code =
      nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, id, error_code);
  if (code != 0) return LibraryError(code);
  NotifyClosed(
      id, {error_code == NGHTTP2_CANCEL ? Code::kCanceled : Code::kInternal,
           "stream reset",
           {}});
  return {};
}
Status Http2StreamSession::Impl::Shutdown() {
  draining_ = true;
  // A client GOAWAY sent before queued request HEADERS would make nghttp2
  // discard those calls. Stop admission now and send GOAWAY after they finish.
  if (role_ == Http2Session::Role::kClient) {
    pending_goaway_ = true;
    return {};
  }
  int code =
      nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE,
                            nghttp2_session_get_last_proc_stream_id(session_),
                            NGHTTP2_NO_ERROR, nullptr, 0);
  return code == 0 ? Status{} : LibraryError(code);
}
void Http2StreamSession::Impl::Fail(Status status) {
  if (!failure_.ok()) return;
  if (status.ok()) status = {Code::kUnavailable, "connection closed", {}};
  failure_ = status;
  draining_ = true;
  std::vector<int32_t> ids;
  for (const auto& [id, stream] : streams_) ids.push_back(id);
  for (int32_t id : ids) NotifyClosed(id, status);
}
Status Http2StreamSession::Impl::Receive(std::string_view bytes) {
  if (!failure_.ok()) return failure_;
  while (!bytes.empty()) {
    nghttp2_ssize count = nghttp2_session_mem_recv2(
        session_, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (count <= 0) {
      Status status =
          count == 0
              ? Status{Code::kInternal, "HTTP/2 parser made no progress", {}}
              : LibraryError(static_cast<int>(count));
      Fail(status);
      return status;
    }
    bytes.remove_prefix(static_cast<size_t>(count));
  }
  return {};
}
Result<std::string> Http2StreamSession::Impl::Drain() {
  if (!failure_.ok()) return Result<std::string>(failure_);
  if (pending_goaway_) {
    bool active = false;
    for (const auto& [id, stream] : streams_) active |= !stream.closed;
    if (!active) {
      int code = nghttp2_submit_goaway(
          session_, NGHTTP2_FLAG_NONE,
          nghttp2_session_get_last_proc_stream_id(session_), NGHTTP2_NO_ERROR,
          nullptr, 0);
      if (code != 0) return Result<std::string>(LibraryError(code));
      pending_goaway_ = false;
    }
  }
  std::string result;
  while (result.size() < 65536) {
    const uint8_t* bytes = nullptr;
    nghttp2_ssize count = nghttp2_session_mem_send2(session_, &bytes);
    if (count < 0) {
      Status status = LibraryError(static_cast<int>(count));
      Fail(status);
      return Result<std::string>(status);
    }
    if (count == 0) break;
    result.append(reinterpret_cast<const char*>(bytes),
                  static_cast<size_t>(count));
  }
  return Result<std::string>(std::move(result));
}

Http2StreamSession::Http2StreamSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
Http2StreamSession::~Http2StreamSession() = default;
Result<std::unique_ptr<Http2StreamSession>> Http2StreamSession::Create(
    Http2Session::Role role, Http2StreamOptions options,
    Http2StreamEvents events) {
  using SessionResult = Result<std::unique_ptr<Http2StreamSession>>;
  if (!events.headers || !events.data || !events.end || !events.close ||
      options.max_metadata_bytes < 32 || options.max_queued_bytes == 0 ||
      options.max_streams == 0 || options.stream_window_bytes < 65535 ||
      options.connection_window_bytes < 65535)
    return SessionResult({Code::kInvalidArgument,
                          "invalid streaming session options or callbacks",
                          {}});
  auto impl = std::make_unique<Impl>(role, options, std::move(events));
  Status status = impl->Initialize();
  if (!status.ok()) return SessionResult(status);
  return SessionResult(std::unique_ptr<Http2StreamSession>(
      new Http2StreamSession(std::move(impl))));
}
Result<int32_t> Http2StreamSession::StartRequest(Metadata headers) {
  return impl_->StartRequest(std::move(headers));
}
Status Http2StreamSession::StartResponse(int32_t id, Metadata headers) {
  return impl_->StartResponse(id, std::move(headers));
}
Result<size_t> Http2StreamSession::TryWrite(int32_t id,
                                            std::string_view bytes) {
  return impl_->TryWrite(id, bytes);
}
Status Http2StreamSession::CloseSend(int32_t id, Metadata trailers) {
  return impl_->CloseSend(id, std::move(trailers));
}
Status Http2StreamSession::Consume(int32_t id, size_t bytes) {
  return impl_->Consume(id, bytes);
}
Status Http2StreamSession::Release(int32_t id) { return impl_->Release(id); }
Status Http2StreamSession::Reset(int32_t id, uint32_t code) {
  return impl_->Reset(id, code);
}
Status Http2StreamSession::Shutdown() { return impl_->Shutdown(); }
Status Http2StreamSession::Receive(std::string_view bytes) {
  return impl_->Receive(bytes);
}
Result<std::string> Http2StreamSession::Drain() { return impl_->Drain(); }
void Http2StreamSession::Fail(Status status) { impl_->Fail(std::move(status)); }
}  // namespace connectrpc
