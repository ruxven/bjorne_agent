#include "jsonrpc/JsonRpc.h"

#include <string>

#include <catch_amalgamated.hpp>

#include "json/Json.h"

using bjorne_agent::json::Value;
using bjorne_agent::jsonrpc::Message;

TEST_CASE("a request encodes with the protocol marker, an id and params") {
  const Value id = bjorne_agent::jsonrpc::numberId(7);
  Value params;
  params.type = Value::Type::Object;
  params.object["question"] = Value();
  params.object["question"].type = Value::Type::String;
  params.object["question"].string = "why?";

  const std::string text = bjorne_agent::jsonrpc::encodeRequest(id, "ask", params);
  REQUIRE(text.find("\"jsonrpc\":\"2.0\"") != std::string::npos);
  REQUIRE(text.find("\"method\":\"ask\"") != std::string::npos);
  REQUIRE(text.find("\"id\":7") != std::string::npos);
  REQUIRE(text.find('\n') == std::string::npos);
}

TEST_CASE("a notification carries no id") {
  const std::string text = bjorne_agent::jsonrpc::encodeNotification("ready", Value());
  REQUIRE(text.find("\"id\"") == std::string::npos);
  REQUIRE(text.find("\"method\":\"ready\"") != std::string::npos);
}

TEST_CASE("a string argument with a newline stays on one line") {
  Value params;
  params.type = Value::Type::Object;
  params.object["text"] = Value();
  params.object["text"].type = Value::Type::String;
  params.object["text"].string = "first\nsecond";

  const std::string text = bjorne_agent::jsonrpc::encodeNotification("note", params);
  REQUIRE(text.find('\n') == std::string::npos);
  REQUIRE(text.find("\\n") != std::string::npos);
}

TEST_CASE("a request decodes into its parts") {
  Message message;
  std::string error;
  REQUIRE(bjorne_agent::jsonrpc::parse(R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"cursor":"c"}})",
                                       message, error));
  REQUIRE(message.kind == Message::Kind::Request);
  REQUIRE(message.hasId);
  REQUIRE(message.method == "tools/list");
  REQUIRE(message.params.find("cursor")->string == "c");
}

TEST_CASE("a notification decodes without an id") {
  Message message;
  std::string error;
  REQUIRE(bjorne_agent::jsonrpc::parse(R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})",
                                       message, error));
  REQUIRE(message.kind == Message::Kind::Notification);
  REQUIRE_FALSE(message.hasId);
  REQUIRE(message.method == "notifications/initialized");
}

TEST_CASE("a result decodes with its id") {
  Message message;
  std::string error;
  REQUIRE(bjorne_agent::jsonrpc::parse(R"({"jsonrpc":"2.0","id":"abc","result":{"ok":true}})", message, error));
  REQUIRE(message.kind == Message::Kind::Response);
  REQUIRE(message.id.string == "abc");
  REQUIRE(message.hasResult);
  REQUIRE_FALSE(message.hasError);
  REQUIRE(message.result.find("ok")->boolean);
}

TEST_CASE("an error response decodes with its code and message") {
  Message message;
  std::string error;
  REQUIRE(bjorne_agent::jsonrpc::parse(
      R"({"jsonrpc":"2.0","id":4,"error":{"code":-32601,"message":"Method not found"}})", message, error));
  REQUIRE(message.kind == Message::Kind::Response);
  REQUIRE(message.hasError);
  REQUIRE(message.error.code == -32601);
  REQUIRE(message.error.message == "Method not found");
}

TEST_CASE("malformed messages are refused with a reason") {
  Message message;
  std::string error;

  REQUIRE_FALSE(bjorne_agent::jsonrpc::parse("not json", message, error));
  REQUIRE(error.find("Invalid JSON") != std::string::npos);

  REQUIRE_FALSE(bjorne_agent::jsonrpc::parse(R"({"id":1,"method":"x"})", message, error));
  REQUIRE(error.find("2.0") != std::string::npos);

  REQUIRE_FALSE(bjorne_agent::jsonrpc::parse(R"({"jsonrpc":"2.0","method":5})", message, error));
  REQUIRE_FALSE(bjorne_agent::jsonrpc::parse(R"({"jsonrpc":"2.0"})", message, error));
  REQUIRE_FALSE(bjorne_agent::jsonrpc::parse(R"({"jsonrpc":"2.0","id":1})", message, error));
  REQUIRE_FALSE(
      bjorne_agent::jsonrpc::parse(R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":1,"message":"x"}})",
                                   message, error));
  REQUIRE_FALSE(bjorne_agent::jsonrpc::parse(R"({"jsonrpc":"2.0","id":1,"error":{"message":"x"}})", message, error));
}

TEST_CASE("an error response encodes with the protocol marker") {
  const Value id = bjorne_agent::jsonrpc::numberId(2);
  const std::string text = bjorne_agent::jsonrpc::encodeError(id, -32601, "Method not found");
  Message message;
  std::string error;
  REQUIRE(bjorne_agent::jsonrpc::parse(text, message, error));
  REQUIRE(message.kind == Message::Kind::Response);
  REQUIRE(message.error.code == -32601);
  REQUIRE(message.error.message == "Method not found");
}
