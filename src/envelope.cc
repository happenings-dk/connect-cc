#include "connectrpc/envelope.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace connectrpc {
namespace {
uint8_t TerminalFlag(Protocol protocol) {
  return protocol == Protocol::kConnect ? 2 : 128;
}
}  // namespace
Result<std::string> EncodeEnvelope(const Envelope& envelope,
                                   Protocol protocol) {
  if (envelope.payload.size() > std::numeric_limits<uint32_t>::max()) {
    return Result<std::string>(
        {Code::kResourceExhausted, "message too large", {}});
  }
  if (envelope.terminal && protocol == Protocol::kGrpc) {
    return Result<std::string>(
        {Code::kInvalidArgument, "invalid terminal flags", {}});
  }
  std::string bytes(5, '\0');
  bytes[0] =
      static_cast<char>((envelope.terminal ? TerminalFlag(protocol) : 0) |
                        (envelope.compressed ? 1 : 0));
  uint32_t length = static_cast<uint32_t>(envelope.payload.size());
  for (int i = 4; i >= 1; --i) {
    bytes[i] = static_cast<char>(length & 255);
    length >>= 8;
  }
  bytes.append(envelope.payload);
  return Result<std::string>(std::move(bytes));
}
EnvelopeDecoder::EnvelopeDecoder(Protocol protocol, size_t max_message_bytes)
    : EnvelopeDecoder(protocol, max_message_bytes, max_message_bytes) {}
EnvelopeDecoder::EnvelopeDecoder(Protocol protocol, size_t max_message_bytes,
                                 size_t max_terminal_bytes)
    : protocol_(protocol),
      limit_(max_message_bytes),
      terminal_limit_(max_terminal_bytes) {}
Result<std::optional<Envelope>> EnvelopeDecoder::ReadOne(
    std::string_view& bytes) {
  using Next = Result<std::optional<Envelope>>;
  if (!status_.ok()) return Next(status_);
  if (bytes.empty()) return Next(std::nullopt);
  if (terminal_) {
    status_ = {Code::kInvalidArgument, "data after terminal envelope", {}};
    return Next(status_);
  }
  while (header_size_ < 5 && !bytes.empty()) {
    header_[header_size_++] = static_cast<uint8_t>(bytes.front());
    bytes.remove_prefix(1);
  }
  if (header_size_ < 5) return Next(std::nullopt);
  if (envelope_.payload.empty()) {
    uint8_t flags = header_[0];
    uint8_t allowed =
        protocol_ == Protocol::kGrpc ? 1 : 1 | TerminalFlag(protocol_);
    if ((flags & ~allowed) != 0) {
      status_ = {Code::kInvalidArgument, "invalid envelope flags", {}};
      return Next(status_);
    }
    length_ = 0;
    for (int i = 1; i < 5; ++i) length_ = (length_ << 8) | header_[i];
    bool terminal = (flags & TerminalFlag(protocol_)) != 0;
    size_t limit = terminal ? terminal_limit_ : limit_;
    if (length_ > limit) {
      status_ = {Code::kResourceExhausted, "message exceeds limit", {}};
      return Next(status_);
    }
    envelope_.compressed = (flags & 1) != 0;
    envelope_.terminal = terminal;
  }
  size_t count = std::min(length_ - envelope_.payload.size(), bytes.size());
  envelope_.payload.append(bytes.substr(0, count));
  bytes.remove_prefix(count);
  if (envelope_.payload.size() != length_) return Next(std::nullopt);
  terminal_ = envelope_.terminal;
  Envelope complete = std::move(envelope_);
  envelope_ = {};
  header_size_ = 0;
  return Next(std::move(complete));
}
Status EnvelopeDecoder::Feed(std::string_view bytes,
                             const std::function<Status(Envelope)>& sink) {
  if (!status_.ok()) return status_;
  while (!bytes.empty()) {
    auto next = ReadOne(bytes);
    if (!next.ok()) return next.status();
    if (*next.value()) {
      status_ = sink(std::move(**next.value()));
      if (!status_.ok()) return status_;
    }
  }
  return status_;
}
Status EnvelopeDecoder::Finish() const {
  if (!status_.ok()) return status_;
  if (header_size_ != 0) return {Code::kDataLoss, "truncated envelope", {}};
  return {};
}
}  // namespace connectrpc
