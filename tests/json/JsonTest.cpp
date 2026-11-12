#include "json/Json.h"

#include <cmath>
#include <limits>
#include <string>

#include <catch_amalgamated.hpp>

using bjorne_agent::json::Value;
using bjorne_agent::json::parse;
using bjorne_agent::json::write;

TEST_CASE("JSON parser parses flat objects") {
  const Value root = bjorne_agent::json::parse("{\"version\":1,\"name\":\"bjorne_agent\"}");
  REQUIRE(root.isObject());
  const Value* version = root.find("version");
  REQUIRE(version != nullptr);
  REQUIRE(version->isNumber());
  REQUIRE(version->number == 1.0);
  const Value* name = root.find("name");
  REQUIRE(name != nullptr);
  REQUIRE(name->isString());
  REQUIRE(name->string == "bjorne_agent");
}

TEST_CASE("JSON parser parses nested objects and arrays") {
  const Value root = bjorne_agent::json::parse(
      "{\"agent\":{\"system_prompt\":\"Be brief\"},\"models\":[{\"provider\":\"openai\"}]}");
  REQUIRE(root.isObject());
  const Value* agent = root.find("agent");
  REQUIRE(agent != nullptr);
  REQUIRE(agent->isObject());
  const Value* systemPrompt = agent->find("system_prompt");
  REQUIRE(systemPrompt != nullptr);
  REQUIRE(systemPrompt->isString());
  REQUIRE(systemPrompt->string == "Be brief");

  const Value* models = root.find("models");
  REQUIRE(models != nullptr);
  REQUIRE(models->isArray());
  REQUIRE(models->array.size() == 1);
  REQUIRE(models->array.front().isObject());
  REQUIRE(models->array.front().find("provider") != nullptr);
}

TEST_CASE("JSON parser handles escaped strings") {
  const Value root = bjorne_agent::json::parse("{\"key\":\"a\\\"b\\\\c\\nd\\te\"}");
  REQUIRE(root.isObject());
  const Value* key = root.find("key");
  REQUIRE(key != nullptr);
  REQUIRE(key->isString());
  REQUIRE(key->string == "a\"b\\c\nd\te");
}

TEST_CASE("JSON parser handles unicode escapes") {
  const Value root = bjorne_agent::json::parse("{\"key\":\"A\\u0041\\u00e9\"}");
  REQUIRE(root.isObject());
  const Value* key = root.find("key");
  REQUIRE(key != nullptr);
  REQUIRE(key->isString());
  REQUIRE(key->string == "AAé");
}

TEST_CASE("JSON parser handles numbers and literals") {
  const Value number = bjorne_agent::json::parse("3.5");
  REQUIRE(number.isNumber());
  REQUIRE(number.number == 3.5);

  const Value integer = bjorne_agent::json::parse("42");
  REQUIRE(integer.isNumber());
  REQUIRE(integer.number == 42.0);

  const Value truthy = bjorne_agent::json::parse("true");
  REQUIRE(truthy.isBoolean());
  REQUIRE(truthy.boolean);

  const Value falsy = bjorne_agent::json::parse("false");
  REQUIRE(falsy.isBoolean());
  REQUIRE_FALSE(falsy.boolean);

  const Value nothing = bjorne_agent::json::parse("null");
  REQUIRE(nothing.isNull());
}

TEST_CASE("JSON parser handles every simple escape") {
  const Value root = bjorne_agent::json::parse(R"({"key":"slash\/back\bform\fcarriage\rtab\t"})");
  const Value* key = root.find("key");
  REQUIRE(key != nullptr);
  REQUIRE(key->isString());
  REQUIRE(key->string == std::string("slash/back\bform\fcarriage\rtab\t"));
}

TEST_CASE("JSON parser decodes multi byte unicode escapes") {
  const Value root = bjorne_agent::json::parse(R"({"key":"\u20ac"})");
  const Value* key = root.find("key");
  REQUIRE(key != nullptr);
  REQUIRE(key->isString());
  REQUIRE(key->string == "\xE2\x82\xAC");
}

TEST_CASE("JSON parser accepts uppercase hex digits in unicode escapes") {
  // RFC 8259 allows either case for the hex digits of a \u escape.
  const Value root = bjorne_agent::json::parse(R"({"key":"\u00C9\u0041\uD83D\uDE00"})");
  const Value* key = root.find("key");
  REQUIRE(key != nullptr);
  REQUIRE(key->isString());
  // É, A, and the emoji that the surrogate pair spells out.
  REQUIRE(key->string == "\xC3\x89"
                        "A"
                        "\xF0\x9F\x98\x80");
}

TEST_CASE("JSON parser parses negative and exponent numbers") {
  REQUIRE(bjorne_agent::json::parse("-42").number == -42.0);
  REQUIRE(bjorne_agent::json::parse("-3.5e2").number == Catch::Approx(-350.0));
  REQUIRE(bjorne_agent::json::parse("1e-3").number == Catch::Approx(0.001));
}

TEST_CASE("JSON parser parses empty containers") {
  const Value object = bjorne_agent::json::parse("{}");
  REQUIRE(object.isObject());
  REQUIRE(object.object.empty());

  const Value array = bjorne_agent::json::parse("[]");
  REQUIRE(array.isArray());
  REQUIRE(array.array.empty());
}

TEST_CASE("JSON parser rejects bad string content") {
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("{\"key\":\"abc"), "Unterminated JSON string");
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("{\"key\":\"a\nb\"}"), "Control character in JSON string");
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse(R"({"key":"\q"})"), "Invalid escape in JSON string");
  REQUIRE_THROWS(bjorne_agent::json::parse(R"({"key":"\uZZZZ"})"));
}

