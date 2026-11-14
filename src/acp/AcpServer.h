#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "agent/AgentLoop.h"
#include "config/Config.h"
#include "context/ContextManager.h"
#include "json/Json.h"
#include "jsonrpc/Endpoint.h"
#include "model/ModelClient.h"
#include "session/Session.h"
#include "tools/Tools.h"

namespace bjorne_agent {
namespace acp {

// The error code ACP uses for a session or another resource that does not
// exist. It is outside the JSON-RPC range on purpose.
constexpr int kResourceNotFound = -32002;

// What one ACP session runs on. The model, the context budget and the tool set
// depend on the workspace and the model the client selected, so the server asks
// its factory for them instead of sharing the one-shot runtime.
struct SessionRuntime {
  std::shared_ptr<model::ModelBackend> model;
  std::shared_ptr<context::ContextManager> context;
  std::shared_ptr<tools::ToolRegistry> tools;
  std::string system_prompt;
};

// The settings a session setup asks the factory for. `model` is empty when the
// configured default should be used. `mcp_servers` holds the servers the
// client named for this session; the factory merges them over the configured
// ones by name, with the client winning.
struct RuntimeRequest {
  std::string workspace;
  std::string model;
  std::map<std::string, config::McpServerConfig> mcp_servers;
};

using RuntimeFactory = std::function<bool(const RuntimeRequest&, SessionRuntime&, std::string&)>;

// The agent half of the Agent Client Protocol. The endpoint hands it every
// request from the editor and it answers; it streams session updates back
// through the same endpoint, so the questions a turn asks the editor travel
// back over the connection the turn came in on. The editor is the client half
// of the same endpoint class, which is what the MCP client already is.
class AcpServer {
 public:
  AcpServer(std::shared_ptr<jsonrpc::Endpoint> endpoint, std::shared_ptr<session::SessionStore> store,
            RuntimeFactory factory, std::vector<std::string> models, std::string default_model);

  // The entry point the endpoint calls for every request. Returns false and
  // fills error for a refusal; an empty error means the result is in result.
  bool handle(const std::string& method, const json::Value& params, json::Value& result, jsonrpc::Error& error);

  // The entry point the endpoint calls for every notification. Only the
  // cancellation notification has a handler today.
  void notify(const std::string& method, const json::Value& params);

  // The protocol version this build speaks. Version 1 is the latest the agent
  // implements; the draft v2 is watched and not followed.
  static constexpr int kProtocolVersion = 1;

  // The identity the agent advertises in the initialize response.
  static constexpr const char* kAgentName = "bjorne_agent";
  static constexpr const char* kAgentVersion = "0.1.0";

 private:
  struct Session;

  // Turns the loop events into session updates for one conversation, and
  // writes the stored record after every event.
  class SessionObserver final : public agent::LoopObserver {
   public:
    SessionObserver(AcpServer& server, std::shared_ptr<Session> session);

    void onTurn(const std::vector<model::ModelMessage>& messages, long long turn) override;
    void onAssistantMessage(const std::string& content) override;
    void onReasoning(const std::string& reasoning) override;
    void onComplete(const std::vector<model::ModelMessage>& messages, const std::string& result) override;
    void onFailure(const std::vector<model::ModelMessage>& messages, const std::string& error) override;
    void onInterrupt(const std::vector<model::ModelMessage>& messages) override;

   private:
    void persist(const std::vector<model::ModelMessage>& messages, const std::string& status, const std::string& result,
                 const std::string& error);

    AcpServer& server_;
    std::shared_ptr<Session> session_;
  };

  struct Session {
    // One line of the plan the agent is following. The status moves from
    // pending to in_progress to completed as the turn goes.
    struct PlanEntry {
      std::string content;
      std::string status;
    };

    std::string id;
    std::string workspace;
    std::string model;
    std::string system_prompt;
    std::map<std::string, config::McpServerConfig> mcp_servers;
    std::shared_ptr<model::ModelBackend> model_backend;
    std::shared_ptr<context::ContextManager> context;
    std::shared_ptr<tools::ToolRegistry> tools;
    std::shared_ptr<SessionObserver> observer;
    session::SessionState state;
    // Set by the reader thread when the editor cancels, polled by the loop.
    std::atomic<bool> cancelled{false};
    // Tool calls announced to the editor and not yet finished. Only the worker
    // thread touches this.
    std::vector<model::ToolCall> pending_calls;
    // The plan of the turn that is running. It is empty between turns.
    std::vector<PlanEntry> plan;
    bool plan_open = false;
    // The tools the user allowed for the rest of the session.
    std::set<std::string> always_allowed;
  };

