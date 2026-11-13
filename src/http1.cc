#include "connectrpc/http1.h"

#include <llhttp.h>

#include <algorithm>
#include <charconv>
#include <utility>
#include <vector>

#include "protocol_internal.h"

namespace connectrpc {
namespace {
using EventResult = Result<std::optional<Http1Event>>;
using internal_protocol::Lower;
using internal_protocol::Single;
using internal_protocol::Trim;
Status Invalid(std::string message) {
  return {Code::kInvalidArgument, std::move(message), {}};
}
Status Precondition(std::string message) {
  return {Code::kFailedPrecondition, std::move(message), {}};
}
Status TooLarge() {
  return {
      Code::kResourceExhausted, "HTTP/1 metadata or framing exceeds limit", {}};
}
bool Token(std::string_view name) {
  if (name.empty()) return false;
  for (unsigned char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          std::string_view("!#$%&'*+-.^_`|~").find(c) !=
              std::string_view::npos))
      return false;
  return true;
}
bool Value(std::string_view value) {
  for (unsigned char c : value)
    if ((c < 32 && c != '\t') || c == 127) return false;
  return true;
}
bool Authority(std::string_view authority) {
  if (authority.empty()) return false;
  for (unsigned char c : authority)
    if (c <= 32 || c >= 127 ||
        std::string_view("/@?#,\\").find(c) != std::string_view::npos)
      return false;
  return true;
}
bool Target(std::string_view target) {
  if (target.empty() || (target.front() != '/' && target != "*")) return false;
  for (unsigned char c : target)
    if (c <= 32 || c >= 127 || c == '#') return false;
  return true;
}
bool ForbiddenTrailer(std::string_view name) {
  return name == "content-length" || name == "transfer-encoding" ||
         name == "host" || name == "connection" || name == "te" ||
         name == "trailer" || name == "upgrade" || name == "keep-alive" ||
         name == "proxy-connection" || name == "content-encoding" ||
         name == "content-type" || name == "content-range" ||
         name == "authorization" || name == "proxy-authorization" ||
         name == "proxy-authenticate" || name == "www-authenticate" ||
         name == "cache-control" || name == "expect";
}
Status Charge(size_t amount, size_t& remaining) {
  if (amount > remaining) return TooLarge();
  remaining -= amount;
  return {};
}
Status ValidateFields(const Metadata& fields, size_t limit, bool trailers) {
  for (const auto& [name, value] : fields) {
    if (!Token(name) || Lower(name) != name || !Value(value) ||
        (trailers && ForbiddenTrailer(name)))
      return Invalid("invalid HTTP/1 field");
    for (size_t size : {name.size(), value.size(), size_t{32}}) {
      Status status = Charge(size, limit);
      if (!status.ok()) return status;
    }
  }
  return {};
}
Result<uint64_t> ContentLength(std::string_view text) {
  uint64_t value = 0;
  if (text.empty()) return Result<uint64_t>(Invalid("empty content-length"));
  for (char c : text)
    if (c < '0' || c > '9')
      return Result<uint64_t>(Invalid("invalid content-length"));
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{})
    return Result<uint64_t>(Invalid("invalid content-length"));
  return Result<uint64_t>(value);
}
std::vector<std::string> ConnectionTokens(const Metadata& headers) {
  std::vector<std::string> tokens;
  for (const auto& [name, value] : headers) {
    if (name != "connection") continue;
    std::string_view rest(value);
    for (;;) {
      size_t comma = rest.find(',');
      tokens.push_back(Lower(Trim(rest.substr(0, comma))));
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1);
    }
  }
  return tokens;
}
std::string Fields(const Metadata& fields) {
  std::string output;
  for (const auto& [name, value] : fields)
    output += name + ": " + value + "\r\n";
  return output;
}
}  // namespace
class Http1Parser::Impl {
 public:
  Impl(Http1Role role, Http1Limits limits)
      : role_(role), limits_(limits), remaining_(limits.max_metadata_bytes) {
    llhttp_settings_init(&settings_);
    settings_.on_url = [](llhttp_t* parser, const char* data, size_t size) {
      return Self(parser).Append(Self(parser).target_, data, size);
    };
    settings_.on_status = [](llhttp_t* parser, const char*, size_t size) {
      return Self(parser).Count(size);
    };
    settings_.on_header_field = [](llhttp_t* parser, const char* data,
                                   size_t size) {
      return Self(parser).Append(Self(parser).field_, data, size);
    };
    settings_.on_header_value = [](llhttp_t* parser, const char* data,
                                   size_t size) {
      return Self(parser).Append(Self(parser).value_, data, size);
    };
    settings_.on_header_value_complete = [](llhttp_t* parser) {
      return Self(parser).FieldComplete();
    };
    settings_.on_headers_complete = [](llhttp_t* parser) {
      return Self(parser).HeadersComplete();
    };
    settings_.on_body = [](llhttp_t* parser, const char* data,
                           size_t size) -> int {
      if (size == 0) return 0;
      auto& self = Self(parser);
      self.body_bytes_ = size;
      self.event_ = Http1Event{Http1Event::Kind::kBody,
                               {},
                               std::string(data, size),
                               self.keep_alive_};
      return HPE_PAUSED;
    };
    settings_.on_chunk_header = [](llhttp_t* parser) {
      auto& self = Self(parser);
      if (parser->content_length == 0) {
        self.trailers_ = true;
        self.remaining_ = self.limits_.max_metadata_bytes;
      }
      return 0;
    };
    settings_.on_message_complete = [](llhttp_t* parser) -> int {
      auto& self = Self(parser);
      self.complete_ = true;
      self.event_ = Http1Event{Http1Event::Kind::kEnd,
                               std::move(self.fields_),
                               {},
                               self.keep_alive_};
      return HPE_PAUSED;
    };
    llhttp_init(&parser_,
                role == Http1Role::kServer ? HTTP_REQUEST : HTTP_RESPONSE,
                &settings_);
    parser_.data = this;
  }
  EventResult ReadOne(std::string_view& bytes);
  EventResult EndInput();
  Status NextMessage();

