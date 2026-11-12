#include "json/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace bjorne_agent {
namespace json {

namespace {

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  Value parse() {
    Value value = parseValue(0);
    skipWhitespace();
    if (pos_ != text_.size()) {
      throw std::runtime_error("Unexpected trailing characters in JSON document");
    }
    return value;
  }

 private:
  std::string text_;
  size_t pos_ = 0;

  char peek() const {
    if (pos_ >= text_.size()) {
      throw std::runtime_error("Unexpected end of JSON document");
    }
    return text_[pos_];
  }

  void skipWhitespace() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  void advanceLiteral(const char* literal) {
    const size_t len = std::string(literal).size();
    if (text_.compare(pos_, len, literal) != 0) {
      throw std::runtime_error("Invalid JSON literal");
    }
    pos_ += len;
  }

  Value parseValue(size_t depth) {
    if (depth > 128) {
      throw std::runtime_error("JSON nesting too deep");
    }
    skipWhitespace();
    const char c = peek();
    switch (c) {
      case '{':
        return parseObject(depth);
      case '[':
        return parseArray(depth);
      case '"': {
        Value value;
        value.type = Value::Type::String;
        value.string = parseString();
        return value;
      }
      case 't': {
        Value value;
        value.type = Value::Type::Boolean;
        value.boolean = true;
        advanceLiteral("true");
        return value;
      }
      case 'f': {
        Value value;
        value.type = Value::Type::Boolean;
        value.boolean = false;
        advanceLiteral("false");
        return value;
      }
      case 'n': {
        Value value;
        value.type = Value::Type::Null;
        advanceLiteral("null");
        return value;
      }
      default:
        if (c == '-' || (c >= '0' && c <= '9')) {
          return parseNumber();
        }
        throw std::runtime_error("Invalid JSON character");
    }
  }

  Value parseObject(size_t depth) {
    ++pos_;
    Value value;
    value.type = Value::Type::Object;
    skipWhitespace();
    if (peek() == '}') {
      ++pos_;
      return value;
    }
    while (true) {
      skipWhitespace();
      if (peek() != '"') {
        throw std::runtime_error("Expected JSON object key");
      }
      const std::string key = parseString();
      skipWhitespace();
      if (peek() != ':') {
        throw std::runtime_error("Expected ':' in JSON object");
      }
      ++pos_;
      value.object[key] = parseValue(depth + 1);
      skipWhitespace();
      const char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == '}') {
        ++pos_;
        break;
      }
      throw std::runtime_error("Expected ',' or '}' in JSON object");
    }
    return value;
  }

