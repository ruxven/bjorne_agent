#include "app/App.h"

#include "acp/AcpServer.h"
#include "acp/StdioTransport.h"
#include "agent/AgentLoop.h"
#include "app/Init.h"
#include "config/ConfigPaths.h"
#include "context/AgentsFiles.h"
#include "interaction/Interaction.h"
#include "log/Log.h"
#include "mcp/McpClient.h"
#include "platform/Platform.h"
#include "platform/Shutdown.h"
#include "session/SessionRecorder.h"
#include "skills/Skills.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace bjorne_agent {
namespace app {

namespace {

std::string readAll(std::istream& input) {
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool readTextFile(const std::string& path, std::string& out) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

// Windows accepts either separator; POSIX has only the forward slash, and a
// backslash is an ordinary character in a POSIX file name.
bool isPathSeparator(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

// True for a path that starts at the root: /dir on POSIX, C:\dir or \\server
// on Windows.
bool isAbsolutePath(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (isPathSeparator(path.front())) {
    return true;
  }
#if defined(_WIN32)
  if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':' &&
      isPathSeparator(path[2])) {
    return true;
  }
#endif
  return false;
}

size_t lastPathSeparator(const std::string& path) {
#if defined(_WIN32)
  return path.find_last_of("/\\");
#else
  return path.find_last_of('/');
#endif
}

// A relative path in the configuration is resolved against the directory of
// the config file, so a config file can be moved together with what it names.
std::string resolveAgainstConfig(const std::string& config_path, const std::string& path) {
  if (path.empty() || isAbsolutePath(path)) {
    return path;
  }
  const size_t slash = lastPathSeparator(config_path);
  if (slash == std::string::npos) {
    return path;
  }
  return config_path.substr(0, slash + 1) + path;
}

// The system prompt may live in a file next to the config. The file wins over
// the text in the config, so the prompt can be edited without touching the
// JSON.
bool applySystemPromptFile(const std::string& config_path, config::Config& config, std::string& error) {
  if (config.agent.system_prompt_path.empty()) {
    return true;
  }
  const std::string path = resolveAgainstConfig(config_path, config.agent.system_prompt_path);
  log::detail("System prompt: " + path);
  std::string prompt;
  if (!readTextFile(path, prompt)) {
    error = "Cannot open system prompt: " + path;
    return false;
  }
  config.agent.system_prompt = prompt;
  return true;
}

// The model with the lowest priority number wins.
const config::ModelConfig* selectModel(const std::vector<config::ModelConfig>& models) {
  if (models.empty()) {
    return nullptr;
  }
  const auto best = std::min_element(models.begin(), models.end(),
                                     [](const config::ModelConfig& left, const config::ModelConfig& right) {
                                       return left.priority < right.priority;
                                     });
  return &*best;
}

// Fills in the context configuration and loads the compression prompt. The
// prompt path is relative to the config file, so it is resolved against the
// config path rather than the working directory.
bool buildContextConfig(const std::string& config_path, const config::ModelConfig& model,
                        context::ContextManagerConfig& contextConfig, std::string& error) {
  contextConfig.context_window = model.context_window;
  contextConfig.max_tokens = model.max_tokens;
  if (model.compression_prompt_path.empty()) {
    return true;
  }

  std::string prompt;
  const std::string promptPath = resolveAgainstConfig(config_path, model.compression_prompt_path);
  log::detail("Compression prompt: " + promptPath);
  if (!readTextFile(promptPath, prompt)) {
    error = "Cannot open compression prompt: " + promptPath;
    return false;
  }
  contextConfig.compression_prompt = prompt;
  return true;
}

// Builds the callback that asks the model to summarize a conversation when
// the context no longer fits.
context::CompressionHandler makeCompressionHandler(const std::shared_ptr<model::ModelBackend>& model) {
  return [model](const std::vector<model::ModelMessage>& messages, const std::string& instruction,
                 std::string& summary, std::string& handlerError) {
    std::vector<model::ModelMessage> request;
    model::ModelMessage system;
    system.role = "system";
    system.content = instruction;
    request.push_back(system);
    request.insert(request.end(), messages.begin(), messages.end());
    model::ModelResponse response;
    log::progress("Compressing context");
    if (!model->respond(request, response, handlerError)) {
      return false;
    }
    summary = response.content;
    return true;
  };
}

// Builds the system prompt from the configured text and the assembled
// fragments. Skills come first and the nearest AGENTS.md last, so the most
// specific instruction is the last thing the model reads.
std::string buildSystemPrompt(const std::string& base, const std::vector<context::Fragment>& fragments) {
  std::string prompt = base;
  const std::string assembled = context::renderFragments(fragments);
  if (assembled.empty()) {
    return prompt;
  }
  if (!prompt.empty()) {
    prompt += "\n\n";
  }
  prompt += assembled;
  return prompt;
}

std::string shortened(const std::string& text, std::size_t max) {
  if (text.size() <= max) {
    return text;
  }
  return text.substr(0, max) + "...";
}

// The real model client, used when no factory was supplied.
std::shared_ptr<model::ModelBackend> makeRealModel(const config::ModelConfig& selected) {
  model::ModelClientOptions modelOptions;
  modelOptions.endpoint = selected.endpoint;
  modelOptions.model = selected.model;
  modelOptions.api_key_env = selected.api_key_env;
  modelOptions.ignore_self_signed = selected.ignore_self_signed;
  modelOptions.hostname = selected.hostname;
  return std::make_shared<model::ModelClient>(std::move(modelOptions));
}

}  // namespace

App::App() : App(Options{}) {}

App::App(Options options)
    : options_(std::move(options)),
      out_(options_.out != nullptr ? *options_.out : std::cout),
      err_(options_.err != nullptr ? *options_.err : std::cerr),
      in_(options_.in != nullptr ? *options_.in : std::cin),
      input_is_terminal_(options_.input_is_terminal.value_or(platform::standardInputIsTerminal())) {}

void App::reportError(const std::string& message) {
  log::fail(message);
  err_ << message << "\n";
}

bool App::prepareModel(const std::string& config_path, config::Config& config,
                       const config::ModelConfig*& selected) {
  std::string error;
  log::progress("Loading config: " + config_path);
  if (!config::load(config_path, config, error)) {
    reportError(error);
    return false;
  }
  if (!applySystemPromptFile(config_path, config, error)) {
    reportError(error);
    return false;
  }

  selected = selectModel(config.models);
  if (selected == nullptr) {
    reportError("Config has no models");
    return false;
  }
  log::progress("Selected model: " + selected->model + " at " + selected->endpoint);
  log::detail("API key comes from environment variable: " + selected->api_key_env);
  log::detail("Workspace: " + config.agent.workspace);
  if (selected->ignore_self_signed) {
    log::progress("TLS peer verification is off (ignore_self_signed is set)");
  }
  if (!selected->hostname.empty()) {
    log::detail("TLS server name: " + selected->hostname);
  }
  return true;
}

bool App::buildRuntimeFor(const std::string& config_path, const config::Config& config,
                          const config::ModelConfig& selected, const RuntimeSetup& setup, Runtime& out) {
  std::string error;
  if (!buildContextConfig(config_path, selected, out.context_config, error)) {
    reportError(error);
    return false;
  }

  // Skills and AGENTS.md are assembled before the model client, because the
  // system prompt they produce is part of the request and load_skill has to be
  // in the tool set from the first turn.
  const std::string skillsDirectory = resolveAgainstConfig(config_path, config.agent.skills_path);
  const skills::SkillLoad skillLoad = skills::loadSkills(skillsDirectory);
  if (!skillsDirectory.empty() && !skillLoad.directory_found) {
    log::fail("Skills directory not found: " + skillsDirectory);
  }
  for (const std::string& skipped : skillLoad.skipped) {
    log::fail("Skipping skill " + skipped);
  }
  for (const skills::Skill& skill : skillLoad.skills) {
    log::detail("Skill: " + skill.name);
  }

  const context::AgentsFiles agents = context::loadAgentsFiles(setup.workspace, config::userConfigDirectory());
  for (const std::string& path : agents.unreadable) {
    log::fail("Cannot read " + path);
  }
  for (const context::Fragment& fragment : agents.fragments) {
    log::detail("Context: " + fragment.title);
  }

  if (!skillLoad.skills.empty()) {
    out.fragments.push_back(skills::skillIndexFragment(skillLoad.skills));
  }
  out.fragments.insert(out.fragments.end(), agents.fragments.begin(), agents.fragments.end());

  out.model = options_.make_model ? options_.make_model(selected) : makeRealModel(selected);

  out.context = std::make_shared<context::ContextManager>(out.context_config, makeCompressionHandler(out.model));
  out.tools = std::make_shared<tools::ToolRegistry>(setup.workspace);
  interaction::registerRequestInputTool(*out.tools, interaction::makeChannel(config.agent.interactive));
  if (!skillLoad.skills.empty()) {
    auto skillCatalog = std::make_shared<std::vector<skills::Skill>>(skillLoad.skills);
    skills::registerLoadSkillTool(*out.tools, skillCatalog);
  }
  // MCP tools are registered last, so they sit alongside the built-ins in the
  // same namespace without being able to replace one: the namespaced name
  // cannot collide with a built-in name.
  mcp::registerServers(setup.mcp_servers, *out.tools);
  out.model->setTools(out.tools->definitionsJson());
  return true;
}

bool App::buildRuntime(const std::string& config_path, const config::Config& config,
                       const config::ModelConfig& selected, Runtime& out) {
  RuntimeSetup setup;
  setup.workspace = config.agent.workspace;
  setup.mcp_servers = config.mcp_servers;
  return buildRuntimeFor(config_path, config, selected, setup, out);
}

bool App::sessionDirectoryFor(const std::string& config_path, const config::Config& config, std::string& out) {
  if (config.agent.session_dir.empty()) {
    reportError("No session_dir is configured; session storage is disabled");
    return false;
  }
  out = resolveAgainstConfig(config_path, config.agent.session_dir);
  return true;
}

std::shared_ptr<session::SessionStore> App::openSessionStore(const std::string& directory) {
  std::string error;
  std::shared_ptr<session::SessionStore> store = session::SessionStore::open(directory, error);
  if (!store) {
    reportError(error);
  }
  return store;
}

int App::run(const cli::Invocation& invocation) {
  switch (invocation.command) {
    case cli::Command::Run:
      return runTask(invocation);
    case cli::Command::Sessions:
      return listSessions();
    case cli::Command::Resume:
      return resumeTask(invocation);
    case cli::Command::Acp:
      return runAcp(invocation);
    case cli::Command::Init:
      return runInit();
  }

  // Every command is handled above. The switch covers every value of the
  // enumeration, so a new one without a case is a compiler warning rather
  // than a silent fall through.
  return 1;
}

int App::runInit() {
  // The wizard asks its questions on the terminal, so the answer stream is the
  // standard input when it is one.
  Init init(in_, out_, input_is_terminal_);
  return init.run();
}

int App::runTask(const cli::Invocation& invocation) {
  std::string error;
  const std::string& configPath = invocation.configPath;
  config::Config config;
  const config::ModelConfig* selected = nullptr;
  if (!prepareModel(configPath, config, selected)) {
    return 1;
  }

  // The task comes from the command line when one was given, otherwise from
  // standard input.
  std::string task = invocation.taskFromArgument ? invocation.task : readAll(in_);
  if (task.empty()) {
    err_ << cli::usage();
    return 1;
  }
  log::progress("Task: " + task);

  Runtime runtime;
  if (!buildRuntime(configPath, config, *selected, runtime)) {
    return 1;
  }

  // With a session directory the run creates a record before the first model
  // call and rewrites it after each turn. Without one the run stays one-shot.
  std::shared_ptr<session::SessionStore> store;
  std::shared_ptr<session::SessionRecorder> recorder;
  if (!config.agent.session_dir.empty()) {
    std::string directory;
    if (!sessionDirectoryFor(configPath, config, directory)) {
      return 1;
    }
    store = openSessionStore(directory);
    if (!store) {
      return 1;
    }

    session::SessionState state;
    state.task = task;
    state.workspace = config.agent.workspace;
    state.model = selected->model;
    if (!store->begin(state, error)) {
      reportError(error);
      return 1;
    }
    log::progress("Session: " + state.id);
    recorder = std::make_shared<session::SessionRecorder>(store, std::move(state));
  }

  agent::AgentLoopConfig loopConfig;
  loopConfig.system_prompt = buildSystemPrompt(config.agent.system_prompt, runtime.fragments);
  loopConfig.workspace = config.agent.workspace;
  loopConfig.stop_requested = [] { return platform::shutdownRequested(); };
  agent::AgentLoop loop(loopConfig, runtime.model, runtime.context, runtime.tools, recorder);

  std::string result;
  if (!loop.run(task, result, error)) {
    reportError(error);
    return 1;
  }
  log::progress("Done");
  out_ << result << "\n";
  return 0;
}

App::ResumeAction App::loadResumeSession(const session::SessionStore& store, const std::string& requested,
                                         session::SessionState& state, std::string& error) {
  std::string id = requested;
  if (id.empty()) {
    id = store.latest(error);
    if (id.empty()) {
      if (error.empty()) {
        error = "No stored sessions";
      }
      return ResumeAction::Failed;
    }
  }

  if (!store.load(id, state, error)) {
    return ResumeAction::Failed;
  }
  if (state.status == "complete") {
    log::progress("Session " + id + " is already complete");
    out_ << state.result << "\n";
    return ResumeAction::AlreadyComplete;
  }
  return ResumeAction::Continue;
}

int App::continueSession(const std::string& config_path, const config::Config& config,
                         const config::ModelConfig& selected, const std::shared_ptr<session::SessionStore>& store,
                         session::SessionState& state) {
  std::string error;
  const std::size_t repaired = session::repairDanglingToolCalls(state.messages);
  if (repaired > 0) {
    log::progress("Repaired " + std::to_string(repaired) + " unfinished tool call(s)");
  }
  state.status = "running";
  state.error.clear();

  Runtime runtime;
  if (!buildRuntime(config_path, config, selected, runtime)) {
    return 1;
  }

  // The stored conversation keeps its own system message. Replacing its text
  // lets AGENTS.md and skill changes take effect on the resumed session.
  const std::string systemPrompt = buildSystemPrompt(config.agent.system_prompt, runtime.fragments);
  if (!systemPrompt.empty() && !state.messages.empty() && state.messages.front().role == "system") {
    state.messages.front().content = systemPrompt;
  }

  // Persist the repaired conversation before the first model call, so another
  // teardown during the resume does not repeat the repair from a stale file.
  if (!store->save(state, error)) {
    reportError(error);
    return 1;
  }

  auto recorder = std::make_shared<session::SessionRecorder>(store, state);
  agent::AgentLoopConfig loopConfig;
  loopConfig.system_prompt = systemPrompt;
  loopConfig.workspace = config.agent.workspace;
  loopConfig.history = state.messages;
  loopConfig.stop_requested = [] { return platform::shutdownRequested(); };
  agent::AgentLoop loop(loopConfig, runtime.model, runtime.context, runtime.tools, recorder);

  std::string result;
  if (!loop.run("", result, error)) {
    reportError(error);
    return 1;
  }
  log::progress("Done");
  out_ << result << "\n";
  return 0;
}

int App::resumeTask(const cli::Invocation& invocation) {
  std::string error;
  std::string configPath = invocation.configPath;
  if (configPath.empty()) {
    configPath = config::findDefaultConfig(error);
    if (configPath.empty()) {
      reportError(error);
      return 1;
    }
  }

  config::Config config;
  const config::ModelConfig* selected = nullptr;
  if (!prepareModel(configPath, config, selected)) {
    return 1;
  }

  std::string directory;
  if (!sessionDirectoryFor(configPath, config, directory)) {
    return 1;
  }
  const std::shared_ptr<session::SessionStore> store = openSessionStore(directory);
  if (!store) {
    return 1;
  }

  session::SessionState state;
  const ResumeAction action = loadResumeSession(*store, invocation.session, state, error);
  if (action == ResumeAction::Failed) {
    reportError(error);
    return 1;
  }
  if (action == ResumeAction::AlreadyComplete) {
    return 0;
  }

  return continueSession(configPath, config, *selected, store, state);
}

int App::listSessions() {
  std::string error;
  const std::string configPath = config::findDefaultConfig(error);
  if (configPath.empty()) {
    reportError(error);
    return 1;
  }

  config::Config config;
  if (!config::load(configPath, config, error)) {
    reportError(error);
    return 1;
  }

  std::string directory;
  if (!sessionDirectoryFor(configPath, config, directory)) {
    return 1;
  }
  const std::shared_ptr<session::SessionStore> store = openSessionStore(directory);
  if (!store) {
    return 1;
  }

  const std::vector<session::SessionSummary> summaries = store->list(error);
  if (!error.empty()) {
    reportError(error);
    return 1;
  }
  if (summaries.empty()) {
    out_ << "No stored sessions.\n";
    return 0;
  }

  for (const session::SessionSummary& summary : summaries) {
    out_ << session::formatTimestamp(summary.updated) << "  " << summary.status << "  " << summary.id << "  "
         << shortened(summary.task, 80) << "\n";
  }
  return 0;
}

int App::runAcp(const cli::Invocation& invocation) {
  std::string error;
  const std::string& configPath = invocation.configPath;
  config::Config config;
  const config::ModelConfig* selected = nullptr;
  if (!prepareModel(configPath, config, selected)) {
    return 1;
  }

  // A session store is optional. Without one the agent serves the editor but
  // cannot list, load or resume a conversation.
  std::shared_ptr<session::SessionStore> store;
  if (!config.agent.session_dir.empty()) {
    std::string directory;
    if (!sessionDirectoryFor(configPath, config, directory)) {
      return 1;
    }
    store = openSessionStore(directory);
    if (!store) {
      return 1;
    }
  }

  // The editor can pick the model of a session, so every configured model is
  // offered by name.
  std::vector<std::string> models;
  std::map<std::string, config::ModelConfig> byName;
  for (const config::ModelConfig& model : config.models) {
    models.push_back(model.model);
    byName[model.model] = model;
  }

  // Each session builds its own runtime, because the editor names the
  // workspace, the model and the MCP servers it wants.
  const auto factory = [this, &config, &configPath, &byName, selected](const acp::RuntimeRequest& request,
                                                                 acp::SessionRuntime& runtime,
                                                                 std::string& factoryError) {
    const config::ModelConfig* model = selected;
    if (!request.model.empty()) {
      auto entry = byName.find(request.model);
      if (entry != byName.end()) {
        model = &entry->second;
      }
    }
    // The servers the editor named win over the configured ones by name.
    std::map<std::string, config::McpServerConfig> servers = config.mcp_servers;
    for (const auto& [name, server] : request.mcp_servers) {
      servers[name] = server;
    }

    RuntimeSetup setup;
    setup.workspace = request.workspace;
    setup.mcp_servers = servers;

    Runtime built;
    if (!buildRuntimeFor(configPath, config, *model, setup, built)) {
      factoryError = "The session runtime could not be built";
      return false;
    }
    runtime.model = built.model;
    runtime.context = built.context;
    runtime.tools = built.tools;
    runtime.system_prompt = buildSystemPrompt(config.agent.system_prompt, built.fragments);
    return true;
  };

  auto transport = options_.make_transport ? options_.make_transport() : std::make_shared<acp::StdioTransport>();
  auto endpoint = std::make_shared<jsonrpc::Endpoint>(transport);
  auto server = std::make_shared<acp::AcpServer>(endpoint, store, factory, models, selected->model);
  endpoint->setRequestHandler([server](const std::string& method, const json::Value& params,
                                       json::Value& result, jsonrpc::Error& error) {
    return server->handle(method, params, result, error);
  });
  endpoint->setNotificationHandler([server](const std::string& method, const json::Value& params) {
    server->notify(method, params);
  });

  log::progress("Serving the Agent Client Protocol");
  endpoint->start();
  // The connection ends when the editor closes it or when the process is
  // asked to stop.
  while (!endpoint->closed() && !platform::shutdownRequested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  log::progress("Connection closed");
  return 0;
}

}  // namespace app
}  // namespace bjorne_agent
