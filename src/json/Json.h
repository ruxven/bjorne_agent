#pragma once

#include <map>
#include <string>
#include <vector>

namespace bjorne_agent {
namespace json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
 public:
  enum class Type { Null, Boolean, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  Array array;
  Object object;

  bool isObject() const { return type == Type::Object; }
  bool isArray() const { return type == Type::Array; }
  bool isString() const { return type == Type::String; }
  bool isNumber() const { return type == Type::Number; }
  bool isBoolean() const { return type == Type::Boolean; }
  bool isNull() const { return type == Type::Null; }

  const Value* find(const std::string& key) const {
    if (type != Type::Object) {
      return nullptr;
    }
    auto it = object.find(key);
    if (it == object.end()) {
      return nullptr;
    }
    return &it->second;
  }

  const Value& operator[](const std::string& key) const {
    return *find(key);
  }
};

Value parse(const std::string& text);

// Serializes a value to compact JSON. Strings are escaped, so the result never
// contains a control character or an unescaped quote. A non-finite number is an
// error, because JSON has no representation for it.
std::string write(const Value& value);

}  // namespace json
}  // namespace bjorne_agent