  Value parseArray(size_t depth) {
    ++pos_;
    Value value;
    value.type = Value::Type::Array;
    skipWhitespace();
    if (peek() == ']') {
      ++pos_;
      return value;
    }
    while (true) {
      value.array.push_back(parseValue(depth + 1));
      skipWhitespace();
      const char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == ']') {
        ++pos_;
        break;
      }
      throw std::runtime_error("Expected ',' or ']' in JSON array");
    }
    return value;
  }

  // Appends the UTF-8 encoding of a Unicode code point.
  static void appendUtf8(std::string& out, unsigned int code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  // Reads four hex digits at pos_ and advances past them.
  unsigned int readHex4() {
    if (pos_ + 4 > text_.size()) {
      throw std::runtime_error("Truncated JSON unicode escape");
    }
    unsigned int code = 0;
    for (size_t i = 0; i < 4; ++i) {
      const char digit = text_[pos_ + i];
      code <<= 4;
      if (digit >= '0' && digit <= '9') {
        code |= static_cast<unsigned int>(digit - '0');
      } else if (digit >= 'a' && digit <= 'f') {
        code |= static_cast<unsigned int>(digit - 'a' + 10);
      } else if (digit >= 'A' && digit <= 'F') {
        code |= static_cast<unsigned int>(digit - 'A' + 10);
      } else {
        throw std::runtime_error("Invalid JSON unicode escape");
      }
    }
    pos_ += 4;
    return code;
  }

  // Handles \uXXXX with pos_ on the 'u'. A high surrogate must be followed by
  // a low surrogate, which is how models send characters outside the basic
  // multilingual plane, emoji included.
  void parseUnicodeEscape(std::string& out) {
    ++pos_;
    unsigned int code = readHex4();
    if (code >= 0xD800 && code <= 0xDBFF) {
      if (pos_ + 6 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
        throw std::runtime_error("Lone high surrogate in JSON string");
      }
      pos_ += 2;
      const unsigned int low = readHex4();
      if (low < 0xDC00 || low > 0xDFFF) {
        throw std::runtime_error("Invalid low surrogate in JSON string");
      }
      code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
    } else if (code >= 0xDC00 && code <= 0xDFFF) {
      throw std::runtime_error("Lone low surrogate in JSON string");
    }
    appendUtf8(out, code);
  }

  std::string parseString() {
    ++pos_;
    std::string result;
    while (true) {
      if (pos_ >= text_.size()) {
        throw std::runtime_error("Unterminated JSON string");
      }
      const char c = text_[pos_];
      if (c == '"') {
        ++pos_;
        break;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        throw std::runtime_error("Control character in JSON string");
      }
      if (c == '\\') {
        ++pos_;
        const char esc = peek();
        switch (esc) {
          case '"': result.push_back('"'); ++pos_; break;
          case '\\': result.push_back('\\'); ++pos_; break;
          case '/': result.push_back('/'); ++pos_; break;
          case 'b': result.push_back('\b'); ++pos_; break;
          case 'f': result.push_back('\f'); ++pos_; break;
          case 'n': result.push_back('\n'); ++pos_; break;
          case 'r': result.push_back('\r'); ++pos_; break;
          case 't': result.push_back('\t'); ++pos_; break;
          case 'u': {
            parseUnicodeEscape(result);
            break;
          }
          default:
            throw std::runtime_error("Invalid escape in JSON string");
        }
        continue;
      }
      result.push_back(c);
      ++pos_;
    }
    return result;
  }

  Value parseNumber() {
    const size_t start = pos_;
    if (peek() == '-') {
      ++pos_;
    }
    while (pos_ < text_.size() && ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.' ||
                                  text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' || text_[pos_] == '-')) {
      ++pos_;
    }
    const std::string token = text_.substr(start, pos_ - start);
    Value value;
    value.type = Value::Type::Number;
    value.number = std::strtod(token.c_str(), nullptr);
    return value;
  }
};

constexpr size_t kMaxWriteDepth = 128;

void writeString(const std::string& text, std::string& out) {
  static const char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const char c : text) {
    const unsigned char byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (byte < 0x20) {
          out += "\\u00";
          out.push_back(kHex[byte >> 4]);
          out.push_back(kHex[byte & 0x0F]);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
}

void writeNumber(double number, std::string& out) {
  if (!std::isfinite(number)) {
    throw std::runtime_error("Cannot serialize a non-finite JSON number");
  }
  // Print an integral value without a decimal point. The range keeps the
  // conversion to long long well defined; larger values fall through to the
  // general format, which still produces valid JSON.
  if (number == std::floor(number) && std::fabs(number) < 9.0e15) {
    char buffer[32];
    const int written = std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(number));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
      throw std::runtime_error("Cannot format a JSON number");
    }
    out += buffer;
    return;
  }
  char buffer[32];
  // %.17g round-trips every double, so parsing the text returns the same value.
  const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", number);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
    throw std::runtime_error("Cannot format a JSON number");
  }
  out += buffer;
}

void writeValue(const Value& value, std::string& out, size_t depth) {
  if (depth > kMaxWriteDepth) {
    throw std::runtime_error("JSON nesting too deep");
  }
  switch (value.type) {
    case Value::Type::Null:
      out += "null";
      break;
    case Value::Type::Boolean:
      out += value.boolean ? "true" : "false";
      break;
    case Value::Type::Number:
      writeNumber(value.number, out);
      break;
    case Value::Type::String:
      writeString(value.string, out);
      break;
    case Value::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const Value& entry : value.array) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        writeValue(entry, out, depth + 1);
      }
      out.push_back(']');
      break;
    }
    case Value::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& [key, entry] : value.object) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        writeString(key, out);
        out.push_back(':');
        writeValue(entry, out, depth + 1);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

std::string write(const Value& value) {
  std::string out;
  writeValue(value, out, 0);
  return out;
}

Value parse(const std::string& text) {
  Parser parser(text);
  return parser.parse();
}

}  // namespace json
}  // namespace bjorne_agent