 private:
  static Impl& Self(llhttp_t* parser) {
    return *static_cast<Impl*>(parser->data);
  }
  int Count(size_t size) {
    error_ = Charge(size, remaining_);
    return error_.ok() ? 0 : HPE_USER;
  }
  int Append(std::string& destination, const char* data, size_t size) {
    int error = Count(size);
    if (error == 0) destination.append(data, size);
    return error;
  }
  int Fail(Status status) {
    error_ = std::move(status);
    return HPE_USER;
  }
  int FieldComplete();
  int HeadersComplete();
  EventResult TakeEvent();
  Http1Role role_;
  Http1Limits limits_;
  llhttp_settings_t settings_{};
  llhttp_t parser_{};
  size_t remaining_;
  size_t framing_bytes_ = 0;
  size_t body_bytes_ = 0;
  std::string target_;
  std::string field_;
  std::string value_;
  Metadata fields_;
  std::vector<std::string> connection_tokens_;
  std::optional<Http1Event> event_;
  Status error_;
  bool paused_ = false;
  bool complete_ = false;
  bool keep_alive_ = false;
  bool trailers_ = false;
  bool input_ended_ = false;
};
int Http1Parser::Impl::FieldComplete() {
  if (Count(32) != 0) return HPE_USER;
  field_ = Lower(field_);
  if (!Token(field_) || !Value(value_))
    return Fail(Invalid("invalid HTTP/1 field"));
  if (trailers_ &&
      (ForbiddenTrailer(field_) ||
       std::find(connection_tokens_.begin(), connection_tokens_.end(),
                 field_) != connection_tokens_.end()))
    return Fail(Invalid("forbidden HTTP/1 trailer"));
  fields_.emplace_back(std::move(field_), std::string(Trim(value_)));
  field_.clear();
  value_.clear();
  return 0;
}
int Http1Parser::Impl::HeadersComplete() {
  if (parser_.http_major != 1 || parser_.http_minor != 1)
    return Fail({Code::kUnimplemented, "requires HTTP/1.1", {}});
  if (parser_.upgrade || parser_.method == HTTP_CONNECT ||
      parser_.status_code == 101)
    return Fail(
        {Code::kUnimplemented, "HTTP upgrade/tunneling is unsupported", {}});
  auto transfer = Single(fields_, "transfer-encoding");
  if (!transfer.ok()) return Fail(transfer.status());
  // Single uses an empty string for both absence and an empty field. Reject
  // the latter explicitly, including with older supported llhttp versions.
  for (const auto& [name, value] : fields_)
    if (name == "transfer-encoding" && Trim(value).empty())
      return Fail(Invalid("empty transfer encoding"));
  if (!transfer.value()->empty() && Lower(Trim(*transfer.value())) != "chunked")
    return Fail({Code::kUnimplemented, "unsupported transfer encoding", {}});
  if (role_ == Http1Role::kClient &&
      (parser_.status_code < 200 || parser_.status_code == 204)) {
    for (const auto& [name, value] : fields_)
      if (name == "content-length" || name == "transfer-encoding")
        return Fail(Invalid("body framing forbidden for this HTTP status"));
  }
  connection_tokens_ = ConnectionTokens(fields_);
  for (const auto& token : connection_tokens_)
    if (!Token(token)) return Fail(Invalid("invalid connection token"));
  // Framing and authority must not be hidden by connection-specific fields.
  for (const auto& token : connection_tokens_)
    if (token == "content-length" || token == "transfer-encoding" ||
        token == "host")
      return Fail(Invalid("connection token names a framing field"));
  keep_alive_ = llhttp_should_keep_alive(&parser_) != 0;
  Metadata headers;
  if (role_ == Http1Role::kServer) {
    auto host = Single(fields_, "host");
    if (!host.ok()) return Fail(host.status());
    if (!Authority(*host.value())) return Fail(Invalid("invalid HTTP/1 host"));
    std::string scheme;
    size_t scheme_end = target_.find("://");
    if (!target_.starts_with('/') && scheme_end != std::string::npos) {
      scheme = Lower(std::string_view(target_).substr(0, scheme_end));
      if (scheme != "http" && scheme != "https")
        return Fail(Invalid("unsupported URI scheme"));
      std::string_view rest(target_.data() + scheme_end + 3,
                            target_.size() - scheme_end - 3);
      size_t path = rest.find_first_of("/?");
      std::string authority(rest.substr(0, path));
      if (!Authority(authority)) return Fail(Invalid("invalid URI authority"));
      std::string normalized =
          path == std::string_view::npos ? "/" : std::string(rest.substr(path));
      if (normalized.front() == '?') normalized.insert(normalized.begin(), '/');
      target_ = std::move(normalized);
      // RFC 9112 absolute-form authority takes precedence over Host.
      *host.value() = std::move(authority);
    }
    if (!Target(target_) || (target_ == "*" && parser_.method != HTTP_OPTIONS))
      return Fail(Invalid("invalid HTTP/1 authority or request target"));
    headers = {{":method", llhttp_method_name(
                               static_cast<llhttp_method_t>(parser_.method))},
               {":path", std::move(target_)},
               {":authority", std::move(*host.value())}};
    if (!scheme.empty()) headers.emplace_back(":scheme", std::move(scheme));
  } else {
    // Preserve syntactically valid, unrecognized statuses for the RPC layer's
    // HTTP-to-RPC fallback mapping (for example, status 800 maps to unknown).
    if (parser_.status_code < 100 || parser_.status_code > 999)
      return Fail(Invalid("invalid HTTP status"));
    headers.emplace_back(":status", std::to_string(parser_.status_code));
  }
  for (auto& [name, value] : fields_) {
    if (name == "host" || name == "connection" || name == "transfer-encoding" ||
        name == "keep-alive" || name == "proxy-connection" ||
        name == "upgrade" || name == "trailer" || name == "te" ||
        std::find(connection_tokens_.begin(), connection_tokens_.end(), name) !=
            connection_tokens_.end())
      continue;
    headers.emplace_back(std::move(name), std::move(value));
  }
  fields_.clear();
  event_ = Http1Event{
      Http1Event::Kind::kHeaders, std::move(headers), {}, keep_alive_};
  return HPE_PAUSED;
}
EventResult Http1Parser::Impl::TakeEvent() {
  auto event = std::move(event_);
  event_.reset();
  return EventResult(std::move(event));
}
EventResult Http1Parser::Impl::ReadOne(std::string_view& bytes) {
  if (!error_.ok()) return EventResult(error_);
  if (input_ended_)
    return EventResult(Precondition("HTTP input already ended"));
  if (complete_)
    return EventResult(
        Precondition("call NextMessage before parsing another message"));
  if (paused_) {
    llhttp_resume(&parser_);
    paused_ = false;
  }
  body_bytes_ = 0;
  size_t size = std::min(bytes.size(), limits_.max_chunk_bytes);
  const char* data = bytes.empty() ? "" : bytes.data();
  auto result = llhttp_execute(&parser_, data, size);
  size_t consumed =
      result == HPE_OK
          ? size
          : static_cast<size_t>(llhttp_get_error_pos(&parser_) - data);
  if (consumed > size || body_bytes_ > consumed)
    return EventResult(
        error_ = Status{Code::kInternal, "invalid HTTP parser cursor", {}});
  bytes.remove_prefix(consumed);
  size_t framing = consumed - body_bytes_;
  if (framing > limits_.max_metadata_bytes - framing_bytes_)
    return EventResult(error_ = TooLarge());
  framing_bytes_ += framing;
  if (event_) framing_bytes_ = 0;
  if (!error_.ok()) return EventResult(error_);
  if (result != HPE_OK && result != HPE_PAUSED)
    return EventResult(error_ =
                           Invalid(std::string("invalid HTTP/1 message: ") +
                                   llhttp_errno_name(result)));
  paused_ = result == HPE_PAUSED;
  return TakeEvent();
}
EventResult Http1Parser::Impl::EndInput() {
  if (!error_.ok()) return EventResult(error_);
  if (input_ended_)
    return EventResult(Precondition("HTTP input already ended"));
  input_ended_ = true;
  if (complete_) return EventResult(std::nullopt);
  if (paused_) {
    llhttp_resume(&parser_);
    paused_ = false;
  }
  auto result = llhttp_finish(&parser_);
  if (!error_.ok()) return EventResult(error_);
  if (result != HPE_OK && result != HPE_PAUSED)
    return EventResult(
        error_ = Status{Code::kDataLoss, "truncated HTTP/1 message", {}});
  if (!complete_)
    return EventResult(
        error_ = Status{Code::kDataLoss, "missing HTTP/1 message", {}});
  return TakeEvent();
}
Status Http1Parser::Impl::NextMessage() {
  if (!error_.ok()) return error_;
  if (!complete_ || !keep_alive_ || input_ended_)
    return Precondition("HTTP/1 connection cannot start another message");
  llhttp_reset(&parser_);
  remaining_ = limits_.max_metadata_bytes;
  framing_bytes_ = 0;
  body_bytes_ = 0;
  target_.clear();
  fields_.clear();
  connection_tokens_.clear();
  event_.reset();
  paused_ = false;
  complete_ = false;
  keep_alive_ = false;
  trailers_ = false;
  return {};
}
Http1Parser::Http1Parser(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Http1Parser::~Http1Parser() = default;
Result<std::unique_ptr<Http1Parser>> Http1Parser::Create(Http1Role role,
                                                         Http1Limits limits) {
  if (limits.max_metadata_bytes == 0 || limits.max_chunk_bytes == 0)
    return Result<std::unique_ptr<Http1Parser>>(
        Invalid("HTTP/1 limits must be positive"));
  return Result<std::unique_ptr<Http1Parser>>(std::unique_ptr<Http1Parser>(
      new Http1Parser(std::make_unique<Impl>(role, limits))));
}
EventResult Http1Parser::ReadOne(std::string_view& bytes) {
  return impl_->ReadOne(bytes);
}
EventResult Http1Parser::EndInput() { return impl_->EndInput(); }
Status Http1Parser::NextMessage() { return impl_->NextMessage(); }
Http1Encoder::Http1Encoder(Http1Role role, Http1Limits limits)
    : role_(role), limits_(limits) {}
Result<std::string> Http1Encoder::Headers(const Metadata& headers,
                                          bool keep_alive) {
  if (started_)
    return Result<std::string>(Precondition("HTTP headers already sent"));
  if (limits_.max_chunk_bytes == 0 || limits_.max_metadata_bytes == 0)
    return Result<std::string>(Invalid("HTTP/1 limits must be positive"));
  // Include pseudo-headers in the budget and validate before copying fields or
  // constructing the request/status line. Caller-owned oversized input must
  // not cause another unbounded allocation in the encoder.
  size_t remaining_metadata = limits_.max_metadata_bytes;
  for (const auto& [name, value] : headers) {
    for (size_t size : {name.size(), value.size(), size_t{32}}) {
      Status status = Charge(size, remaining_metadata);
      if (!status.ok()) return Result<std::string>(status);
    }
  }
  Metadata fields;
  for (const auto& [name, value] : headers) {
    if (!name.empty() && name.front() == ':') {
      if ((role_ == Http1Role::kClient && name != ":method" &&
           name != ":path" && name != ":scheme" && name != ":authority") ||
          (role_ == Http1Role::kServer && name != ":status"))
        return Result<std::string>(Invalid("unexpected HTTP pseudo-header"));
      continue;
    }
    if (name == "connection" || name == "transfer-encoding" ||
        name == "trailer" || name == "upgrade" || name == "te" ||
        name == "keep-alive" || name == "proxy-connection")
      return Result<std::string>(
          Invalid("HTTP encoder owns framing and connection headers"));
    fields.emplace_back(name, value);
  }
  Status valid = ValidateFields(fields, limits_.max_metadata_bytes, false);
  if (!valid.ok()) return Result<std::string>(valid);
  auto length = Single(fields, "content-length");
  if (!length.ok()) return Result<std::string>(length.status());
  std::optional<uint64_t> remaining;
  if (!length.value()->empty()) {
    auto parsed = ContentLength(*length.value());
    if (!parsed.ok()) return Result<std::string>(parsed.status());
    remaining = *parsed.value();
  } else if (std::any_of(fields.begin(), fields.end(), [](const auto& field) {
               return field.first == "content-length";
             })) {
    return Result<std::string>(Invalid("empty content-length"));
  }
  std::string line;
  bool no_body = false;
  if (role_ == Http1Role::kClient) {
    auto method = Single(headers, ":method");
    auto target = Single(headers, ":path");
    auto authority = Single(headers, ":authority");
    auto host = Single(fields, "host");
    auto scheme = Single(headers, ":scheme");
    if (!method.ok()) return Result<std::string>(method.status());
    if (!target.ok()) return Result<std::string>(target.status());
    if (!authority.ok()) return Result<std::string>(authority.status());
    if (!host.ok()) return Result<std::string>(host.status());
    if (!scheme.ok()) return Result<std::string>(scheme.status());
    if (!Token(*method.value()) || !Target(*target.value()) ||
        (*target.value() == "*" && *method.value() != "OPTIONS") ||
        (!scheme.value()->empty() && *scheme.value() != "http" &&
         *scheme.value() != "https"))
      return Result<std::string>(Invalid("invalid HTTP request line"));
    if (*method.value() == "CONNECT" || *method.value() == "HEAD")
      return Result<std::string>(
          Status{Code::kUnimplemented,
                 "HTTP method unsupported by RPC transport",
                 {}});
    if (!authority.value()->empty() && !host.value()->empty() &&
        *authority.value() != *host.value())
      return Result<std::string>(Invalid("conflicting HTTP authority"));
    std::string hostname =
        authority.value()->empty() ? *host.value() : *authority.value();
    if (!Authority(hostname))
      return Result<std::string>(Invalid("invalid HTTP authority"));
    std::erase_if(fields,
                  [](const auto& field) { return field.first == "host"; });
    fields.emplace_back("host", std::move(hostname));
    line = *method.value() + " " + *target.value() + " HTTP/1.1\r\n";
    if (!remaining &&
        (*method.value() == "GET" || *method.value() == "OPTIONS")) {
      remaining = 0;
      fields.emplace_back("content-length", "0");
    }
  } else {
    auto status = Single(headers, ":status");
    if (!status.ok()) return Result<std::string>(status.status());
    auto code = ContentLength(*status.value());
    if (!code.ok() || status.value()->size() != 3 || *code.value() < 200 ||
        *code.value() > 599)
      return Result<std::string>(Invalid("invalid final HTTP status"));
    no_body = *code.value() == 204 || *code.value() == 304;
    if (*code.value() == 204 && remaining)
      return Result<std::string>(
          Invalid("204 response cannot have content-length"));
    line = "HTTP/1.1 " + *status.value() + " \r\n";
  }
  bool chunked = !remaining && !no_body;
  if (chunked) fields.emplace_back("transfer-encoding", "chunked");
  if (!keep_alive) fields.emplace_back("connection", "close");
  valid = ValidateFields(fields, limits_.max_metadata_bytes, false);
  if (!valid.ok()) return Result<std::string>(valid);
  std::string output = line + Fields(fields) + "\r\n";
  if (output.size() > limits_.max_metadata_bytes)
    return Result<std::string>(TooLarge());
  started_ = true;
  chunked_ = chunked;
  remaining_ = no_body ? std::optional<uint64_t>(0) : remaining;
  return Result<std::string>(std::move(output));
}
Result<std::string> Http1Encoder::Body(std::string_view bytes) {
  if (!started_ || ended_)
    return Result<std::string>(Precondition("HTTP body not writable"));
  if (bytes.size() > limits_.max_chunk_bytes)
    return Result<std::string>(
        Status{Code::kResourceExhausted, "HTTP body chunk exceeds limit", {}});
  if (remaining_ && bytes.size() > *remaining_)
    return Result<std::string>(Invalid("HTTP body exceeds content-length"));
  if (remaining_) *remaining_ -= bytes.size();
  if (!chunked_ || bytes.empty())
    return Result<std::string>(std::string(bytes));
  char hex[2 * sizeof(size_t)];
  auto encoded = std::to_chars(hex, hex + sizeof(hex), bytes.size(), 16);
  return Result<std::string>(std::string(hex, encoded.ptr) + "\r\n" +
                             std::string(bytes) + "\r\n");
}
Result<std::string> Http1Encoder::End(const Metadata& trailers) {
  if (!started_ || ended_)
    return Result<std::string>(Precondition("HTTP send direction not open"));
  if (remaining_ && *remaining_ != 0)
    return Result<std::string>(
        Invalid("HTTP body shorter than content-length"));
  if (!chunked_ && !trailers.empty())
    return Result<std::string>(
        Invalid("HTTP trailers require chunked transfer"));
  Status valid = ValidateFields(trailers, limits_.max_metadata_bytes, true);
  if (!valid.ok()) return Result<std::string>(valid);
  ended_ = true;
  return Result<std::string>(chunked_ ? "0\r\n" + Fields(trailers) + "\r\n"
                                      : "");
}
}  // namespace connectrpc
