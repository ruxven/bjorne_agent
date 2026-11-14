#include "jsonrpc/JsonRpc.h"

#include <exception>
#include <string>

#include "json/Json.h"

namespace bjorne_agent {
namespace jsonrpc {

namespace {

json::Value stringValue(const std::string& text) {
  json::Value value;
  value.type = json::Value::Type::String;
  value.string = text;
  return value;
}

json::Value numberValue(double number) {
  json::Value value;
  value.type = json::Value::Type::Number;
  value.number = number;
  return value;
}

json::Value objectValue() {
  json::Value value;
  value.type = json::Value::Type::Object;
  return value;
}

// Every message starts with the protocol marker. Keeping the assembly here
// means a new message kind cannot forget it.
json::Value envelope() {
  json::Value value = objectValue();
  value.object["jsonrpc"] = stringValue("2.0");
  return value;
}

bool readId(const json::Value& source, json::Value& id, std::string& error) {
  const json::Value* value = source.find("id");
  if (value == nullptr) {
    return true;
  }
  if (!value->isString() && !value->isNumber()) {
    error = "The id is neither a string nor a number";
    return false;
  }
  id = *value;
  return true;
}

bool parseParams(const json::Value& source, Message& out, std::string& error) {
  const json::Value* params = source.find("params");
  if (params == nullptr) {
    return true;
  }
  if (!params->isObject() && !params->isArray() && !params->isNull()) {
    error = "params is not an object, an array, or null";
    return false;
  }
  out.params = *params;
  return true;
}

bool parseError(const json::Value& source, Message& out, std::string& error) {
  const json::Value* value = source.find("error");
  if (value == nullptr) {
    return true;
  }
  if (!value->isObject()) {
    error = "error is not an object";
    return false;
  }
  const json::Value* code = value->find("code");
  const json::Value* message = value->find("message");
  if (code == nullptr || !code->isNumber() || message == nullptr || !message->isString()) {
    error = "error does not carry a numeric code and a message";
    return false;
  }
  out.hasError = true;
  out.error.code = static_cast<int>(code->number);
  out.error.message = message->string;
  return true;
}

// A request or a notification: the message names a method.
bool parseCall(const json::Value& source, Message& out, std::string& error) {
  const json::Value* method = source.find("method");
  if (!method->isString() || method->string.empty()) {
    error = "method is not a non-empty string";
    return false;
  }
  out.method = method->string;
  if (!parseParams(source, out, error)) {
    return false;
  }
  if (!readId(source, out.id, error)) {
    return false;
  }
  out.hasId = out.id.type != json::Value::Type::Null;
  out.kind = out.hasId ? Message::Kind::Request : Message::Kind::Notification;
  return true;
}

// A response: the message carries an id and exactly one of result or error.
bool parseResponse(const json::Value& source, Message& out, std::string& error) {
  if (source.find("id") == nullptr) {
    error = "The message has neither a method nor an id";
    return false;
  }
  if (!readId(source, out.id, error)) {
    return false;
  }
  if (out.id.type == json::Value::Type::Null) {
    error = "A response carries a non-null id";
    return false;
  }
  out.hasId = true;

  const json::Value* result = source.find("result");
  if (result != nullptr) {
    out.result = *result;
    out.hasResult = true;
  }
  if (!parseError(source, out, error)) {
    return false;
  }
  if (out.hasResult == out.hasError) {
    error = "A response carries exactly one of result or error";
    return false;
  }
  out.kind = Message::Kind::Response;
  return true;
}

}  // namespace

json::Value numberId(long long value) {
  return numberValue(static_cast<double>(value));
}

json::Value stringId(const std::string& value) {
  return stringValue(value);
}

std::string encodeRequest(const json::Value& id, const std::string& method, const json::Value& params) {
  json::Value message = envelope();
  message.object["id"] = id;
  message.object["method"] = stringValue(method);
  message.object["params"] = params;
  return json::write(message);
}

std::string encodeNotification(const std::string& method, const json::Value& params) {
  json::Value message = envelope();
  message.object["method"] = stringValue(method);
  message.object["params"] = params;
  return json::write(message);
}

std::string encodeResult(const json::Value& id, const json::Value& result) {
  json::Value message = envelope();
  message.object["id"] = id;
  message.object["result"] = result;
  return json::write(message);
}

std::string encodeError(const json::Value& id, int code, const std::string& message) {
  json::Value error = objectValue();
  error.object["code"] = numberValue(static_cast<double>(code));
  error.object["message"] = stringValue(message);

  json::Value reply = envelope();
  reply.object["id"] = id;
  reply.object["error"] = error;
  return json::write(reply);
}

bool parse(const std::string& text, Message& out, std::string& error) {
  json::Value root;
  try {
    root = json::parse(text);
  } catch (const std::exception& exception) {
    error = std::string("Invalid JSON: ") + exception.what();
    return false;
  }
  if (!root.isObject()) {
    error = "The message is not a JSON object";
    return false;
  }
  const json::Value* version = root.find("jsonrpc");
  if (version == nullptr || !version->isString() || version->string != "2.0") {
    error = "The message does not declare jsonrpc 2.0";
    return false;
  }
  if (root.find("method") != nullptr) {
    return parseCall(root, out, error);
  }
  return parseResponse(root, out, error);
}

}  // namespace jsonrpc
}  // namespace bjorne_agent
