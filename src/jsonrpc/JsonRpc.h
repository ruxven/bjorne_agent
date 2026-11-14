#pragma once

#include <string>

#include "json/Json.h"

namespace bjorne_agent {
namespace jsonrpc {

// The error codes JSON-RPC 2.0 defines, plus the one used when a peer's request
// cannot be answered because no handler is registered for it.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

// The error member of a failed response.
struct Error {
  int code = 0;
  std::string message;
};

// One decoded JSON-RPC message. A request carries a method and an id, a
// notification carries a method and no id, and a response carries an id plus
// either a result or an error.
struct Message {
  enum class Kind { Request, Notification, Response, Invalid };

  Kind kind = Kind::Invalid;
  json::Value id;
  bool hasId = false;
  std::string method;
  json::Value params;
  json::Value result;
  bool hasResult = false;
  Error error;
  bool hasError = false;
};

// Builds the identifier a request or response carries. Numbers are used by the
// MCP client; strings exist because a JSON-RPC peer may choose either.
json::Value numberId(long long value);
json::Value stringId(const std::string& value);

// Encodes one message as compact JSON. The writer escapes newlines, so the
// result is exactly one line and the newline framing stays intact.
std::string encodeRequest(const json::Value& id, const std::string& method, const json::Value& params);
std::string encodeNotification(const std::string& method, const json::Value& params);
std::string encodeResult(const json::Value& id, const json::Value& result);
std::string encodeError(const json::Value& id, int code, const std::string& message);

// Decodes one line. Returns false with a reason when the text is not a JSON-RPC
// 2.0 message.
bool parse(const std::string& text, Message& out, std::string& error);

}  // namespace jsonrpc
}  // namespace bjorne_agent
