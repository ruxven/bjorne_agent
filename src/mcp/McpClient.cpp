#include "mcp/McpClient.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "json/Json.h"
#include "jsonrpc/JsonRpc.h"
#include "log/Log.h"
#include "platform/Process.h"

namespace bjorne_agent {
namespace mcp {

namespace {

constexpr int kHandshakeTimeoutMs = 30000;
constexpr int kCallTimeoutMs = 60000;
constexpr int kMaxToolPages = 100;
constexpr const char* kProtocolVersion = "2024-11-05";
constexpr const char* kClientVersion = "0.1.0";

json::Value objectValue() {
  json::Value value;
  value.type = json::Value::Type::Object;
  return value;
}

json::Value stringValue(const std::string& text) {
  json::Value value;
  value.type = json::Value::Type::String;
  value.string = text;
  return value;
}

// Adapts the child process pipes to the JSON-RPC transport interface. The
// process knows nothing about JSON; this seam keeps it that way.
class ProcessTransport final : public jsonrpc::Transport {
 public:
  explicit ProcessTransport(std::shared_ptr<platform::ChildProcess> process) : process_(std::move(process)) {}

  bool sendLine(const std::string& line, std::string& error) override {
    return process_->sendLine(line, error);
  }

  jsonrpc::ReadResult readLine(int timeout_ms, std::string& line, std::string& error) override {
    switch (process_->readLine(timeout_ms, line, error)) {
      case platform::ReadStatus::Line:
        return jsonrpc::ReadResult::Line;
      case platform::ReadStatus::Timeout:
        return jsonrpc::ReadResult::Timeout;
      case platform::ReadStatus::Closed:
        return jsonrpc::ReadResult::Closed;
      case platform::ReadStatus::Error:
      default:
        return jsonrpc::ReadResult::Error;
    }
  }

