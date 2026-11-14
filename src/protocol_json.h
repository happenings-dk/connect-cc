#ifndef CONNECTCPP_SRC_PROTOCOL_JSON_H_
#define CONNECTCPP_SRC_PROTOCOL_JSON_H_
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "connectrpc/status.h"
namespace connectrpc::internal_json {
// A bounded JSON value for protocol errors/metadata. Message JSON remains the
// responsibility of the selected Protobuf runtime. Numbers are retained as
// tokens so unknown fields never lose precision during error forwarding.
struct Value {
  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };
  Kind kind = Kind::kNull;
  std::string text;
  std::vector<Value> array;
  std::map<std::string, Value, std::less<>> object;
  static Value Object() {
    Value v;
    v.kind = Kind::kObject;
    return v;
  }
  static Value Array() {
    Value v;
    v.kind = Kind::kArray;
    return v;
  }
  static Value String(std::string text) {
    Value v;
    v.kind = Kind::kString;
    v.text = std::move(text);
    return v;
  }
};
Result<Value> Parse(std::string_view bytes);
Result<std::string> Encode(const Value& value);
}  // namespace connectrpc::internal_json
#endif
