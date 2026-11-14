#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "agent/AgentLoop.h"
#include "cli/Invocation.h"
#include "config/Config.h"
#include "config/ConfigPaths.h"
#include "context/AgentsFiles.h"
#include "context/ContextManager.h"
#include "context/Fragments.h"
#include "log/Log.h"
#include "model/ModelClient.h"
#include "platform/Platform.h"
#include "platform/Shutdown.h"
#include "session/Session.h"
#include "session/SessionRecorder.h"
#include "skills/Skills.h"
#include "tools/Tools.h"

namespace {

std::string readStdin() {
  std::stringstream buffer;
  buffer << std::cin.rdbuf();
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

std::string resolveAgainstConfig(const std::string& configPath, const std::string& path) {
  if (path.empty() || isAbsolutePath(path)) {
    return path;
  }
  const size_t slash = lastPathSeparator(configPath);
  if (slash == std::string::npos) {
    return path;
  }
  return configPath.substr(0, slash + 1) + path;
}

const bjorne_agent::config::ModelConfig* selectModel(const std::vector<bjorne_agent::config::ModelConfig>& models) {
  if (models.empty()) {
    return nullptr;
  }
  const auto best = std::min_element(models.begin(), models.end(),
                                     [](const bjorne_agent::config::ModelConfig& left, const bjorne_agent::config::ModelConfig& right) {
                                       return left.priority < right.priority;
                                     });
  return &*best;
}

// Reads BJORNE_AGENT_LOG into the level. Returns false when the variable holds
// an unknown level.
bool applyLogEnvironment(bjorne_agent::log::Level& level) {
  std::string configured;
  if (!bjorne_agent::platform::getEnvironmentVariable("BJORNE_AGENT_LOG", configured)) {
    return true;
  }
  if (!bjorne_agent::log::parseLevel(configured, level)) {
    std::cerr << "Unknown BJORNE_AGENT_LOG level: " << configured << "\n";
    return false;
  }
  return true;
}

void reportError(const std::string& message) {
  bjorne_agent::log::fail(message);
  std::cerr << message << "\n";
}

// Fills in the context configuration and loads the compression prompt. The
// prompt path is relative to the config file, so it is resolved against the
// config path rather than the working directory.
bool buildContextConfig(const std::string& configPath, const bjorne_agent::config::ModelConfig& model,
                        bjorne_agent::context::ContextManagerConfig& contextConfig) {
  contextConfig.context_window = model.context_window;
  contextConfig.max_tokens = model.max_tokens;
  if (model.compression_prompt_path.empty()) {
    return true;
  }

  std::string prompt;
  const std::string promptPath = resolveAgainstConfig(configPath, model.compression_prompt_path);
  bjorne_agent::log::detail("Compression prompt: " + promptPath);
  if (!readTextFile(promptPath, prompt)) {
    reportError("Cannot open compression prompt: " + promptPath);
    return false;
  }
  contextConfig.compression_prompt = prompt;
  return true;
}

// Builds the callback that asks the model to summarize a conversation when
// the context no longer fits.
bjorne_agent::context::CompressionHandler makeCompressionHandler(const std::shared_ptr<bjorne_agent::model::ModelClient>& model) {
  return [model](const std::vector<bjorne_agent::model::ModelMessage>& messages, const std::string& instruction,
                 std::string& summary, std::string& handlerError) {
    std::vector<bjorne_agent::model::ModelMessage> request;
    bjorne_agent::model::ModelMessage system;
    system.role = "system";
    system.content = instruction;
    request.push_back(system);
    request.insert(request.end(), messages.begin(), messages.end());
    bjorne_agent::model::ModelResponse response;
    bjorne_agent::log::progress("Compressing context");
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
std::string buildSystemPrompt(const std::string& base, const std::vector<bjorne_agent::context::Fragment>& fragments) {
  std::string prompt = base;
  const std::string assembled = bjorne_agent::context::renderFragments(fragments);
  if (assembled.empty()) {
    return prompt;
  }
  if (!prompt.empty()) {
    prompt += "\n\n";
  }
  prompt += assembled;
  return prompt;
}

// Loads the config and picks the model. The rest of the run only needs the
// selected model, so the checks and the progress lines live in one place.
bool prepareModel(const std::string& configPath, bjorne_agent::config::Config& config,
                  const bjorne_agent::config::ModelConfig*& selected) {
  std::string error;
  bjorne_agent::log::progress("Loading config: " + configPath);
  if (!bjorne_agent::config::load(configPath, config, error)) {
    reportError(error);
    return false;
  }

  selected = selectModel(config.models);
  if (selected == nullptr) {
    reportError("Config has no models");
    return false;
  }
  bjorne_agent::log::progress("Selected model: " + selected->model + " at " + selected->endpoint);
  bjorne_agent::log::detail("API key comes from environment variable: " + selected->api_key_env);
  bjorne_agent::log::detail("Workspace: " + config.agent.workspace);
  if (selected->ignore_self_signed) {
    bjorne_agent::log::progress("TLS peer verification is off (ignore_self_signed is set)");
  }
  if (!selected->hostname.empty()) {
    bjorne_agent::log::detail("TLS server name: " + selected->hostname);
  }
  return true;
}

// Everything the loop needs that is built from the configuration: the model,
// context manager, tool set, and the fragments for the system prompt. Both a
// fresh run and a resume build this, so the tool set is identical.
struct Runtime {
  bjorne_agent::context::ContextManagerConfig context_config;
  std::shared_ptr<bjorne_agent::model::ModelClient> model;
  std::shared_ptr<bjorne_agent::context::ContextManager> context;
  std::shared_ptr<bjorne_agent::tools::ToolRegistry> tools;
  std::vector<bjorne_agent::context::Fragment> fragments;
};

bool buildRuntime(const std::string& configPath, const bjorne_agent::config::Config& config,
                  const bjorne_agent::config::ModelConfig& selected, Runtime& out) {
  if (!buildContextConfig(configPath, selected, out.context_config)) {
    return false;
  }

  // Skills and AGENTS.md are assembled before the model client, because the
  // system prompt they produce is part of the request and load_skill has to be
  // in the tool set from the first turn.
  const std::string skillsDirectory = resolveAgainstConfig(configPath, config.agent.skills_path);
  const bjorne_agent::skills::SkillLoad skillLoad = bjorne_agent::skills::loadSkills(skillsDirectory);
  if (!skillsDirectory.empty() && !skillLoad.directory_found) {
    bjorne_agent::log::fail("Skills directory not found: " + skillsDirectory);
  }
  for (const std::string& skipped : skillLoad.skipped) {
    bjorne_agent::log::fail("Skipping skill " + skipped);
  }
  for (const bjorne_agent::skills::Skill& skill : skillLoad.skills) {
    bjorne_agent::log::detail("Skill: " + skill.name);
  }

  const bjorne_agent::context::AgentsFiles agents =
      bjorne_agent::context::loadAgentsFiles(config.agent.workspace, bjorne_agent::config::userConfigDirectory());
  for (const std::string& path : agents.unreadable) {
    bjorne_agent::log::fail("Cannot read " + path);
  }
  for (const bjorne_agent::context::Fragment& fragment : agents.fragments) {
    bjorne_agent::log::detail("Context: " + fragment.title);
  }

  if (!skillLoad.skills.empty()) {
    out.fragments.push_back(bjorne_agent::skills::skillIndexFragment(skillLoad.skills));
  }
  out.fragments.insert(out.fragments.end(), agents.fragments.begin(), agents.fragments.end());

  bjorne_agent::model::ModelClientOptions modelOptions;
  modelOptions.endpoint = selected.endpoint;
  modelOptions.model = selected.model;
  modelOptions.api_key_env = selected.api_key_env;
  modelOptions.ignore_self_signed = selected.ignore_self_signed;
  modelOptions.hostname = selected.hostname;
  out.model = std::make_shared<bjorne_agent::model::ModelClient>(std::move(modelOptions));

  out.context =
      std::make_shared<bjorne_agent::context::ContextManager>(out.context_config, makeCompressionHandler(out.model));
  out.tools = std::make_shared<bjorne_agent::tools::ToolRegistry>(config.agent.workspace);
  if (!skillLoad.skills.empty()) {
    auto skillCatalog = std::make_shared<std::vector<bjorne_agent::skills::Skill>>(skillLoad.skills);
    bjorne_agent::skills::registerLoadSkillTool(*out.tools, skillCatalog);
  }
  out.model->setTools(out.tools->definitionsJson());
  return true;
}

// The session directory named by the config, resolved next to the config file.
bool sessionDirectoryFor(const std::string& configPath, const bjorne_agent::config::Config& config, std::string& out) {
  if (config.agent.session_dir.empty()) {
    reportError("No session_dir is configured; session storage is disabled");
    return false;
  }
  out = resolveAgainstConfig(configPath, config.agent.session_dir);
  return true;
}

std::shared_ptr<bjorne_agent::session::SessionStore> openSessionStore(const std::string& directory) {
  std::string error;
  std::shared_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(directory, error);
  if (!store) {
    reportError(error);
  }
  return store;
}

std::string shortened(const std::string& text, std::size_t max) {
  if (text.size() <= max) {
    return text;
  }
  return text.substr(0, max) + "...";
}

// Runs one task: loads the config, builds the model, context and tools, and
// prints the final answer. Returns 0 on success and 1 after reporting an error.
int runTask(const bjorne_agent::cli::Invocation& invocation) {
  std::string error;
  const std::string& configPath = invocation.configPath;
  bjorne_agent::config::Config config;
  const bjorne_agent::config::ModelConfig* selected = nullptr;
  if (!prepareModel(configPath, config, selected)) {
    return 1;
  }

  // The task comes from the command line when one was given, otherwise from
  // standard input.
  std::string task = invocation.taskFromArgument ? invocation.task : readStdin();
  if (task.empty()) {
    std::cerr << bjorne_agent::cli::usage();
    return 1;
  }
  bjorne_agent::log::progress("Task: " + task);

  Runtime runtime;
  if (!buildRuntime(configPath, config, *selected, runtime)) {
    return 1;
  }

  // With a session directory the run creates a record before the first model
  // call and rewrites it after each turn. Without one the run stays one-shot.
  std::shared_ptr<bjorne_agent::session::SessionStore> store;
  std::shared_ptr<bjorne_agent::session::SessionRecorder> recorder;
  if (!config.agent.session_dir.empty()) {
    std::string directory;
    if (!sessionDirectoryFor(configPath, config, directory)) {
      return 1;
    }
    store = openSessionStore(directory);
    if (!store) {
      return 1;
    }

    bjorne_agent::session::SessionState state;
    state.task = task;
    state.workspace = config.agent.workspace;
    state.model = selected->model;
    if (!store->begin(state, error)) {
      reportError(error);
      return 1;
    }
    bjorne_agent::log::progress("Session: " + state.id);
    recorder = std::make_shared<bjorne_agent::session::SessionRecorder>(store, std::move(state));
  }

  bjorne_agent::agent::AgentLoopConfig loopConfig;
  loopConfig.system_prompt = buildSystemPrompt(config.agent.system_prompt, runtime.fragments);
  loopConfig.workspace = config.agent.workspace;
  loopConfig.stop_requested = [] { return bjorne_agent::platform::shutdownRequested(); };
  bjorne_agent::agent::AgentLoop loop(loopConfig, runtime.model, runtime.context, runtime.tools, recorder);

  std::string result;
  if (!loop.run(task, result, error)) {
    reportError(error);
    return 1;
  }
  bjorne_agent::log::progress("Done");
  std::cout << result << "\n";
  return 0;
}

// The outcome of looking up the session a resume asked for. A completed
// session is not a failure: its stored answer is printed and no model runs.
enum class ResumeAction { Continue, AlreadyComplete, Failed };

ResumeAction loadResumeSession(const bjorne_agent::session::SessionStore& store, const std::string& requested,
                               bjorne_agent::session::SessionState& state, std::string& error) {
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
    bjorne_agent::log::progress("Session " + id + " is already complete");
    std::cout << state.result << "\n";
    return ResumeAction::AlreadyComplete;
  }
  return ResumeAction::Continue;
}

// Continues a stored session. The stored system message is refreshed from the
// current config, and an unfinished tool call gets a synthetic result so the
// sequence is valid again.
int resumeTask(const bjorne_agent::cli::Invocation& invocation) {
  std::string error;
  std::string configPath = invocation.configPath;
  if (configPath.empty()) {
    configPath = bjorne_agent::config::findDefaultConfig(error);
    if (configPath.empty()) {
      reportError(error);
      return 1;
    }
  }

  bjorne_agent::config::Config config;
  const bjorne_agent::config::ModelConfig* selected = nullptr;
  if (!prepareModel(configPath, config, selected)) {
    return 1;
  }

  std::string directory;
  if (!sessionDirectoryFor(configPath, config, directory)) {
    return 1;
  }
  const std::shared_ptr<bjorne_agent::session::SessionStore> store = openSessionStore(directory);
  if (!store) {
    return 1;
  }

  bjorne_agent::session::SessionState state;
  const ResumeAction action = loadResumeSession(*store, invocation.session, state, error);
  if (action == ResumeAction::Failed) {
    reportError(error);
    return 1;
  }
  if (action == ResumeAction::AlreadyComplete) {
    return 0;
  }

  const std::size_t repaired = bjorne_agent::session::repairDanglingToolCalls(state.messages);
  if (repaired > 0) {
    bjorne_agent::log::progress("Repaired " + std::to_string(repaired) + " unfinished tool call(s)");
  }
  state.status = "running";
  state.error.clear();

  Runtime runtime;
  if (!buildRuntime(configPath, config, *selected, runtime)) {
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

  auto recorder = std::make_shared<bjorne_agent::session::SessionRecorder>(store, state);
  bjorne_agent::agent::AgentLoopConfig loopConfig;
  loopConfig.system_prompt = systemPrompt;
  loopConfig.workspace = config.agent.workspace;
  loopConfig.history = state.messages;
  loopConfig.stop_requested = [] { return bjorne_agent::platform::shutdownRequested(); };
  bjorne_agent::agent::AgentLoop loop(loopConfig, runtime.model, runtime.context, runtime.tools, recorder);

  std::string result;
  if (!loop.run("", result, error)) {
    reportError(error);
    return 1;
  }
  bjorne_agent::log::progress("Done");
  std::cout << result << "\n";
  return 0;
}

int listSessions() {
  std::string error;
  const std::string configPath = bjorne_agent::config::findDefaultConfig(error);
  if (configPath.empty()) {
    reportError(error);
    return 1;
  }

  bjorne_agent::config::Config config;
  if (!bjorne_agent::config::load(configPath, config, error)) {
    reportError(error);
    return 1;
  }

  std::string directory;
  if (!sessionDirectoryFor(configPath, config, directory)) {
    return 1;
  }
  const std::shared_ptr<bjorne_agent::session::SessionStore> store = openSessionStore(directory);
  if (!store) {
    return 1;
  }

  const std::vector<bjorne_agent::session::SessionSummary> summaries = store->list(error);
  if (!error.empty()) {
    reportError(error);
    return 1;
  }
  if (summaries.empty()) {
    std::cout << "No stored sessions.\n";
    return 0;
  }

  for (const bjorne_agent::session::SessionSummary& summary : summaries) {
    std::cout << bjorne_agent::session::formatTimestamp(summary.updated) << "  " << summary.status << "  "
              << summary.id << "  " << shortened(summary.task, 80) << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bjorne_agent::platform::installShutdownHandlers();

  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  bjorne_agent::cli::Invocation invocation;
  std::string error;
  if (!bjorne_agent::cli::parse(arguments, invocation, error)) {
    bjorne_agent::log::fail(error);
    std::cerr << error << "\n";
    if (invocation.showUsage) {
      std::cerr << "\n" << bjorne_agent::cli::usage();
    }
    return 1;
  }

  if (invocation.help) {
    if (invocation.commandSet) {
      std::cout << bjorne_agent::cli::usage(invocation.command);
    } else {
      std::cout << bjorne_agent::cli::usage();
    }
    return 0;
  }

  // A command line flag wins over the environment variable. The library starts
  // quiet so that embedding it produces no output; the binary defaults to
  // normal so the user sees what it is doing.
  bjorne_agent::log::Level level = invocation.levelSet ? invocation.level : bjorne_agent::log::Level::Normal;
  if (!invocation.levelSet && !applyLogEnvironment(level)) {
    return 1;
  }
  bjorne_agent::log::setLevel(level);

  switch (invocation.command) {
    case bjorne_agent::cli::Command::Run:
      return runTask(invocation);
    case bjorne_agent::cli::Command::Sessions:
      return listSessions();
    case bjorne_agent::cli::Command::Resume:
      return resumeTask(invocation);
    case bjorne_agent::cli::Command::Init:
      break;
  }

  std::cerr << "The '" << bjorne_agent::cli::commandName(invocation.command) << "' command is not implemented yet.\n";
  return 1;
}
