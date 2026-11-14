#include "protocol_json.h"

#include <cstdint>

namespace connectrpc::internal_json {
namespace {
Status Invalid() {
  return {Code::kInvalidArgument, "invalid protocol JSON", {}};
}
bool Utf8(std::string_view bytes) {
  for (size_t i = 0; i < bytes.size();) {
    auto c = static_cast<unsigned char>(bytes[i++]);
    if (c < 0x80) continue;
    int remaining;
    uint32_t point, minimum;
    if (c >= 0xc2 && c <= 0xdf) {
      remaining = 1;
      point = c & 0x1f;
      minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      remaining = 2;
      point = c & 0xf;
      minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      remaining = 3;
      point = c & 7;
      minimum = 0x10000;
    } else
      return false;
    while (remaining--) {
      if (i == bytes.size()) return false;
      c = static_cast<unsigned char>(bytes[i++]);
      if ((c & 0xc0) != 0x80) return false;
      point = (point << 6) | (c & 0x3f);
    }
    if (point < minimum || point > 0x10ffff ||
        (point >= 0xd800 && point <= 0xdfff))
      return false;
  }
  return true;
}
void AppendUtf8(uint32_t point, std::string& out) {
  if (point < 0x80)
    out.push_back(static_cast<char>(point));
  else {
    if (point >= 0x10000)
      out.push_back(static_cast<char>(0xf0 | (point >> 18)));
    if (point >= 0x800)
      out.push_back(static_cast<char>((point >= 0x10000 ? 0x80 : 0xe0) |
                                      ((point >> 12) & 0x3f)));
    out.push_back(static_cast<char>((point >= 0x800 ? 0x80 : 0xc0) |
                                    ((point >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
  }
}
class Parser {
 public:
  explicit Parser(std::string_view input) : input_(input) {}
  bool Parse(Value& value, unsigned depth = 0) {
    Space();
    if (depth > 64 || ++nodes_ > 65536 || input_.empty()) return false;
    if (input_.front() == '"') {
      value.kind = Value::Kind::kString;
      return String(value.text);
    }
    if (Take('{')) {
      value = Value::Object();
      Space();
      if (Take('}')) return true;
      do {
        std::string name;
        Space();
        if (!String(name)) return false;
        Space();
        if (!Take(':')) return false;
        Value item;
        if (!Parse(item, depth + 1) ||
            !value.object.emplace(std::move(name), std::move(item)).second)
          return false;
        Space();
        if (Take('}')) return true;
      } while (Take(','));
      return false;
    }
    if (Take('[')) {
      value = Value::Array();
      Space();
      if (Take(']')) return true;
      do {
        Value item;
        if (!Parse(item, depth + 1)) return false;
        value.array.push_back(std::move(item));
        Space();
        if (Take(']')) return true;
      } while (Take(','));
      return false;
    }
    for (auto word : {"null", "true", "false"}) {
      if (input_.starts_with(word)) {
        input_.remove_prefix(std::char_traits<char>::length(word));
        value.kind = word[0] == 'n' ? Value::Kind::kNull : Value::Kind::kBool;
        value.text = word;
        return true;
      }
    }
    auto start = input_;
    Take('-');
    if (!Take('0')) {
      if (input_.empty() || input_.front() < '1' || input_.front() > '9')
        return false;
      Digits();
    }
    if (Take('.')) {
      if (!Digit()) return false;
      Digits();
    }
    if (Take('e') || Take('E')) {
      if (!Take('+')) Take('-');
      if (!Digit()) return false;
      Digits();
    }
    value.kind = Value::Kind::kNumber;
    value.text = start.substr(0, start.size() - input_.size());
    return true;
  }
  bool Done() {
    Space();
    return input_.empty();
  }

 private:
  bool Take(char c) {
    if (input_.empty() || input_.front() != c) return false;
    input_.remove_prefix(1);
    return true;
  }
  bool Digit() const {
    return !input_.empty() && input_.front() >= '0' && input_.front() <= '9';
  }
  void Digits() {
    while (Digit()) input_.remove_prefix(1);
  }
  void Space() {
    while (!input_.empty() &&
           (input_.front() == ' ' || input_.front() == '\n' ||
            input_.front() == '\r' || input_.front() == '\t'))
      input_.remove_prefix(1);
  }
  bool Hex(uint32_t& point) {
    point = 0;
    for (int i = 0; i < 4; ++i) {
      if (input_.empty()) return false;
      char c = input_.front();
      input_.remove_prefix(1);
      unsigned n;
      if (c >= '0' && c <= '9')
        n = c - '0';
      else if (c >= 'a' && c <= 'f')
        n = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        n = c - 'A' + 10;
      else
        return false;
      point = (point << 4) | n;
    }
    return true;
  }
  bool String(std::string& out) {
    if (!Take('"')) return false;
    while (!input_.empty()) {
      char c = input_.front();
      input_.remove_prefix(1);
      if (c == '"') return Utf8(out);
      if (static_cast<unsigned char>(c) < 32) return false;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (input_.empty()) return false;
      c = input_.front();
      input_.remove_prefix(1);
      switch (c) {
        case '"':
        case '\\':
        case '/':
          out.push_back(c);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          uint32_t point;
          if (!Hex(point)) return false;
          if (point >= 0xd800 && point <= 0xdbff) {
            uint32_t low;
            if (!Take('\\') || !Take('u') || !Hex(low) || low < 0xdc00 ||
                low > 0xdfff)
              return false;
            point = 0x10000 + ((point - 0xd800) << 10) + low - 0xdc00;
          } else if (point >= 0xdc00 && point <= 0xdfff)
            return false;
          AppendUtf8(point, out);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }
  std::string_view input_;
  size_t nodes_ = 0;
};
bool Quote(std::string_view text, std::string& out) {
  if (!Utf8(text)) return false;
  constexpr char hex[] = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 32) {
      out += "\\u00";
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    } else
      out.push_back(static_cast<char>(c));
  }
  out.push_back('"');
  return true;
}
bool Write(const Value& value, std::string& out, unsigned depth) {
  if (depth > 64) return false;
  switch (value.kind) {
    case Value::Kind::kNull:
      out += "null";
      return true;
    case Value::Kind::kString:
      return Quote(value.text, out);
    case Value::Kind::kNumber:
    case Value::Kind::kBool:
      out += value.text;
      return true;
    case Value::Kind::kArray: {
      out.push_back('[');
      bool first = true;
      for (const auto& item : value.array) {
        if (!first) out.push_back(',');
        first = false;
        if (!Write(item, out, depth + 1)) return false;
      }
      out.push_back(']');
      return true;
    }
    case Value::Kind::kObject: {
      out.push_back('{');
      bool first = true;
      for (const auto& [name, item] : value.object) {
        if (!first) out.push_back(',');
        first = false;
        if (!Quote(name, out)) return false;
        out.push_back(':');
        if (!Write(item, out, depth + 1)) return false;
      }
      out.push_back('}');
      return true;
    }
  }
  return false;
}
}  // namespace
Result<Value> Parse(std::string_view bytes) {
  Value value;
  Parser parser(bytes);
  if (!parser.Parse(value) || !parser.Done()) return Result<Value>(Invalid());
  return Result<Value>(std::move(value));
}
Result<std::string> Encode(const Value& value) {
  std::string bytes;
  if (!Write(value, bytes, 0)) return Result<std::string>(Invalid());
  return Result<std::string>(std::move(bytes));
}
}  // namespace connectrpc::internal_json
