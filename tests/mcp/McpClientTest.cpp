#include "mcp/McpClient.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

#include "json/Json.h"
#include "jsonrpc/Endpoint.h"

using bjorne_agent::json::Value;
using bjorne_agent::jsonrpc::Endpoint;
using bjorne_agent::jsonrpc::ReadResult;
using bjorne_agent::jsonrpc::Transport;

namespace {

// A transport that answers like an MCP server: it reads the request the
// endpoint wrote and replies with the scripted answer for that method. The
// scripted answers are the result bodies, so a test can shape them without
// repeating the envelope.
class ScriptedServer final : public Transport {
 public:
  bool sendLine(const std::string& line, std::string& error) override {
    (void)error;
    Value request = bjorne_agent::json::parse(line);
    const Value* method = request.find("method");
    const Value* id = request.find("id");
    const std::string name = method != nullptr && method->isString() ? method->string : std::string();

    if (method == nullptr) {
      // A response to a request the client made. The server keeps it, so the
      // test can look at what the client answered.
      std::lock_guard<std::mutex> guard(mutex_);
      sent.push_back(line);
      return true;
    }

    if (id == nullptr) {
      std::lock_guard<std::mutex> guard(mutex_);
      notifications.push_back(name);
      return true;
    }

    const std::string identifier = std::to_string(static_cast<int>(id->number));
    std::string body;
    if (name == "initialize") {
      body = initialize_body.empty()
                 ? R"({"protocolVersion":"2024-11-05","capabilities":{},"serverInfo":{"name":"fake"}})"
                 : initialize_body;
    } else if (name == "tools/list") {
      body = listBody();
    } else if (name == "tools/call") {
      body = callBody();
    } else {
      const std::string reply = R"({"jsonrpc":"2.0","id":)" + identifier +
                               R"(,"error":{"code":-32601,"message":"Method not found"}})";
      push(reply);
      return true;
    }

    push(R"({"jsonrpc":"2.0","id":)" + identifier + R"(,"result":)" + body + "}");
    return true;
  }

  ReadResult readLine(int timeout_ms, std::string& line, std::string& error) override {
    (void)error;
    std::unique_lock<std::mutex> lock(mutex_);
    if (!incoming.empty()) {
      line = incoming.front();
      incoming.pop_front();
      return ReadResult::Line;
    }
    condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !incoming.empty(); });
    if (!incoming.empty()) {
      line = incoming.front();
      incoming.pop_front();
      return ReadResult::Line;
    }
    return ReadResult::Timeout;
  }

  // The result bodies a test can replace.
  std::string initialize_body;
  std::string list_body;
  std::string call_body;
  // Pages of tools, handed out one per tools/list request.
  std::vector<std::string> list_pages;

  std::vector<std::string> notificationNames() {
    std::lock_guard<std::mutex> guard(mutex_);
    return notifications;
  }

  std::vector<std::string> sentLines() {
    std::lock_guard<std::mutex> guard(mutex_);
    return sent;
  }

  void push(const std::string& line) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      incoming.push_back(line);
    }
    condition_.notify_all();
  }

 private:
  std::string listBody() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (list_pages.empty()) {
      return list_body.empty()
                 ? R"({"tools":[{"name":"echo","description":"Echoes the text","inputSchema":{"type":"object",)"
                   R"("properties":{"text":{"type":"string"}}}}]})"
                 : list_body;
    }
    const std::string page = list_pages.front();
    list_pages.erase(list_pages.begin());
    return page;
  }

  std::string callBody() {
    std::lock_guard<std::mutex> guard(mutex_);
    return call_body.empty() ? R"({"content":[{"type":"text","text":"called"}]})" : call_body;
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::string> incoming;
  std::vector<std::string> notifications;
  std::vector<std::string> sent;
};

// Builds the pieces a client needs over the scripted server.
struct Harness {
  std::shared_ptr<ScriptedServer> transport = std::make_shared<ScriptedServer>();
  std::shared_ptr<Endpoint> endpoint = std::make_shared<Endpoint>(transport);
  std::shared_ptr<bjorne_agent::mcp::Client> client = std::make_shared<bjorne_agent::mcp::Client>(endpoint);
};

}  // namespace

