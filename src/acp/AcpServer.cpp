#include "acp/AcpServer.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "acp/AcpChannel.h"
#include "json/Json.h"
#include "log/Log.h"

namespace bjorne_agent {
namespace acp {

namespace {

constexpr int kClientTimeoutMs = 30000;
constexpr int kShellTimeoutMs = 300000;

json::Value objectValue() {
  json::Value value;
  value.type = json::Value::Type::Object;
  return value;
}

json::Value arrayValue() {
  json::Value value;
  value.type = json::Value::Type::Array;
  return value;
}

json::Value stringValue(const std::string& text) {
  json::Value value;
  value.type = json::Value::Type::String;
  value.string = text;
  return value;
}

json::Value boolValue(bool flag) {
  json::Value value;
  value.type = json::Value::Type::Boolean;
  value.boolean = flag;
  return value;
}

json::Value numberValue(double number) {
  json::Value value;
  value.type = json::Value::Type::Number;
  value.number = number;
  return value;
}

// The shell the agent runs a command line in. The model gives a whole command
// line, so it is handed to the shell as one argument.
const char* shellProgram() {
#if defined(_WIN32)
  return "cmd.exe";
#else
  return "/bin/sh";
#endif
}

const char* shellFlag() {
#if defined(_WIN32)
  return "/C";
#else
  return "-c";
#endif
}

// The ACP tool kinds. The editor uses the kind to pick an icon and to decide
// how much of the operation it shows.
std::string toolKind(const std::string& name) {
  if (name == "read_file" || name == "load_skill") {
    return "read";
  }
  if (name == "write_file" || name == "edit_file") {
    return "edit";
  }
  if (name == "run_shell") {
    return "execute";
  }
  return "other";
}

// A one line description of the operation, for the editor.
std::string toolTitle(const std::string& name, const std::string& arguments) {
  json::Value args;
  std::string path;
  if (tools::parseToolArguments(arguments, args) && tools::toolStringArgument(args, "path", path) && !path.empty()) {
    if (name == "read_file") {
      return "Read " + path;
    }
    if (name == "write_file") {
      return "Write " + path;
    }
    if (name == "edit_file") {
      return "Edit " + path;
    }
  }
  std::string command;
  if (name == "run_shell" && tools::parseToolArguments(arguments, args) &&
      tools::toolStringArgument(args, "command", command) && !command.empty()) {
    return "Run " + command;
  }
  return name;
}

// The tools that need no permission. Reading a file and loading a skill do
// not change anything, and asking the user is a question, not an action.
bool autoApproved(const std::string& name) {
  return name == "read_file" || name == "load_skill" || name == "request_input";
}

// The text of a block, when it is a plain text block.
bool appendTextBlock(const json::Value& block, std::string& out, jsonrpc::Error& error) {
  const json::Value* text = block.find("text");
  if (text == nullptr || !text->isString()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "A text block has no text";
    return false;
  }
  out.append(text->string);
  return true;
}

// A link to a resource the agent may read. The link itself is the text,
// because the agent has no way to open a link.
bool appendResourceLinkBlock(const json::Value& block, std::string& out) {
  const json::Value* uri = block.find("uri");
  const json::Value* name = block.find("name");
  out.append("Resource: ");
  out.append(name != nullptr && name->isString() ? name->string : "resource");
  out.append(" (");
  out.append(uri != nullptr && uri->isString() ? uri->string : "");
  out.append(")\n");
  return true;
}

// An embedded resource. Its text is used when there is one, and its location
// is used when there is not.
bool appendResourceBlock(const json::Value& block, std::string& out, jsonrpc::Error& error) {
  const json::Value* resource = block.find("resource");
  if (resource != nullptr && resource->isObject()) {
    const json::Value* text = resource->find("text");
    if (text != nullptr && text->isString()) {
      out.append(text->string);
      return true;
    }
    const json::Value* uri = resource->find("uri");
    if (uri != nullptr && uri->isString()) {
      out.append("Resource: ");
      out.append(uri->string);
      out.append("\n");
      return true;
    }
  }
  error.code = jsonrpc::kInvalidParams;
  error.message = "A resource block carries no text";
  return false;
}

// One content block of a prompt, turned into text the model can read.
bool appendPromptBlock(const json::Value& block, std::string& out, jsonrpc::Error& error) {
  const json::Value* type = block.find("type");
  if (type == nullptr || !type->isString()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "A prompt block has no type";
    return false;
  }
  if (type->string == "text") {
    return appendTextBlock(block, out, error);
  }
  if (type->string == "resource_link") {
    return appendResourceLinkBlock(block, out);
  }
  if (type->string == "resource") {
    return appendResourceBlock(block, out, error);
  }
  error.code = jsonrpc::kInvalidParams;
  error.message = "Unsupported prompt block type: " + type->string;
  return false;
}

// The arguments of an MCP server the client named.
bool readMcpArgs(const json::Value& entry, const std::string& name, config::McpServerConfig& server,
                 jsonrpc::Error& error) {
  const json::Value* args = entry.find("args");
  if (args == nullptr || !args->isArray()) {
    return true;
  }
  for (const json::Value& argument : args->array) {
    if (!argument.isString()) {
      error.code = jsonrpc::kInvalidParams;
      error.message = "The MCP server " + name + " has a non-string argument";
      return false;
    }
    server.args.push_back(argument.string);
  }
  return true;
}

// The environment of an MCP server the client named.
bool readMcpEnv(const json::Value& entry, const std::string& name, config::McpServerConfig& server,
                jsonrpc::Error& error) {
  const json::Value* env = entry.find("env");
  if (env == nullptr || !env->isArray()) {
    return true;
  }
  for (const json::Value& variable : env->array) {
    const json::Value* key = variable.find("name");
    const json::Value* value = variable.find("value");
    if (key == nullptr || !key->isString() || value == nullptr || !value->isString()) {
      error.code = jsonrpc::kInvalidParams;
      error.message = "The MCP server " + name + " has an environment entry without a name and a value";
      return false;
    }
    server.env[key->string] = value->string;
  }
  return true;
}

// One MCP server the client named for a session.
bool readMcpServer(const json::Value& entry, std::string& name, config::McpServerConfig& server,
                   jsonrpc::Error& error) {
  if (!entry.isObject()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "An MCP server entry is not an object";
    return false;
  }
  const json::Value* nameValue = entry.find("name");
  const json::Value* command = entry.find("command");
  if (nameValue == nullptr || !nameValue->isString() || nameValue->string.empty()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "An MCP server entry has no name";
    return false;
  }
  if (command == nullptr || !command->isString() || command->string.empty()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "The MCP server " + nameValue->string + " has no command";
    return false;
  }
  name = nameValue->string;
  server.command = command->string;

  if (!readMcpArgs(entry, name, server, error)) {
    return false;
  }
  return readMcpEnv(entry, name, server, error);
}

// The tools the model sees, as one JSON document.
std::string toolArgumentsOf(const std::string& arguments, const std::string& key) {
  json::Value args;
  if (!tools::parseToolArguments(arguments, args)) {
    return {};
  }
  std::string value;
  if (!tools::toolStringArgument(args, key, value)) {
    return {};
  }
  return value;
}

}  // namespace

AcpServer::AcpServer(std::shared_ptr<jsonrpc::Endpoint> endpoint, std::shared_ptr<session::SessionStore> store,
                     RuntimeFactory factory, std::vector<std::string> models, std::string default_model)
    : endpoint_(std::move(endpoint)),
      store_(std::move(store)),
      factory_(std::move(factory)),
      models_(std::move(models)),
      default_model_(std::move(default_model)) {}

bool AcpServer::handle(const std::string& method, const json::Value& params, json::Value& result,
                        jsonrpc::Error& error) {
  if (method != "initialize" && !initialized_) {
    error.code = jsonrpc::kInvalidRequest;
    error.message = "The connection is not initialized";
    return false;
  }
  if (method == "initialize") {
    return initialize(params, result, error);
  }
  if (method == "session/new") {
    return sessionNew(params, result, error);
  }
  if (method == "session/prompt") {
    return sessionPrompt(params, result, error);
  }
  if (method == "session/load") {
    return sessionLoad(params, result, error, true);
  }
  if (method == "session/resume") {
    return sessionLoad(params, result, error, false);
  }
  if (method == "session/list") {
    return sessionList(params, result, error);
  }
  if (method == "session/delete") {
    return sessionDelete(params, result, error);
  }
  if (method == "session/set_config_option") {
    return sessionSetConfig(params, result, error);
  }
  error.code = jsonrpc::kMethodNotFound;
  error.message = "Method not found: " + method;
  return false;
}

void AcpServer::notify(const std::string& method, const json::Value& params) {
  if (method != "session/cancel") {
    return;
  }
  const json::Value* id = params.find("sessionId");
  if (id == nullptr || !id->isString()) {
    return;
  }
  const std::shared_ptr<Session> session = findSession(id->string);
  if (session) {
    // The loop polls the flag, and a permission request that is waiting for an
    // answer gives up when it is set.
    session->cancelled.store(true);
  }
}

// The filesystem capabilities the editor advertises.
void AcpServer::readFsCapabilities(const json::Value& capabilities) {
  const json::Value* fs = capabilities.find("fs");
  if (fs == nullptr || !fs->isObject()) {
    return;
  }
  const json::Value* read = fs->find("readTextFile");
  const json::Value* write = fs->find("writeTextFile");
  client_read_ = read != nullptr && read->isBoolean() && read->boolean;
  client_write_ = write != nullptr && write->isBoolean() && write->boolean;
}

// The elicitation capability the editor advertises. Only the form mode is
// used, and it has to be advertised explicitly.
void AcpServer::readElicitationCapability(const json::Value& capabilities) {
  const json::Value* elicitation = capabilities.find("elicitation");
  if (elicitation == nullptr || !elicitation->isObject()) {
    return;
  }
  const json::Value* form = elicitation->find("form");
  client_elicitation_ = form != nullptr && !form->isNull();
}

// What the editor can do for the agent. The agent only asks for what the
// editor said it supports.
void AcpServer::readClientCapabilities(const json::Value& params) {
  const json::Value* capabilities = params.find("clientCapabilities");
  if (capabilities == nullptr || !capabilities->isObject()) {
    return;
  }
  readFsCapabilities(*capabilities);
  const json::Value* terminal = capabilities->find("terminal");
  client_terminal_ = terminal != nullptr && terminal->isBoolean() && terminal->boolean;
  readElicitationCapability(*capabilities);
}

// What the agent can do. The session capabilities follow the configuration,
// because without a store there is nothing to list, load or resume.
json::Value AcpServer::agentCapabilities() const {
  json::Value capabilities = objectValue();
  capabilities.object["loadSession"] = boolValue(store_ != nullptr);

  json::Value promptCapabilities = objectValue();
  promptCapabilities.object["image"] = boolValue(false);
  promptCapabilities.object["audio"] = boolValue(false);
  promptCapabilities.object["embeddedContext"] = boolValue(true);
  capabilities.object["promptCapabilities"] = std::move(promptCapabilities);

  json::Value mcpCapabilities = objectValue();
  mcpCapabilities.object["http"] = boolValue(false);
  mcpCapabilities.object["sse"] = boolValue(false);
  capabilities.object["mcpCapabilities"] = std::move(mcpCapabilities);

  if (store_) {
    json::Value sessionCapabilities = objectValue();
    sessionCapabilities.object["list"] = objectValue();
    sessionCapabilities.object["resume"] = objectValue();
    sessionCapabilities.object["delete"] = objectValue();
    capabilities.object["sessionCapabilities"] = std::move(sessionCapabilities);
  }
  return capabilities;
}

bool AcpServer::initialize(const json::Value& params, json::Value& result, jsonrpc::Error& error) {
  if (initialized_) {
    error.code = jsonrpc::kInvalidRequest;
    error.message = "The connection is already initialized";
    return false;
  }

  int requested = 0;
  const json::Value* version = params.find("protocolVersion");
  if (version != nullptr && version->isNumber()) {
    requested = static_cast<int>(version->number);
  }
  // The version the client asked for when it is supported, and the latest the
  // agent speaks otherwise.
  const int chosen = requested == kProtocolVersion ? requested : kProtocolVersion;
  readClientCapabilities(params);

  json::Value info = objectValue();
  info.object["name"] = stringValue(kAgentName);
  info.object["version"] = stringValue(kAgentVersion);

  result = objectValue();
  result.object["protocolVersion"] = numberValue(chosen);
  result.object["agentCapabilities"] = agentCapabilities();
  result.object["agentInfo"] = std::move(info);
  result.object["authMethods"] = arrayValue();
  initialized_ = true;
  return true;
}

bool AcpServer::sessionNew(const json::Value& params, json::Value& result, jsonrpc::Error& error) {
  const json::Value* cwd = params.find("cwd");
  if (cwd == nullptr || !cwd->isString() || cwd->string.empty()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "session/new requires a cwd";
    return false;
  }
  std::map<std::string, config::McpServerConfig> servers;
  if (!readMcpServers(params, servers, error)) {
    return false;
  }

  session::SessionState state;
  state.id = "sess_" + session::generateSessionId();
  state.workspace = cwd->string;
  state.model = default_model_;
  if (store_) {
    std::string storeError;
    if (!store_->begin(state, storeError)) {
      error.code = jsonrpc::kInternalError;
      error.message = storeError;
      return false;
    }
  }

  std::string buildError;
  const std::shared_ptr<Session> session =
      buildSession(state, state.workspace, default_model_, servers, buildError);
  if (!session) {
    error.code = jsonrpc::kInternalError;
    error.message = buildError;
    return false;
  }

  result = objectValue();
  result.object["sessionId"] = stringValue(session->id);
  const json::Value options = configOptions(*session);
  if (!options.array.empty()) {
    result.object["configOptions"] = options;
  }
  emitCommands(session->id);
  return true;
}

bool AcpServer::sessionPrompt(const json::Value& params, json::Value& result, jsonrpc::Error& error) {
  const json::Value* id = params.find("sessionId");
  if (id == nullptr || !id->isString()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "session/prompt requires a sessionId";
    return false;
  }
  const std::shared_ptr<Session> session = findSession(id->string);
  if (!session) {
    error.code = kResourceNotFound;
    error.message = "No such session: " + id->string;
    error.data = objectValue();
    error.data.object["sessionId"] = stringValue(id->string);
    error.hasData = true;
    return false;
  }

  std::string task;
  if (!readPrompt(params, task, error)) {
    return false;
  }
  if (session->state.task.empty()) {
    session->state.task = task;
  }

  session->cancelled.store(false);
  agent::AgentLoopConfig loopConfig;
  loopConfig.system_prompt = session->system_prompt;
  loopConfig.workspace = session->workspace;
  loopConfig.history = session->state.messages;
  loopConfig.stop_requested = [session]() { return session->cancelled.load(); };
  loopConfig.permissionGate = [this, session](const std::string& tool, const std::string& arguments,
                                              const std::string& tool_call_id, std::string& denial) {
    return gate(session, tool, arguments, tool_call_id, denial);
  };

  agent::AgentLoop loop(loopConfig, session->model_backend, session->context, session->tools, session->observer);
  std::string answer;
  std::string runError;
  const bool ok = loop.run(task, answer, runError);

  if (!ok) {
    const std::string reason = loop.stopReason();
    // A cancelled turn and the two turn limits are answers, not failures.
    if (reason == "cancelled" || reason == "max_tokens" || reason == "max_turn_requests") {
      result = objectValue();
      result.object["stopReason"] = stringValue(reason);
      return true;
    }
    error.code = jsonrpc::kInternalError;
    error.message = runError;
    return false;
  }

  result = objectValue();
  result.object["stopReason"] = stringValue(loop.stopReason().empty() ? "end_turn" : loop.stopReason());
  return true;
}

bool AcpServer::sessionLoad(const json::Value& params, json::Value& result, jsonrpc::Error& error, bool with_replay) {
  if (!store_) {
    error.code = jsonrpc::kMethodNotFound;
    error.message = "This agent has no session store";
    return false;
  }
  const json::Value* id = params.find("sessionId");
  if (id == nullptr || !id->isString()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "A session request requires a sessionId";
    return false;
  }

  session::SessionState state;
  std::string loadError;
  if (!store_->load(id->string, state, loadError)) {
    error.code = kResourceNotFound;
    error.message = "No such session: " + id->string;
    error.data = objectValue();
    error.data.object["sessionId"] = stringValue(id->string);
    error.hasData = true;
    return false;
  }

  std::map<std::string, config::McpServerConfig> servers;
  if (!readMcpServers(params, servers, error)) {
    return false;
  }

  const std::string model = state.model.empty() ? default_model_ : state.model;
  std::string buildError;
  const std::shared_ptr<Session> session = buildSession(state, state.workspace, model, servers, buildError);
  if (!session) {
    error.code = jsonrpc::kInternalError;
    error.message = buildError;
    return false;
  }
  if (with_replay) {
    // A load shows the conversation that was there. A resume only picks it up
    // again, so it says nothing.
    replay(*session);
    emitCommands(session->id);
  }

  result = objectValue();
  const json::Value options = configOptions(*session);
  if (!options.array.empty()) {
    result.object["configOptions"] = options;
  }
  return true;
}

bool AcpServer::sessionList(const json::Value& params, json::Value& result, jsonrpc::Error& error) {
  if (!store_) {
    error.code = jsonrpc::kMethodNotFound;
    error.message = "This agent has no session store";
    return false;
  }
  std::string cwd;
  const json::Value* cwdValue = params.find("cwd");
  if (cwdValue != nullptr && !cwdValue->isNull()) {
    if (!cwdValue->isString()) {
      error.code = jsonrpc::kInvalidParams;
      error.message = "cwd must be a string";
      return false;
    }
    cwd = cwdValue->string;
  }

  std::string listError;
  const std::vector<session::SessionSummary> summaries = store_->list(listError);
  json::Value items = arrayValue();
  for (const session::SessionSummary& summary : summaries) {
    if (!cwd.empty() && summary.workspace != cwd) {
      continue;
    }
    json::Value item = objectValue();
    item.object["sessionId"] = stringValue(summary.id);
    item.object["cwd"] = stringValue(summary.workspace);
    item.object["title"] = stringValue(summary.task);
    item.object["updatedAt"] = stringValue(session::formatIsoTimestamp(summary.updated));
    items.array.push_back(std::move(item));
  }

  result = objectValue();
  result.object["sessions"] = std::move(items);
  return true;
}

bool AcpServer::sessionDelete(const json::Value& params, json::Value& result, jsonrpc::Error& error) {
  if (!store_) {
    error.code = jsonrpc::kMethodNotFound;
    error.message = "This agent has no session store";
    return false;
  }
  const json::Value* id = params.find("sessionId");
  if (id == nullptr || !id->isString()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "session/delete requires a sessionId";
    return false;
  }

  std::string eraseError;
  if (!store_->erase(id->string, eraseError)) {
    error.code = session::isValidSessionId(id->string) ? jsonrpc::kInternalError : jsonrpc::kInvalidParams;
    error.message = eraseError;
    return false;
  }
  {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    sessions_.erase(id->string);
  }
  result = objectValue();
  return true;
}

bool AcpServer::sessionSetConfig(const json::Value& params, json::Value& result, jsonrpc::Error& error) {
  const json::Value* id = params.find("sessionId");
  if (id == nullptr || !id->isString()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "session/set_config_option requires a sessionId";
    return false;
  }
  const std::shared_ptr<Session> session = findSession(id->string);
  if (!session) {
    error.code = kResourceNotFound;
    error.message = "No such session: " + id->string;
    error.data = objectValue();
    error.data.object["sessionId"] = stringValue(id->string);
    error.hasData = true;
    return false;
  }

  const json::Value* configId = params.find("configId");
  if (configId == nullptr || !configId->isString() || configId->string != "model") {
    error.code = jsonrpc::kInvalidParams;
    error.message = "The only configuration option is model";
    return false;
  }
  const json::Value* value = params.find("value");
  if (value == nullptr || !value->isString()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "The model option takes a string value";
    return false;
  }
  if (std::find(models_.begin(), models_.end(), value->string) == models_.end()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "Unknown model: " + value->string;
    return false;
  }

  std::string rebuildError;
  if (!rebuildRuntime(session, value->string, rebuildError)) {
    error.code = jsonrpc::kInternalError;
    error.message = rebuildError;
    return false;
  }
  session->state.model = value->string;
  if (store_) {
    std::string saveError;
    if (!store_->save(session->state, saveError)) {
      log::fail("Cannot save the session: " + saveError);
    }
  }

  result = objectValue();
  result.object["configOptions"] = configOptions(*session);
  return true;
}

std::shared_ptr<AcpServer::Session> AcpServer::findSession(const std::string& id) const {
  std::lock_guard<std::mutex> guard(sessions_mutex_);
  auto entry = sessions_.find(id);
  if (entry == sessions_.end()) {
    return nullptr;
  }
  return entry->second;
}

std::shared_ptr<AcpServer::Session> AcpServer::buildSession(const session::SessionState& state,
                                                             const std::string& workspace, const std::string& model,
                                                             const std::map<std::string, config::McpServerConfig>& servers,
                                                             std::string& error) {
  auto session = std::make_shared<Session>();
  session->id = state.id;
  session->workspace = workspace;
  session->model = model;
  session->mcp_servers = servers;
  session->state = state;
  if (!rebuildRuntime(session, model, error)) {
    return nullptr;
  }
  session->observer = std::make_shared<SessionObserver>(*this, session);
  {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    sessions_[session->id] = session;
  }
  return session;
}

bool AcpServer::rebuildRuntime(const std::shared_ptr<Session>& session, const std::string& model,
                               std::string& error) {
  RuntimeRequest request;
  request.workspace = session->workspace;
  request.model = model;
  request.mcp_servers = session->mcp_servers;

  SessionRuntime runtime;
  if (!factory_ || !factory_(request, runtime, error)) {
    if (error.empty()) {
      error = "The session runtime could not be built";
    }
    return false;
  }
  session->model_backend = runtime.model;
  session->context = runtime.context;
  session->tools = runtime.tools;
  session->system_prompt = runtime.system_prompt;
  session->model = model.empty() ? default_model_ : model;
  installSessionTools(session);
  return true;
}

void AcpServer::installSessionTools(const std::shared_ptr<Session>& session) {
  if (!session->tools) {
    return;
  }
  const std::weak_ptr<Session> weak = session;
  if (client_read_) {
    installReadTool(session, weak);
  }
  if (client_write_) {
    installWriteTool(session, weak);
  }
  if (client_terminal_) {
    installShellTool(session, weak);
  }
  if (client_elicitation_) {
    installAskTool(session, weak);
  }
  if (session->model_backend) {
    session->model_backend->setTools(session->tools->definitionsJson());
  }
}

void AcpServer::installReadTool(const std::shared_ptr<Session>& session, const std::weak_ptr<Session>& weak) {
  tools::ToolDefinition definition;
  definition.name = "read_file";
  definition.description = "Read a file from the workspace through the editor.";
  definition.parameters = tools::stringParameters({{"path", "File path."}}, {"path"});

  session->tools->registerTool(definition, [this, weak](const std::string& arguments) {
    const std::shared_ptr<Session> live = weak.lock();
    if (!live) {
      return tools::ToolResult{false, "The session is gone"};
    }
    const std::string path = toolArgumentsOf(arguments, "path");
    if (path.empty()) {
      return tools::ToolResult{false, "read_file requires path"};
    }
    std::string error;
    const std::string resolved = tools::resolveToolPath(live->workspace, path, error);
    if (resolved.empty()) {
      return tools::ToolResult{false, error};
    }
    std::string content;
    if (!clientReadFile(*live, resolved, content, error)) {
      return tools::ToolResult{false, error};
    }
    return tools::ToolResult{true, content};
  });
}

void AcpServer::installWriteTool(const std::shared_ptr<Session>& session, const std::weak_ptr<Session>& weak) {
  tools::ToolDefinition definition;
  definition.name = "write_file";
  definition.description = "Write a file in the workspace through the editor.";
  definition.parameters = tools::stringParameters({{"path", "File path."}, {"content", "File content."}}, {"path", "content"});

  session->tools->registerTool(definition, [this, weak](const std::string& arguments) {
    const std::shared_ptr<Session> live = weak.lock();
    if (!live) {
      return tools::ToolResult{false, "The session is gone"};
    }
    json::Value args;
    if (!tools::parseToolArguments(arguments, args)) {
      return tools::ToolResult{false, "write_file requires a JSON object"};
    }
    std::string path;
    std::string content;
    if (!tools::toolStringArgument(args, "path", path) || path.empty()) {
      return tools::ToolResult{false, "write_file requires path"};
    }
    if (!tools::toolStringArgument(args, "content", content)) {
      return tools::ToolResult{false, "write_file requires content"};
    }
    std::string error;
    const std::string resolved = tools::resolveToolPath(live->workspace, path, error);
    if (resolved.empty()) {
      return tools::ToolResult{false, error};
    }
    if (!clientWriteFile(*live, resolved, content, error)) {
      return tools::ToolResult{false, error};
    }
    return tools::ToolResult{true, "Wrote " + resolved};
  });
}

void AcpServer::installShellTool(const std::shared_ptr<Session>& session, const std::weak_ptr<Session>& weak) {
  tools::ToolDefinition definition;
  definition.name = "run_shell";
  definition.description = "Run a shell command in the editor's terminal.";
  definition.parameters = tools::stringParameters({{"command", "Command to run."}}, {"command"});

  session->tools->registerTool(definition, [this, weak](const std::string& arguments) {
    const std::shared_ptr<Session> live = weak.lock();
    if (!live) {
      return tools::ToolResult{false, "The session is gone"};
    }
    const std::string command = toolArgumentsOf(arguments, "command");
    if (command.empty()) {
      return tools::ToolResult{false, "run_shell requires command"};
    }
    std::string output;
    bool ok = false;
    std::string error;
    if (!clientRunShell(*live, command, output, ok, error)) {
      return tools::ToolResult{false, error};
    }
    if (!ok) {
      return tools::ToolResult{false, output};
    }
    return tools::ToolResult{true, output};
  });
}

void AcpServer::installAskTool(const std::shared_ptr<Session>& session, const std::weak_ptr<Session>& weak) {
  tools::ToolDefinition definition;
  definition.name = "request_input";
  definition.description =
      "Ask the user a question through the editor. Use this when a decision needs a human. With options, "
      "the answer must be one of them.";
  definition.parameters = json::parse(R"({
    "type": "object",
    "properties": {
      "question": {"type": "string", "description": "Question to ask the user."},
      "options": {
        "type": "array",
        "items": {"type": "string"},
        "description": "Constrained choices. Omit for a free form answer."
      }
    },
    "required": ["question"]
  })");

