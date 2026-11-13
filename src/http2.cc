#include "connectrpc/http2.h"

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace connectrpc {
struct Http2Session::Impl {
  struct Stream {
    HttpMessage incoming;
    size_t metadata_bytes = 0;
    std::string outgoing;
    size_t sent = 0;
    bool delivered = false;
    bool failed = false;
  };
  nghttp2_session* session = nullptr;
  Role role;
  size_t max_body;
  size_t max_metadata;
  std::map<int32_t, Stream> streams;
  std::function<void(HttpMessage)> handler;
  std::function<void(int32_t, Status)> on_error;
  bool draining = false;
  void Fail(int32_t id, Status status) {
    auto found = streams.find(id);
    if (found == streams.end() || found->second.failed) return;
    found->second.failed = true;
    if ((!found->second.delivered || role == Role::kServer) && on_error)
      on_error(id, std::move(status));
  }
  ~Impl() {
    if (session) nghttp2_session_del(session);
  }
  static int Header(nghttp2_session*, const nghttp2_frame* frame,
                    const uint8_t* name, size_t name_len, const uint8_t* value,
                    size_t value_len, uint8_t, void* data) {
    auto& self = *static_cast<Impl*>(data);
    if (self.streams.size() >= 100 &&
        !self.streams.contains(frame->hd.stream_id)) {
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    Stream& stream = self.streams[frame->hd.stream_id];
    if (stream.failed) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    if (name_len > self.max_metadata - stream.metadata_bytes ||
        value_len > self.max_metadata - stream.metadata_bytes - name_len) {
      self.Fail(frame->hd.stream_id,
                {Code::kResourceExhausted, "metadata exceeds limit", {}});
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    stream.metadata_bytes += name_len + value_len;
    stream.incoming.stream_id = frame->hd.stream_id;
    stream.incoming.headers.emplace_back(
        std::string(reinterpret_cast<const char*>(name), name_len),
        std::string(reinterpret_cast<const char*>(value), value_len));
    return 0;
  }
  static int Data(nghttp2_session*, uint8_t, int32_t id, const uint8_t* bytes,
                  size_t count, void* data) {
    auto& self = *static_cast<Impl*>(data);
    auto found = self.streams.find(id);
    if (found == self.streams.end() || found->second.failed) return 0;
    std::string& body = found->second.incoming.body;
    if (count > self.max_body - body.size()) {
      self.Fail(id, {Code::kResourceExhausted, "body exceeds limit", {}});
      return nghttp2_submit_rst_stream(self.session, NGHTTP2_FLAG_NONE, id,
                                       NGHTTP2_ENHANCE_YOUR_CALM);
    }
    body.append(reinterpret_cast<const char*>(bytes), count);
    return 0;
  }
  static int Frame(nghttp2_session*, const nghttp2_frame* frame, void* data) {
    auto& self = *static_cast<Impl*>(data);
    if (frame->hd.type == NGHTTP2_GOAWAY) {
      self.draining = true;
      for (auto& [id, stream] : self.streams) {
        if (id > frame->goaway.last_stream_id ||
            frame->goaway.error_code != 0) {
          self.Fail(id, {Code::kUnavailable, "peer sent GOAWAY", {}});
        }
      }
    }
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
      auto found = self.streams.find(frame->hd.stream_id);
      if (found != self.streams.end() && !found->second.failed &&
          !found->second.delivered) {
        found->second.delivered = true;
        self.handler(std::move(found->second.incoming));
      }
    }
    return 0;
  }
  static int Close(nghttp2_session*, int32_t id, uint32_t code, void* data) {
    auto& self = *static_cast<Impl*>(data);
    self.Fail(id,
              {code == NGHTTP2_CANCEL ? Code::kCanceled : Code::kUnavailable,
               "HTTP/2 stream closed before response",
               {}});
    self.streams.erase(id);
    return 0;
  }
  static nghttp2_ssize Read(nghttp2_session*, int32_t id, uint8_t* buffer,
                            size_t length, uint32_t* flags,
                            nghttp2_data_source*, void* data) {
    auto& self = *static_cast<Impl*>(data);
    auto found = self.streams.find(id);
    if (found == self.streams.end())
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    Stream& stream = found->second;
    size_t count = std::min(length, stream.outgoing.size() - stream.sent);
    std::memcpy(buffer, stream.outgoing.data() + stream.sent, count);
    stream.sent += count;
    if (stream.sent == stream.outgoing.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<nghttp2_ssize>(count);
  }
};
namespace {
std::vector<nghttp2_nv> Headers(Metadata& metadata) {
  std::vector<nghttp2_nv> headers;
  for (auto& [name, value] : metadata) {
    headers.push_back({reinterpret_cast<uint8_t*>(name.data()),
                       reinterpret_cast<uint8_t*>(value.data()), name.size(),
                       value.size(), NGHTTP2_NV_FLAG_NONE});
  }
  return headers;
}
Status Http2Error(int code) {
  return {Code::kInternal, nghttp2_strerror(code), {}};
}
Status ValidateMetadata(const Metadata& metadata, size_t limit) {
  size_t total = 0;
  for (const auto& [name, value] : metadata) {
    if (name.size() > limit - total ||
        value.size() > limit - total - name.size()) {
      return {Code::kResourceExhausted, "metadata exceeds limit", {}};
    }
    if (!nghttp2_check_header_name(
            reinterpret_cast<const uint8_t*>(name.data()), name.size()) ||
        !nghttp2_check_header_value(
            reinterpret_cast<const uint8_t*>(value.data()), value.size())) {
      return {Code::kInvalidArgument, "invalid HTTP/2 header", {}};
    }
    total += name.size() + value.size();
  }
  return {};
}
}  // namespace
Http2Session::Http2Session(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
Http2Session::~Http2Session() = default;
Result<std::unique_ptr<Http2Session>> Http2Session::Create(
    Role role, size_t max_body_bytes, size_t max_metadata_bytes,
    std::function<void(HttpMessage)> handler,
    std::function<void(int32_t, Status)> on_error) {
  using SessionResult = Result<std::unique_ptr<Http2Session>>;
  if (!handler)
    return SessionResult({Code::kInvalidArgument, "missing handler", {}});
  auto impl = std::make_unique<Impl>();
  impl->role = role;
  impl->max_body = max_body_bytes;
  impl->max_metadata = max_metadata_bytes;
  impl->handler = std::move(handler);
  impl->on_error = std::move(on_error);
  nghttp2_session_callbacks* callbacks = nullptr;
  int code = nghttp2_session_callbacks_new(&callbacks);
  if (code != 0) return SessionResult(Http2Error(code));
  nghttp2_session_callbacks_set_on_header_callback(callbacks, Impl::Header);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                            Impl::Data);
  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, Impl::Frame);
  nghttp2_session_callbacks_set_on_stream_close_callback(callbacks,
                                                         Impl::Close);
  code =
      role == Role::kClient
          ? nghttp2_session_client_new(&impl->session, callbacks, impl.get())
          : nghttp2_session_server_new(&impl->session, callbacks, impl.get());
  nghttp2_session_callbacks_del(callbacks);
  if (code != 0) return SessionResult(Http2Error(code));
  nghttp2_settings_entry settings[] = {
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
      {NGHTTP2_SETTINGS_ENABLE_PUSH, 0}};
  code = nghttp2_submit_settings(impl->session, NGHTTP2_FLAG_NONE, settings,
                                 role == Role::kClient ? 2 : 1);
  if (code != 0) return SessionResult(Http2Error(code));
  return SessionResult(
      std::unique_ptr<Http2Session>(new Http2Session(std::move(impl))));
}
Result<int32_t> Http2Session::Request(Metadata metadata, std::string body) {
  if (impl_->draining)
    return Result<int32_t>({Code::kUnavailable, "session draining", {}});
  Status validation = ValidateMetadata(metadata, impl_->max_metadata);
  if (!validation.ok()) return Result<int32_t>(validation);
  if (impl_->role != Role::kClient) {
    return Result<int32_t>({Code::kFailedPrecondition, "not a client", {}});
  }
  if (body.size() > impl_->max_body || impl_->streams.size() >= 100) {
    return Result<int32_t>(
        {Code::kResourceExhausted, "request exceeds limits", {}});
  }
  auto headers = Headers(metadata);
  nghttp2_data_provider2 provider{};
  provider.read_callback = Impl::Read;
  int32_t id = nghttp2_submit_request2(impl_->session, nullptr, headers.data(),
                                       headers.size(), &provider, nullptr);
  if (id < 0) return Result<int32_t>(Http2Error(id));
  impl_->streams[id].outgoing = std::move(body);
  return Result<int32_t>(id);
}
Status Http2Session::Respond(int32_t id, Metadata metadata, std::string body) {
  Status validation = ValidateMetadata(metadata, impl_->max_metadata);
  if (!validation.ok()) return validation;
  if (impl_->role != Role::kServer || !impl_->streams.contains(id)) {
    return {Code::kFailedPrecondition, "unknown server stream", {}};
  }
  if (body.size() > impl_->max_body) {
    return {Code::kResourceExhausted, "response exceeds limits", {}};
  }
  auto headers = Headers(metadata);
  nghttp2_data_provider2 provider{};
  provider.read_callback = Impl::Read;
  int code = nghttp2_submit_response2(impl_->session, id, headers.data(),
                                      headers.size(), &provider);
  if (code != 0) return Http2Error(code);
  impl_->streams[id].outgoing = std::move(body);
  return {};
}
Status Http2Session::Receive(std::string_view bytes) {
  while (!bytes.empty()) {
    nghttp2_ssize count = nghttp2_session_mem_recv2(
        impl_->session, reinterpret_cast<const uint8_t*>(bytes.data()),
        bytes.size());
    if (count < 0) return Http2Error(static_cast<int>(count));
    if (count == 0)
      return {Code::kInternal, "HTTP/2 parser made no progress", {}};
    bytes.remove_prefix(static_cast<size_t>(count));
  }
  return {};
}
Result<std::string> Http2Session::Drain() {
  std::string output;
  const uint8_t* bytes;
  while (output.size() < 65536) {
    nghttp2_ssize count = nghttp2_session_mem_send2(impl_->session, &bytes);
    if (count < 0)
      return Result<std::string>(Http2Error(static_cast<int>(count)));
    if (count == 0) break;
    output.append(reinterpret_cast<const char*>(bytes),
                  static_cast<size_t>(count));
  }
  return Result<std::string>(std::move(output));
}
Status Http2Session::Reset(int32_t id, uint32_t error_code) {
  if (!impl_->streams.contains(id))
    return {Code::kNotFound, "unknown stream", {}};
  int code = nghttp2_submit_rst_stream(impl_->session, NGHTTP2_FLAG_NONE, id,
                                       error_code);
  if (code != 0) return Http2Error(code);
  impl_->Fail(id, {Code::kCanceled, "stream canceled", {}});
  return {};
}
Status Http2Session::Shutdown() {
  impl_->draining = true;
  int code = nghttp2_submit_goaway(
      impl_->session, NGHTTP2_FLAG_NONE,
      nghttp2_session_get_last_proc_stream_id(impl_->session), NGHTTP2_NO_ERROR,
      nullptr, 0);
  return code == 0 ? Status{} : Http2Error(code);
}
}  // namespace connectrpc
