// A stub MCP server for the tests. It speaks the newline delimited JSON-RPC
// dialect of the stdio transport: initialize, tools/list and tools/call. The
// environment variable BJORNE_AGENT_MCP_STUB_EXIT_AFTER makes it exit after
// that many tool calls, which is how the tests exercise the restart path.

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include "json/Json.h"

namespace {

void send(const bjorne_agent::json::Value& message) {
  std::cout << bjorne_agent::json::write(message) << "\n";
  std::cout.flush();
}

bjorne_agent::json::Value objectValue() {
  bjorne_agent::json::Value value;
  value.type = bjorne_agent::json::Value::Type::Object;
  return value;
}

bjorne_agent::json::Value stringValue(const std::string& text) {
  bjorne_agent::json::Value value;
  value.type = bjorne_agent::json::Value::Type::String;
  value.string = text;
  return value;
}

bjorne_agent::json::Value numberValue(double number) {
  bjorne_agent::json::Value value;
  value.type = bjorne_agent::json::Value::Type::Number;
  value.number = number;
  return value;
}

}  // namespace

int main() {
  const char* exitAfterText = std::getenv("BJORNE_AGENT_MCP_STUB_EXIT_AFTER");
  const int exitAfter = exitAfterText != nullptr ? std::atoi(exitAfterText) : 0;
  int calls = 0;

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    bjorne_agent::json::Value request = bjorne_agent::json::parse(line);
    const bjorne_agent::json::Value* method = request.find("method");
    const bjorne_agent::json::Value* id = request.find("id");
    if (method == nullptr || !method->isString()) {
      continue;
    }
    if (id == nullptr) {
      continue;
    }

    bjorne_agent::json::Value reply = objectValue();
    reply.object["jsonrpc"] = stringValue("2.0");
    reply.object["id"] = *id;

    if (method->string == "initialize") {
      bjorne_agent::json::Value result = objectValue();
      result.object["protocolVersion"] = stringValue("2024-11-05");
      result.object["capabilities"] = objectValue();
      reply.object["result"] = result;
    } else if (method->string == "tools/list") {
      bjorne_agent::json::Value tool = objectValue();
      tool.object["name"] = stringValue("echo");
      tool.object["description"] = stringValue("Echoes the text back.");
      bjorne_agent::json::Value schema = objectValue();
      schema.object["type"] = stringValue("object");
      bjorne_agent::json::Value properties = objectValue();
      bjorne_agent::json::Value text = objectValue();
      text.object["type"] = stringValue("string");
      properties.object["text"] = text;
      schema.object["properties"] = properties;
      tool.object["inputSchema"] = schema;

      bjorne_agent::json::Value tools = bjorne_agent::json::Value();
      tools.type = bjorne_agent::json::Value::Type::Array;
      tools.array.push_back(tool);
      bjorne_agent::json::Value result = objectValue();
      result.object["tools"] = tools;
      reply.object["result"] = result;
    } else if (method->string == "tools/call") {
      ++calls;
      bjorne_agent::json::Value text = objectValue();
      text.object["type"] = stringValue("text");
      text.object["text"] = stringValue("echo:" + std::to_string(calls));
      bjorne_agent::json::Value content = bjorne_agent::json::Value();
      content.type = bjorne_agent::json::Value::Type::Array;
      content.array.push_back(text);
      bjorne_agent::json::Value result = objectValue();
      result.object["content"] = content;
      reply.object["result"] = result;
    } else {
      bjorne_agent::json::Value error = objectValue();
      error.object["code"] = numberValue(-32601);
      error.object["message"] = stringValue("Method not found");
      reply.object["error"] = error;
    }
    send(reply);

    if (exitAfter > 0 && calls >= exitAfter) {
      return 3;
    }
  }
  return 0;
}