  bool initialize(const json::Value& params, json::Value& result, jsonrpc::Error& error);
  void readClientCapabilities(const json::Value& params);
  void readFsCapabilities(const json::Value& capabilities);
  void readElicitationCapability(const json::Value& capabilities);
  json::Value agentCapabilities() const;
  bool sessionNew(const json::Value& params, json::Value& result, jsonrpc::Error& error);
  bool sessionPrompt(const json::Value& params, json::Value& result, jsonrpc::Error& error);
  bool sessionLoad(const json::Value& params, json::Value& result, jsonrpc::Error& error, bool replay);
  bool sessionList(const json::Value& params, json::Value& result, jsonrpc::Error& error);
  bool sessionDelete(const json::Value& params, json::Value& result, jsonrpc::Error& error);
  bool sessionSetConfig(const json::Value& params, json::Value& result, jsonrpc::Error& error);

  std::shared_ptr<Session> findSession(const std::string& id) const;
  std::shared_ptr<Session> buildSession(const session::SessionState& state, const std::string& workspace,
                                        const std::string& model, const std::map<std::string, config::McpServerConfig>& servers,
                                        std::string& error);
  bool rebuildRuntime(const std::shared_ptr<Session>& session, const std::string& model, std::string& error);
  void installSessionTools(const std::shared_ptr<Session>& session);
  void installReadTool(const std::shared_ptr<Session>& session, const std::weak_ptr<Session>& weak);
  void installWriteTool(const std::shared_ptr<Session>& session, const std::weak_ptr<Session>& weak);
  void installShellTool(const std::shared_ptr<Session>& session, const std::weak_ptr<Session>& weak);
  void installAskTool(const std::shared_ptr<Session>& session, const std::weak_ptr<Session>& weak);
  json::Value configOptions(const Session& session) const;

  bool readMcpServers(const json::Value& params, std::map<std::string, config::McpServerConfig>& out,
                      jsonrpc::Error& error) const;
  bool readPrompt(const json::Value& params, std::string& text, jsonrpc::Error& error) const;

  bool gate(const std::shared_ptr<Session>& session, const std::string& tool, const std::string& arguments,
            const std::string& tool_call_id, std::string& denial);

  void sendUpdate(const std::string& session_id, json::Value update);
  void emitMessage(const std::string& session_id, const std::string& kind, const std::string& text);
  void emitCommands(const std::string& session_id);
  void emitToolCall(Session& session, const model::ToolCall& call);
  void emitToolStatus(Session& session, const std::string& tool_call_id, const std::string& status);
  void emitPlan(Session& session);
  void finishPendingCalls(Session& session, const std::vector<model::ModelMessage>& messages);
  void failPendingCalls(Session& session, const std::string& reason);
  bool requestPermission(Session& session, const model::ToolCall& call, std::string& denial);
  void replay(const Session& session);

  bool clientReadFile(const Session& session, const std::string& path, std::string& content, std::string& error);
  bool clientWriteFile(const Session& session, const std::string& path, const std::string& content, std::string& error);
  bool clientRunShell(const Session& session, const std::string& command, std::string& output, bool& ok,
                      std::string& error);
  std::string terminalOutput(const Session& session, const std::string& terminal_id);
  void releaseTerminal(const Session& session, const std::string& terminal_id);
  bool clientAsk(const Session& session, const std::string& question, const std::vector<std::string>& options,
                 std::string& answer, std::string& error);

  std::shared_ptr<jsonrpc::Endpoint> endpoint_;
  std::shared_ptr<session::SessionStore> store_;
  RuntimeFactory factory_;
  std::vector<std::string> models_;
  std::string default_model_;

  mutable std::mutex sessions_mutex_;
  std::map<std::string, std::shared_ptr<Session>> sessions_;

  bool initialized_ = false;
  bool client_read_ = false;
  bool client_write_ = false;
  bool client_terminal_ = false;
  bool client_elicitation_ = false;
};

}  // namespace acp
}  // namespace bjorne_agent