TEST_CASE("the client performs the initialize handshake") {
  Harness harness;
  harness.endpoint->start();

  std::string error;
  REQUIRE(harness.client->initialize(error));
  REQUIRE(error.empty());
  const std::vector<std::string> notifications = harness.transport->notificationNames();
  REQUIRE(notifications.size() == 1);
  REQUIRE(notifications[0] == "notifications/initialized");
}

TEST_CASE("an initialize response that is not an object is refused") {
  Harness harness;
  harness.transport->initialize_body = R"("not an object")";
  harness.endpoint->start();

  std::string error;
  REQUIRE_FALSE(harness.client->initialize(error));
  REQUIRE(error.find("not an object") != std::string::npos);
}

TEST_CASE("the client lists the tools of a server") {
  Harness harness;
  harness.endpoint->start();

  std::vector<bjorne_agent::mcp::ToolInfo> tools;
  std::string error;
  CAPTURE(error);
  REQUIRE(harness.client->listTools(tools, error));
  REQUIRE(tools.size() == 1);
  REQUIRE(tools[0].name == "echo");
  REQUIRE(tools[0].description == "Echoes the text");
  REQUIRE(tools[0].input_schema.isObject());
}

TEST_CASE("the client follows the pagination cursor") {
  Harness harness;
  harness.transport->list_pages = {
      R"({"tools":[{"name":"first"}],"nextCursor":"page2"})",
      R"({"tools":[{"name":"second"}]})",
  };
  harness.endpoint->start();

  std::vector<bjorne_agent::mcp::ToolInfo> tools;
  std::string error;
  REQUIRE(harness.client->listTools(tools, error));
  REQUIRE(tools.size() == 2);
  REQUIRE(tools[0].name == "first");
  REQUIRE(tools[1].name == "second");
}

TEST_CASE("a tools list without a tools array is refused") {
  Harness harness;
  harness.transport->list_body = R"({"other":true})";
  harness.endpoint->start();

  std::vector<bjorne_agent::mcp::ToolInfo> tools;
  std::string error;
  REQUIRE_FALSE(harness.client->listTools(tools, error));
  REQUIRE(error.find("tools array") != std::string::npos);
}

TEST_CASE("a tool without a schema gets an empty object schema") {
  Harness harness;
  harness.transport->list_body = R"({"tools":[{"name":"bare"}]})";
  harness.endpoint->start();

  std::vector<bjorne_agent::mcp::ToolInfo> tools;
  std::string error;
  REQUIRE(harness.client->listTools(tools, error));
  REQUIRE(tools.size() == 1);
  REQUIRE(tools[0].input_schema.isObject());
  REQUIRE(tools[0].description.empty());
}

TEST_CASE("the client calls a tool and reads the text content") {
  Harness harness;
  harness.endpoint->start();

  Value arguments;
  arguments.type = Value::Type::Object;
  arguments.object["text"] = Value();
  arguments.object["text"].type = Value::Type::String;
  arguments.object["text"].string = "hello";

  std::string output;
  bool is_error = true;
  std::string error;
  REQUIRE(harness.client->callTool("echo", arguments, output, is_error, error));
  REQUIRE(output == "called");
  REQUIRE_FALSE(is_error);
}

TEST_CASE("content that is not text becomes a placeholder") {
  Harness harness;
  harness.transport->call_body = R"({"content":[{"type":"image","data":"..."}]})";
  harness.endpoint->start();

  std::string output;
  bool is_error = false;
  std::string error;
  REQUIRE(harness.client->callTool("echo", Value(), output, is_error, error));
  REQUIRE(output == "[image content omitted]");
}

TEST_CASE("a tool that reports an error is marked as one") {
  Harness harness;
  harness.transport->call_body = R"({"content":[{"type":"text","text":"it failed"}],"isError":true})";
  harness.endpoint->start();

  std::string output;
  bool is_error = false;
  std::string error;
  REQUIRE(harness.client->callTool("echo", Value(), output, is_error, error));
  REQUIRE(is_error);
  REQUIRE(output == "it failed");
}

TEST_CASE("the client answers a ping from the server") {
  Harness harness;
  harness.endpoint->start();

  harness.transport->push(R"({"jsonrpc":"2.0","id":5,"method":"ping","params":{}})");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const std::vector<std::string> sent = harness.transport->sentLines();
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].find("\"id\":5") != std::string::npos);
  REQUIRE(sent[0].find("\"result\"") != std::string::npos);
}