TEST_CASE("JSON parser rejects bad separators") {
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("{\"a\":1 \"b\":2}"), "Expected ',' or '}' in JSON object");
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("{\"a\" 1}"), "Expected ':' in JSON object");
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("[1 2]"), "Expected ',' or ']' in JSON array");
}

TEST_CASE("JSON parser rejects deeply nested documents") {
  std::string text;
  for (int i = 0; i < 130; ++i) {
    text += "[";
  }
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse(text), "JSON nesting too deep");
}

TEST_CASE("JSON parser rejects malformed documents") {
  REQUIRE_THROWS(bjorne_agent::json::parse("{"));
  REQUIRE_THROWS(bjorne_agent::json::parse(""));
  REQUIRE_THROWS(bjorne_agent::json::parse("[1,]"));
  REQUIRE_THROWS(bjorne_agent::json::parse("{bad}"));
  REQUIRE_THROWS(bjorne_agent::json::parse("{\"key\":}"));
  REQUIRE_THROWS(bjorne_agent::json::parse("[1] trailing"));
}

TEST_CASE("JSON parser combines surrogate pairs into UTF-8") {
  // U+1F600 GRINNING FACE arrives from OpenAI compatible models as a pair.
  const Value root = bjorne_agent::json::parse("{\"key\":\"\\ud83d\\ude00\"}");
  REQUIRE(root.isObject());
  const Value* key = root.find("key");
  REQUIRE(key != nullptr);
  REQUIRE(key->isString());
  REQUIRE(key->string == "\xF0\x9F\x98\x80");

  const Value plain = bjorne_agent::json::parse("{\"key\":\"\\u4e2d\"}");
  REQUIRE(plain.find("key")->string == "\xE4\xB8\xAD");
}

TEST_CASE("JSON parser rejects malformed unicode escapes") {
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("\"\\ud83d\""), "Lone high surrogate in JSON string");
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("\"\\ude00\""), "Lone low surrogate in JSON string");
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("\"\\ud83d\\u0041\""), "Invalid low surrogate in JSON string");
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("\"\\u12\""), "Truncated JSON unicode escape");
  REQUIRE_THROWS_WITH(bjorne_agent::json::parse("\"\\u12zz\""), "Invalid JSON unicode escape");
}

TEST_CASE("JSON writer serializes scalars") {
  const Value nothing;
  REQUIRE(write(nothing) == "null");

  Value truthy;
  truthy.type = Value::Type::Boolean;
  truthy.boolean = true;
  REQUIRE(write(truthy) == "true");
  truthy.boolean = false;
  REQUIRE(write(truthy) == "false");

  Value number;
  number.type = Value::Type::Number;
  number.number = 42.0;
  REQUIRE(write(number) == "42");
  number.number = -7.0;
  REQUIRE(write(number) == "-7");
  number.number = 3.5;
  REQUIRE(write(number) == "3.5");
  number.number = -0.25;
  REQUIRE(write(number) == "-0.25");
}

TEST_CASE("JSON writer escapes strings and keys") {
  Value text;
  text.type = Value::Type::String;
  text.string = "a\"b\\c\nd\te";
  text.string.push_back('\x01');
  REQUIRE(write(text) == "\"a\\\"b\\\\c\\nd\\te\\u0001\"");

  Value object;
  object.type = Value::Type::Object;
  Value value;
  value.type = Value::Type::String;
  value.string = "v";
  object.object["a\nb"] = value;
  REQUIRE(write(object) == "{\"a\\nb\":\"v\"}");
}

TEST_CASE("JSON writer serializes containers in key order") {
  Value array;
  array.type = Value::Type::Array;
  Value first;
  first.type = Value::Type::Number;
  first.number = 1;
  Value second;
  second.type = Value::Type::String;
  second.string = "two";
  array.array.push_back(first);
  array.array.push_back(second);
  array.array.push_back(Value{});
  REQUIRE(write(array) == "[1,\"two\",null]");

  Value object;
  object.type = Value::Type::Object;
  object.object["b"] = first;
  object.object["a"] = second;
  REQUIRE(write(object) == "{\"a\":\"two\",\"b\":1}");

  REQUIRE(write(parse("{}")) == "{}");
  REQUIRE(write(parse("[]")) == "[]");
}

TEST_CASE("JSON writer round trips a parsed document") {
  const std::string text =
      R"({"agent":{"name":"bjorne_agent","tags":["a",1,2.5,true,null]},"empty":{},"list":[]})";
  const std::string first = write(parse(text));
  REQUIRE(write(parse(first)) == first);
  REQUIRE(parse(first).find("agent") != nullptr);
}

TEST_CASE("JSON writer rejects non finite numbers") {
  Value number;
  number.type = Value::Type::Number;
  number.number = std::numeric_limits<double>::infinity();
  REQUIRE_THROWS_WITH(write(number), "Cannot serialize a non-finite JSON number");
  number.number = std::numeric_limits<double>::quiet_NaN();
  REQUIRE_THROWS_WITH(write(number), "Cannot serialize a non-finite JSON number");
}

TEST_CASE("JSON writer rejects deeply nested values") {
  Value root;
  root.type = Value::Type::Array;
  Value* current = &root;
  for (int i = 0; i < 130; ++i) {
    Value child;
    child.type = Value::Type::Array;
    current->array.push_back(child);
    current = &current->array.back();
  }
  REQUIRE_THROWS_WITH(write(root), "JSON nesting too deep");
}