  session->tools->registerTool(definition, [this, weak](const std::string& arguments) {
    const std::shared_ptr<Session> live = weak.lock();
    if (!live) {
      return tools::ToolResult{false, "The session is gone"};
    }
    json::Value args;
    if (!tools::parseToolArguments(arguments, args)) {
      return tools::ToolResult{false, "request_input requires a JSON object"};
    }
    std::string question;
    if (!tools::toolStringArgument(args, "question", question) || question.empty()) {
      return tools::ToolResult{false, "request_input requires question"};
    }
    std::vector<std::string> options;
    const json::Value* values = args.find("options");
    if (values != nullptr && values->isArray()) {
      for (const json::Value& option : values->array) {
        if (!option.isString() || option.string.empty()) {
          return tools::ToolResult{false, "request_input options must be non-empty strings"};
        }
        options.push_back(option.string);
      }
    }
    std::string answer;
    std::string error;
    if (!clientAsk(*live, question, options, answer, error)) {
      return tools::ToolResult{false, error};
    }
    return tools::ToolResult{true, answer};
  });
}

json::Value AcpServer::configOptions(const Session& session) const {
  json::Value options = arrayValue();
  if (models_.empty()) {
    return options;
  }
  json::Value values = arrayValue();
  for (const std::string& model : models_) {
    json::Value value = objectValue();
    value.object["value"] = stringValue(model);
    value.object["name"] = stringValue(model);
    values.array.push_back(std::move(value));
  }
  json::Value option = objectValue();
  option.object["id"] = stringValue("model");
  option.object["name"] = stringValue("Model");
  option.object["category"] = stringValue("model");
  option.object["type"] = stringValue("select");
  option.object["currentValue"] = stringValue(session.model);
  option.object["options"] = std::move(values);
  options.array.push_back(std::move(option));
  return options;
}

bool AcpServer::readMcpServers(const json::Value& params, std::map<std::string, config::McpServerConfig>& out,
                               jsonrpc::Error& error) const {
  const json::Value* servers = params.find("mcpServers");
  if (servers == nullptr || servers->isNull()) {
    return true;
  }
  if (!servers->isArray()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "mcpServers must be an array";
    return false;
  }
  for (const json::Value& entry : servers->array) {
    std::string name;
    config::McpServerConfig server;
    if (!readMcpServer(entry, name, server, error)) {
      return false;
    }
    out[name] = std::move(server);
  }
  return true;
}

bool AcpServer::readPrompt(const json::Value& params, std::string& text, jsonrpc::Error& error) const {
  const json::Value* prompt = params.find("prompt");
  if (prompt == nullptr || !prompt->isArray() || prompt->array.empty()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "session/prompt requires a prompt array";
    return false;
  }
  std::string result;
  for (const json::Value& block : prompt->array) {
    if (!appendPromptBlock(block, result, error)) {
      return false;
    }
  }
  if (result.empty()) {
    error.code = jsonrpc::kInvalidParams;
    error.message = "The prompt carries no text the agent can read";
    return false;
  }
  text = result;
  return true;
}

bool AcpServer::gate(const std::shared_ptr<Session>& session, const std::string& tool, const std::string& arguments,
                     const std::string& tool_call_id, std::string& denial) {
  model::ToolCall call;
  call.id = tool_call_id;
  call.name = tool;
  call.arguments = arguments;

  emitToolCall(*session, call);
  session->pending_calls.push_back(call);

  if (!session->plan_open) {
    session->plan.clear();
    session->plan_open = true;
  }
  session->plan.push_back(Session::PlanEntry{toolTitle(tool, arguments), "in_progress"});
  emitPlan(*session);

  const bool allowed = autoApproved(tool) || session->always_allowed.contains(tool);
  if (!allowed) {
    std::string reason;
    if (!requestPermission(*session, call, reason)) {
      denial = reason;
      session->plan_open = false;
      if (!session->plan.empty()) {
        session->plan.back().status = "completed";
      }
      failPendingCalls(*session, reason);
      return false;
    }
  }
  emitToolStatus(*session, call.id, "in_progress");
  return true;
}

void AcpServer::sendUpdate(const std::string& session_id, json::Value update) {
  json::Value params = objectValue();
  params.object["sessionId"] = stringValue(session_id);
  params.object["update"] = std::move(update);
  std::string error;
  if (!endpoint_->notify("session/update", params, error)) {
    log::fail("Cannot send a session update: " + error);
  }
}

void AcpServer::emitMessage(const std::string& session_id, const std::string& kind, const std::string& text) {
  if (text.empty()) {
    return;
  }
  json::Value content = objectValue();
  content.object["type"] = stringValue("text");
  content.object["text"] = stringValue(text);

  json::Value update = objectValue();
  update.object["sessionUpdate"] = stringValue(kind);
  update.object["content"] = std::move(content);
  sendUpdate(session_id, std::move(update));
}

void AcpServer::emitCommands(const std::string& session_id) {
  json::Value update = objectValue();
  update.object["sessionUpdate"] = stringValue("available_commands_update");
  // The agent has no slash commands of its own, so the list is empty.
  update.object["availableCommands"] = arrayValue();
  sendUpdate(session_id, std::move(update));
}

void AcpServer::emitToolCall(Session& session, const model::ToolCall& call) {
  json::Value update = objectValue();
  update.object["sessionUpdate"] = stringValue("tool_call");
  update.object["toolCallId"] = stringValue(call.id);
  update.object["title"] = stringValue(toolTitle(call.name, call.arguments));
  update.object["name"] = stringValue(call.name);
  update.object["kind"] = stringValue(toolKind(call.name));
  update.object["status"] = stringValue("pending");
  sendUpdate(session.id, std::move(update));
}

void AcpServer::emitToolStatus(Session& session, const std::string& tool_call_id, const std::string& status) {
  json::Value update = objectValue();
  update.object["sessionUpdate"] = stringValue("tool_call_update");
  update.object["toolCallId"] = stringValue(tool_call_id);
  update.object["status"] = stringValue(status);
  sendUpdate(session.id, std::move(update));
}

void AcpServer::emitPlan(Session& session) {
  json::Value entries = arrayValue();
  for (const Session::PlanEntry& entry : session.plan) {
    json::Value item = objectValue();
    item.object["content"] = stringValue(entry.content);
    item.object["priority"] = stringValue("medium");
    item.object["status"] = stringValue(entry.status);
    entries.array.push_back(std::move(item));
  }
  json::Value update = objectValue();
  update.object["sessionUpdate"] = stringValue("plan");
  update.object["entries"] = std::move(entries);
  sendUpdate(session.id, std::move(update));
}

void AcpServer::finishPendingCalls(Session& session, const std::vector<model::ModelMessage>& messages) {
  for (const model::ToolCall& call : session.pending_calls) {
    std::string output;
    bool failed = false;
    for (const model::ModelMessage& message : messages) {
      if (message.role == "tool" && message.tool_call_id == call.id) {
        output = message.content;
        failed = output.rfind("Error:", 0) == 0;
        break;
      }
    }
    json::Value update = objectValue();
    update.object["sessionUpdate"] = stringValue("tool_call_update");
    update.object["toolCallId"] = stringValue(call.id);
    update.object["status"] = stringValue(failed ? "failed" : "completed");
    if (!output.empty()) {
      json::Value block = objectValue();
      block.object["type"] = stringValue("text");
      block.object["text"] = stringValue(output);
      json::Value item = objectValue();
      item.object["type"] = stringValue("content");
      item.object["content"] = std::move(block);
      json::Value items = arrayValue();
      items.array.push_back(std::move(item));
      update.object["content"] = std::move(items);
    }
    sendUpdate(session.id, std::move(update));
  }
  session.pending_calls.clear();

  for (Session::PlanEntry& entry : session.plan) {
    entry.status = "completed";
  }
  if (!session.plan.empty()) {
    emitPlan(session);
  }
  session.plan.clear();
  session.plan_open = false;
}

void AcpServer::failPendingCalls(Session& session, const std::string& reason) {
  for (const model::ToolCall& call : session.pending_calls) {
    json::Value update = objectValue();
    update.object["sessionUpdate"] = stringValue("tool_call_update");
    update.object["toolCallId"] = stringValue(call.id);
    update.object["status"] = stringValue("failed");
    json::Value block = objectValue();
    block.object["type"] = stringValue("text");
    block.object["text"] = stringValue(reason);
    json::Value item = objectValue();
    item.object["type"] = stringValue("content");
    item.object["content"] = std::move(block);
    json::Value items = arrayValue();
    items.array.push_back(std::move(item));
    update.object["content"] = std::move(items);
    sendUpdate(session.id, std::move(update));
  }
  session.pending_calls.clear();
}

bool AcpServer::requestPermission(Session& session, const model::ToolCall& call, std::string& denial) {
  // The question goes through the interaction channel of Phase D, so the
  // constrained choice shape is the one the rest of the agent already uses.
  const interaction::Request request = permissionRequest(call.name);
  AcpChannel channel(endpoint_, session.id, call.id, [&session]() { return session.cancelled.load(); });

  std::string answer;
  std::string error;
  if (!channel.ask(request, answer, error)) {
    denial = session.cancelled.load() ? "The turn was cancelled" : error;
    return false;
  }

  const PermissionChoice choice = permissionChoiceForLabel(answer);
  if (!choice.granted) {
    denial = "The user did not allow " + call.name;
    return false;
  }
  if (choice.remember) {
    session.always_allowed.insert(call.name);
  }
  return true;
}

void AcpServer::replay(const Session& session) {
  for (const model::ModelMessage& message : session.state.messages) {
    if (message.role == "user") {
      emitMessage(session.id, "user_message_chunk", message.content);
      continue;
    }
    if (message.role == "assistant") {
      emitMessage(session.id, "agent_message_chunk", message.content);
      for (const model::ToolCall& call : message.tool_calls) {
        json::Value update = objectValue();
        update.object["sessionUpdate"] = stringValue("tool_call");
        update.object["toolCallId"] = stringValue(call.id);
        update.object["title"] = stringValue(toolTitle(call.name, call.arguments));
        update.object["name"] = stringValue(call.name);
        update.object["kind"] = stringValue(toolKind(call.name));
        update.object["status"] = stringValue("completed");
        sendUpdate(session.id, std::move(update));
      }
      continue;
    }
    if (message.role == "tool") {
      json::Value block = objectValue();
      block.object["type"] = stringValue("text");
      block.object["text"] = stringValue(message.content);
      json::Value item = objectValue();
      item.object["type"] = stringValue("content");
      item.object["content"] = std::move(block);
      json::Value items = arrayValue();
      items.array.push_back(std::move(item));
      json::Value update = objectValue();
      update.object["sessionUpdate"] = stringValue("tool_call_update");
      update.object["toolCallId"] = stringValue(message.tool_call_id);
      update.object["status"] = stringValue(message.content.rfind("Error:", 0) == 0 ? "failed" : "completed");
      update.object["content"] = std::move(items);
      sendUpdate(session.id, std::move(update));
    }
  }
}

bool AcpServer::clientReadFile(const Session& session, const std::string& path, std::string& content,
                               std::string& error) {
  json::Value params = objectValue();
  params.object["sessionId"] = stringValue(session.id);
  params.object["path"] = stringValue(path);

  json::Value result;
  if (!endpoint_->request("fs/read_text_file", params, result, error, jsonrpc::RequestOptions{kClientTimeoutMs})) {
    return false;
  }
  const json::Value* text = result.find("content");
  if (text == nullptr || !text->isString()) {
    error = "The fs/read_text_file response carries no content";
    return false;
  }
  content = text->string;
  return true;
}

bool AcpServer::clientWriteFile(const Session& session, const std::string& path, const std::string& content,
                                std::string& error) {
  json::Value params = objectValue();
  params.object["sessionId"] = stringValue(session.id);
  params.object["path"] = stringValue(path);
  params.object["content"] = stringValue(content);

  json::Value result;
  if (!endpoint_->request("fs/write_text_file", params, result, error, jsonrpc::RequestOptions{kClientTimeoutMs})) {
    return false;
  }
  return true;
}

bool AcpServer::clientRunShell(const Session& session, const std::string& command, std::string& output, bool& ok,
                               std::string& error) {
  ok = false;
  json::Value args = arrayValue();
  args.array.push_back(stringValue(shellFlag()));
  args.array.push_back(stringValue(command));

  json::Value params = objectValue();
  params.object["sessionId"] = stringValue(session.id);
  params.object["command"] = stringValue(shellProgram());
  params.object["args"] = std::move(args);
  params.object["cwd"] = stringValue(session.workspace);

  json::Value result;
  if (!endpoint_->request("terminal/create", params, result, error, jsonrpc::RequestOptions{kClientTimeoutMs})) {
    return false;
  }
  const json::Value* terminalId = result.find("terminalId");
  if (terminalId == nullptr || !terminalId->isString() || terminalId->string.empty()) {
    error = "The terminal/create response carries no terminalId";
    return false;
  }
  const std::string id = terminalId->string;

  json::Value waitParams = objectValue();
  waitParams.object["sessionId"] = stringValue(session.id);
  waitParams.object["terminalId"] = stringValue(id);
  json::Value waitResult;
  std::string waitError;
  const bool exited =
      endpoint_->request("terminal/wait_for_exit", waitParams, waitResult, waitError,
                         jsonrpc::RequestOptions{kShellTimeoutMs});

  output = terminalOutput(session, id);
  releaseTerminal(session, id);

  if (!exited) {
    error = waitError;
    return false;
  }
  const json::Value* exitCode = waitResult.find("exitCode");
  const int code = exitCode != nullptr && exitCode->isNumber() ? static_cast<int>(exitCode->number) : -1;
  ok = code == 0;
  if (!output.empty()) {
    output.push_back('\n');
  }
  output.append("Exit code: " + std::to_string(code));
  return true;
}

std::string AcpServer::terminalOutput(const Session& session, const std::string& terminal_id) {
  json::Value params = objectValue();
  params.object["sessionId"] = stringValue(session.id);
  params.object["terminalId"] = stringValue(terminal_id);

  json::Value result;
  std::string error;
  if (!endpoint_->request("terminal/output", params, result, error, jsonrpc::RequestOptions{kClientTimeoutMs})) {
    return {};
  }
  const json::Value* output = result.find("output");
  if (output == nullptr || !output->isString()) {
    return {};
  }
  return output->string;
}

void AcpServer::releaseTerminal(const Session& session, const std::string& terminal_id) {
  json::Value params = objectValue();
  params.object["sessionId"] = stringValue(session.id);
  params.object["terminalId"] = stringValue(terminal_id);

  json::Value result;
  std::string error;
  if (!endpoint_->request("terminal/release", params, result, error, jsonrpc::RequestOptions{kClientTimeoutMs})) {
    log::fail("Cannot release the terminal: " + error);
  }
}

bool AcpServer::clientAsk(const Session& session, const std::string& question, const std::vector<std::string>& options,
                          std::string& answer, std::string& error) {
  json::Value answerSchema = objectValue();
  answerSchema.object["type"] = stringValue("string");
  if (!options.empty()) {
    json::Value values = arrayValue();
    for (const std::string& option : options) {
      values.array.push_back(stringValue(option));
    }
    answerSchema.object["enum"] = std::move(values);
  }
  json::Value properties = objectValue();
  properties.object["answer"] = std::move(answerSchema);
  json::Value required = arrayValue();
  required.array.push_back(stringValue("answer"));
  json::Value schema = objectValue();
  schema.object["type"] = stringValue("object");
  schema.object["properties"] = std::move(properties);
  schema.object["required"] = std::move(required);

  json::Value params = objectValue();
  params.object["sessionId"] = stringValue(session.id);
  params.object["mode"] = stringValue("form");
  params.object["message"] = stringValue(question);
  params.object["requestedSchema"] = std::move(schema);

  json::Value result;
  if (!endpoint_->request("elicitation/create", params, result, error, jsonrpc::RequestOptions{kClientTimeoutMs})) {
    return false;
  }
  const json::Value* action = result.find("action");
  if (action == nullptr || !action->isString() || action->string != "accept") {
    error = "The user did not answer the question";
    return false;
  }
  const json::Value* content = result.find("content");
  if (content == nullptr || !content->isObject()) {
    error = "The answer carries no content";
    return false;
  }
  const json::Value* value = content->find("answer");
  if (value == nullptr || !value->isString()) {
    error = "The answer carries no answer";
    return false;
  }
  answer = value->string;
  return true;
}

AcpServer::SessionObserver::SessionObserver(AcpServer& server, std::shared_ptr<Session> session)
    : server_(server), session_(std::move(session)) {}

void AcpServer::SessionObserver::onTurn(const std::vector<model::ModelMessage>& messages, long long turn) {
  server_.finishPendingCalls(*session_, messages);
  session_->state.turns = turn;
  persist(messages, "running", {}, {});
}

void AcpServer::SessionObserver::onAssistantMessage(const std::string& content) {
  server_.emitMessage(session_->id, "agent_message_chunk", content);
}

void AcpServer::SessionObserver::onReasoning(const std::string& reasoning) {
  server_.emitMessage(session_->id, "agent_thought_chunk", reasoning);
}

void AcpServer::SessionObserver::onComplete(const std::vector<model::ModelMessage>& messages,
                                             const std::string& result) {
  server_.failPendingCalls(*session_, result);
  if (!result.empty()) {
    server_.emitMessage(session_->id, "agent_message_chunk", result);
  }
  persist(messages, "complete", result, {});
}

void AcpServer::SessionObserver::onFailure(const std::vector<model::ModelMessage>& messages, const std::string& error) {
  persist(messages, "error", {}, error);
}

void AcpServer::SessionObserver::onInterrupt(const std::vector<model::ModelMessage>& messages) {
  persist(messages, "interrupted", {}, "Interrupted by a shutdown signal");
}

void AcpServer::SessionObserver::persist(const std::vector<model::ModelMessage>& messages, const std::string& status,
                                         const std::string& result, const std::string& error) {
  session::SessionState& state = session_->state;
  state.messages = messages;
  state.status = status;
  state.result = result;
  state.error = error;
  if (!server_.store_) {
    return;
  }
  std::string saveError;
  if (!server_.store_->save(state, saveError)) {
    log::fail("Cannot save the session: " + saveError);
  }
}

}  // namespace acp
}  // namespace bjorne_agent