TEST_CASE("the tool name carries the server and the tool") {
  REQUIRE(bjorne_agent::mcp::toolName("files", "read") == "mcp__files__read");
}

TEST_CASE("a server starts, lists its tools and calls one") {
  bjorne_agent::config::McpServerConfig config;
  config.command = BJORNE_AGENT_MCP_STUB;

  bjorne_agent::mcp::Server server("demo", config);
  std::string error;
  REQUIRE(server.start(error));
  REQUIRE(server.tools().size() == 1);
  REQUIRE(server.tools()[0].name == "echo");

  const bjorne_agent::tools::ToolResult result = server.call("echo", R"({"text":"hi"})");
  REQUIRE(result.ok);
  REQUIRE(result.output == "echo:1");
}

TEST_CASE("a server that dies is restarted and the call is retried") {
  bjorne_agent::config::McpServerConfig config;
  config.command = BJORNE_AGENT_MCP_STUB;
  config.env["BJORNE_AGENT_MCP_STUB_EXIT_AFTER"] = "1";

  bjorne_agent::mcp::Server server("demo", config);
  std::string error;
  REQUIRE(server.start(error));

  // The stub exits after the first call, so the first attempt fails on the
  // closed pipe and the retry runs against the restarted server.
  const bjorne_agent::tools::ToolResult result = server.call("echo", R"({"text":"hi"})");
  REQUIRE(result.ok);
  REQUIRE(result.output == "echo:1");
}

TEST_CASE("a server that cannot start reports the reason") {
  bjorne_agent::config::McpServerConfig config;
  config.command = "bjorne_agent_no_such_command_12345";

  bjorne_agent::mcp::Server server("demo", config);
  std::string error;
  REQUIRE_FALSE(server.start(error));
  REQUIRE(error.find("bjorne_agent_no_such_command_12345") != std::string::npos);
}

TEST_CASE("a call with broken arguments is refused") {
  bjorne_agent::config::McpServerConfig config;
  config.command = BJORNE_AGENT_MCP_STUB;

  bjorne_agent::mcp::Server server("demo", config);
  std::string error;
  REQUIRE(server.start(error));

  const bjorne_agent::tools::ToolResult result = server.call("echo", "not json");
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.output.find("JSON object") != std::string::npos);
}

TEST_CASE("registering servers adds the namespaced tools to the registry") {
  bjorne_agent::config::McpServerConfig config;
  config.command = BJORNE_AGENT_MCP_STUB;

  std::map<std::string, bjorne_agent::config::McpServerConfig> servers;
  servers["demo"] = config;

  bjorne_agent::tools::ToolRegistry registry("");
  bjorne_agent::mcp::registerServers(servers, registry);

  REQUIRE(registry.hasTool("mcp__demo__echo"));
  REQUIRE(registry.hasTool("run_shell"));
  const std::string definitions = registry.definitionsJson();
  REQUIRE(definitions.find("mcp__demo__echo") != std::string::npos);

  const bjorne_agent::tools::ToolResult result = registry.execute("mcp__demo__echo", R"({"text":"hi"})");
  REQUIRE(result.ok);
  REQUIRE(result.output == "echo:1");
}

TEST_CASE("a server that cannot start leaves the tool set untouched") {
  bjorne_agent::config::McpServerConfig config;
  config.command = "bjorne_agent_no_such_command_12345";

  std::map<std::string, bjorne_agent::config::McpServerConfig> servers;
  servers["demo"] = config;

  bjorne_agent::tools::ToolRegistry registry("");
  bjorne_agent::mcp::registerServers(servers, registry);

  REQUIRE_FALSE(registry.hasTool("mcp__demo__echo"));
  REQUIRE(registry.hasTool("run_shell"));
}

TEST_CASE("a server without a command is skipped") {
  std::map<std::string, bjorne_agent::config::McpServerConfig> servers;
  servers["demo"] = bjorne_agent::config::McpServerConfig();

  bjorne_agent::tools::ToolRegistry registry("");
  bjorne_agent::mcp::registerServers(servers, registry);

  REQUIRE_FALSE(registry.hasTool("mcp__demo__echo"));
}