 private:
  std::shared_ptr<platform::ChildProcess> process_;
};

// Renders one content item. Text passes through; any other type becomes a
// short placeholder, because a text only agent cannot display an image.
std::string contentText(const json::Value& item) {
  if (item.isString()) {
    return item.string;
  }
  if (!item.isObject()) {
    return std::string();
  }
  const json::Value* type = item.find("type");
  if (type != nullptr && type->isString() && type->string == "text") {
    const json::Value* text = item.find("text");
    if (text != nullptr && text->isString()) {
      return text->string;
    }
  }
  std::string label = "non-text content";
  if (type != nullptr && type->isString() && !type->string.empty()) {
    label = type->string + " content";
  }
  return "[" + label + " omitted]";
}

}  // namespace

std::string toolName(const std::string& server, const std::string& tool) {
  return "mcp__" + server + "__" + tool;
}

Client::Client(std::shared_ptr<jsonrpc::Endpoint> endpoint) : endpoint_(std::move(endpoint)) {
  // A server may send requests of its own. Ping is the only one this client
  // promises, and anything else gets a method-not-found response rather than
  // a silent drop.
  endpoint_->setRequestHandler([](const std::string& method, const json::Value& params, json::Value& result,
                                  jsonrpc::Error& error) {
    (void)params;
    if (method == "ping") {
      result = objectValue();
      return true;
    }
    error.code = jsonrpc::kMethodNotFound;
    error.message = "Method not found: " + method;
    return false;
  });
}

bool Client::initialize(std::string& error) {
  json::Value params = objectValue();
  params.object["protocolVersion"] = stringValue(kProtocolVersion);
  params.object["capabilities"] = objectValue();
  json::Value client = objectValue();
  client.object["name"] = stringValue("bjorne_agent");
  client.object["version"] = stringValue(kClientVersion);
  params.object["clientInfo"] = client;

  json::Value result;
  if (!endpoint_->request("initialize", params, result, error, kHandshakeTimeoutMs)) {
    return false;
  }
  if (!result.isObject()) {
    error = "The initialize response is not an object";
    return false;
  }
  return endpoint_->notify("notifications/initialized", objectValue(), error);
}

bool Client::fetchToolPage(const std::string& cursor, json::Value& result, std::string& error) const {
  json::Value params = objectValue();
  if (!cursor.empty()) {
    params.object["cursor"] = stringValue(cursor);
  }
  if (!endpoint_->request("tools/list", params, result, error, kHandshakeTimeoutMs)) {
    return false;
  }
  if (!result.isObject()) {
    error = "The tools/list response is not an object";
    return false;
  }
  const json::Value* list = result.find("tools");
  if (list == nullptr || !list->isArray()) {
    error = "The tools/list response has no tools array";
    return false;
  }
  return true;
}

void Client::appendTools(const json::Value* list, std::vector<ToolInfo>& tools) {
  if (list == nullptr || !list->isArray()) {
    return;
  }
  for (const json::Value& entry : list->array) {
    if (!entry.isObject()) {
      continue;
    }
    const json::Value* name = entry.find("name");
    if (name == nullptr || !name->isString() || name->string.empty()) {
      continue;
    }
    ToolInfo tool;
    tool.name = name->string;
    const json::Value* description = entry.find("description");
    if (description != nullptr && description->isString()) {
      tool.description = description->string;
    }
    const json::Value* schema = entry.find("inputSchema");
    if (schema != nullptr && schema->isObject()) {
      tool.input_schema = *schema;
    } else {
      tool.input_schema = objectValue();
    }
    tools.push_back(std::move(tool));
  }
}

bool Client::listTools(std::vector<ToolInfo>& tools, std::string& error) {
  tools.clear();
  std::string cursor;
  for (int page = 0; page < kMaxToolPages; ++page) {
    json::Value result;
    if (!fetchToolPage(cursor, result, error)) {
      return false;
    }
    appendTools(result.find("tools"), tools);

    const json::Value* next = result.find("nextCursor");
    if (next == nullptr || !next->isString() || next->string.empty()) {
      return true;
    }
    cursor = next->string;
  }
  error = "The server returned more than " + std::to_string(kMaxToolPages) + " pages of tools";
  return false;
}

bool Client::callTool(const std::string& name, const json::Value& arguments, std::string& output, bool& is_error,
                      std::string& error) {
  json::Value params = objectValue();
  params.object["name"] = stringValue(name);
  params.object["arguments"] = arguments;

  json::Value result;
  if (!endpoint_->request("tools/call", params, result, error, kCallTimeoutMs)) {
    return false;
  }
  if (!result.isObject()) {
    error = "The tools/call response is not an object";
    return false;
  }

  output.clear();
  const json::Value* content = result.find("content");
  if (content != nullptr && content->isArray()) {
    bool first = true;
    for (const json::Value& item : content->array) {
      const std::string text = contentText(item);
      if (text.empty()) {
        continue;
      }
      if (!first) {
        output.push_back('\n');
      }
      output.append(text);
      first = false;
    }
  }

  is_error = false;
  const json::Value* flag = result.find("isError");
  if (flag != nullptr && flag->isBoolean()) {
    is_error = flag->boolean;
  }
  return true;
}

bool Client::closed() const { return endpoint_->closed(); }

Server::Server(std::string name, config::McpServerConfig config)
    : name_(std::move(name)), config_(std::move(config)) {}

Server::~Server() { shutdown(); }

std::string Server::key() const { return name_; }

bool Server::start(std::string& error) {
  if (!launch(error)) {
    return false;
  }
  return initializeConnection(error);
}

bool Server::launch(std::string& error) {
  process_ = std::make_shared<platform::ChildProcess>();
  if (!process_->start(config_.command, config_.args, config_.env, error)) {
    process_.reset();
    return false;
  }
  endpoint_ = std::make_shared<jsonrpc::Endpoint>(std::make_shared<ProcessTransport>(process_));
  client_ = std::make_shared<Client>(endpoint_);
  endpoint_->start();
  return true;
}

bool Server::initializeConnection(std::string& error) {
  if (!client_->initialize(error)) {
    return false;
  }
  return client_->listTools(tools_, error);
}

void Server::shutdown() {
  // The endpoint joins its reader thread before the process is stopped, so the
  // reader never touches a closed pipe.
  endpoint_.reset();
  client_.reset();
  if (process_) {
    process_->stop();
    process_.reset();
  }
  tools_.clear();
}

tools::ToolResult Server::call(const std::string& tool, const std::string& arguments) {
  json::Value parsed;
  if (!tools::parseToolArguments(arguments, parsed)) {
    return tools::ToolResult{false, "The tool arguments are not a JSON object"};
  }

  const auto attempt = [this, &tool, &parsed]() {
    std::string output;
    bool is_error = false;
    std::string error;
    if (!client_ || client_->closed()) {
      return tools::ToolResult{false, "The MCP server " + name_ + " is not running"};
    }
    if (!client_->callTool(tool, parsed, output, is_error, error)) {
      return tools::ToolResult{false, error};
    }
    if (is_error) {
      return tools::ToolResult{false, output.empty() ? "The tool reported an error" : output};
    }
    return tools::ToolResult{true, output};
  };

  tools::ToolResult result = attempt();
  if (result.ok) {
    return result;
  }

  // A transport failure may mean the server died. One restart and one retry
  // keeps a crash to a lost call rather than a lost run. A tool that merely
  // reported an error is not retried, because the server itself worked.
  const bool may_restart = !client_ || client_->closed() || result.output.find("timed out") != std::string::npos;
  if (!may_restart) {
    return result;
  }

  log::fail("MCP server " + key() + " failed; restarting it");
  shutdown();
  std::string error;
  if (!launch(error) || !initializeConnection(error)) {
    shutdown();
    return tools::ToolResult{false, "MCP server " + key() + " could not be restarted: " + error};
  }
  return attempt();
}

void registerServers(const std::map<std::string, config::McpServerConfig>& servers,
                     tools::ToolRegistry& registry) {
  for (const auto& [name, config] : servers) {
    if (config.command.empty()) {
      log::fail("MCP server " + name + " has no command");
      continue;
    }
    auto server = std::make_shared<Server>(name, config);
    std::string error;
    if (!server->start(error)) {
      log::fail("MCP server " + name + " is unavailable: " + error);
      continue;
    }
    log::detail("MCP server " + name + ": " + std::to_string(server->tools().size()) + " tool(s)");
    for (const ToolInfo& tool : server->tools()) {
      const std::string exposed = toolName(name, tool.name);
      tools::ToolDefinition definition;
      definition.name = exposed;
      definition.description = tool.description;
      definition.parameters = tool.input_schema;
      if (!definition.parameters.isObject()) {
        definition.parameters = objectValue();
      }
      registry.registerTool(definition, [server, tool_name = tool.name](const std::string& arguments) {
        return server->call(tool_name, arguments);
      });
      log::detail("MCP tool: " + exposed);
    }
  }
}

}  // namespace mcp
}  // namespace bjorne_agent
