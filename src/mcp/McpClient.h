#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config/Config.h"
#include "json/Json.h"
#include "jsonrpc/Endpoint.h"
#include "platform/Process.h"
#include "tools/Tools.h"

namespace bjorne_agent {
namespace mcp {

// One tool a server advertises. The schema is passed through untouched, so a
// server can describe any argument shape.
struct ToolInfo {
  std::string name;
  std::string description;
  json::Value input_schema;
};

// The client half of the MCP stdio transport: initialize, tools/list and
// tools/call. It owns no process; the endpoint owns the connection.
class Client {
 public:
  explicit Client(std::shared_ptr<jsonrpc::Endpoint> endpoint);

  // The initialize handshake, ending with the initialized notification.
  bool initialize(std::string& error);

  // Lists every tool, following pagination cursors.
  bool listTools(std::vector<ToolInfo>& tools, std::string& error);

  // Calls a tool. The returned text is the concatenation of the text content
  // items. A tool that reports an error returns true with is_error set and its
  // text, because the server worked: only a transport or protocol failure
  // returns false.
  bool callTool(const std::string& name, const json::Value& arguments, std::string& output, bool& is_error,
                std::string& error);

  bool closed() const;

 private:
  bool fetchToolPage(const std::string& cursor, json::Value& result, std::string& error) const;
  static void appendTools(const json::Value* list, std::vector<ToolInfo>& tools);

  std::shared_ptr<jsonrpc::Endpoint> endpoint_;
};

// The tool name the model sees. The server and tool names are separated from
// the prefix and from each other, so an MCP tool cannot collide with a builtin.
std::string toolName(const std::string& server, const std::string& tool);

// One configured server. It owns the child process and the endpoint, starts
// them lazily, and restarts them once after a crash or a timeout so that a
// dying server degrades to a failed tool call instead of a dead agent.
class Server {
 public:
  Server(std::string name, config::McpServerConfig config);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Starts the process, performs the handshake, and caches tools/list. A
  // failure is returned rather than treated as fatal.
  bool start(std::string& error);

  const std::string& name() const { return name_; }
  const std::vector<ToolInfo>& tools() const { return tools_; }

  // Calls one tool. A crash or timeout triggers one restart and one retry.
  tools::ToolResult call(const std::string& tool, const std::string& arguments);

 private:
  std::string key() const;
  bool launch(std::string& error);
  void shutdown();
  bool initializeConnection(std::string& error);

  std::string name_;
  config::McpServerConfig config_;
  std::shared_ptr<platform::ChildProcess> process_;
  std::shared_ptr<jsonrpc::Endpoint> endpoint_;
  std::shared_ptr<Client> client_;
  std::vector<ToolInfo> tools_;
};

// Starts every configured server and registers its tools under the namespaced
// name. A server that cannot start is logged and skipped; the rest of the run
// continues. Registrations happen after the built-ins and the interaction
// tool, which is the resolution order the tool set promises.
void registerServers(const std::map<std::string, config::McpServerConfig>& servers,
                     tools::ToolRegistry& registry);

}  // namespace mcp
}  // namespace bjorne_agent
