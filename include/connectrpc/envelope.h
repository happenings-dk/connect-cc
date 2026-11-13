#ifndef CONNECTCPP_INCLUDE_CONNECTRPC_ENVELOPE_H_
#define CONNECTCPP_INCLUDE_CONNECTRPC_ENVELOPE_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "connectrpc/status.h"

namespace connectrpc {
enum class Protocol { kConnect, kGrpc, kGrpcWeb };
struct Envelope {
  bool compressed = false;
  bool terminal = false;
  std::string payload;
};
Result<std::string> EncodeEnvelope(const Envelope& envelope, Protocol protocol);
// Incremental, bounded parser. The sink must consume each message
// synchronously. Instances belong to one stream; errors are sticky. Finish
// checks truncation.
class EnvelopeDecoder {
 public:
  explicit EnvelopeDecoder(Protocol protocol, size_t max_message_bytes);
  EnvelopeDecoder(Protocol protocol, size_t max_message_bytes,
                  size_t max_terminal_bytes);
  // Consumes only through the next envelope, leaving remaining bytes in the
  // view. A nullopt result needs more input. Never queues complete messages.
  Result<std::optional<Envelope>> ReadOne(std::string_view& bytes);
  Status Feed(std::string_view bytes,
              const std::function<Status(Envelope)>& sink);
  Status Finish() const;

 private:
  Protocol protocol_;
  size_t limit_;
  size_t terminal_limit_;
  std::array<uint8_t, 5> header_{};
  size_t header_size_ = 0;
  size_t length_ = 0;
  Envelope envelope_;
  bool terminal_ = false;
  Status status_;
};
}  // namespace connectrpc
#endif  // CONNECTCPP_INCLUDE_CONNECTRPC_ENVELOPE_H_
