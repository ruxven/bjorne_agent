#pragma once

#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cli/Invocation.h"
#include "config/Config.h"
#include "context/ContextManager.h"
#include "context/Fragments.h"
#include "jsonrpc/Endpoint.h"
#include "model/ModelClient.h"
#include "session/Session.h"
#include "tools/Tools.h"

namespace bjorne_agent {
namespace app {

// Builds the model a runtime talks to. The binary builds a real HTTP client
// from the configuration; a test substitutes a scripted backend, so the
// command paths can be covered without a network.
using ModelFactory = std::function<std::shared_ptr<model::ModelBackend>(const config::ModelConfig&)>;

// Builds the stream the ACP server talks to the editor over. The binary uses
// the standard streams; a test uses an in-memory pair, so the ACP command can
// be covered without an editor on the other end.
using TransportFactory = std::function<std::shared_ptr<jsonrpc::Transport>()>;

// The commands the binary dispatches to. Everything that is not the process
// itself arrives through the constructor, so a test can drive the commands
// without the terminal, the network or the session directory of the user.
//
// The class keeps the complexity of each command below the threshold the
// complexity gate uses, by splitting a command into the steps it is made of.
class App {
 public:
  struct Options {
    // Builds the model. An empty factory means the real HTTP client.
    ModelFactory make_model;
    // Where the commands write. Null means the standard streams.
    std::ostream* out = nullptr;
    std::ostream* err = nullptr;
    // Where a task without a command line argument is read from. Null means
    // standard input.
    std::istream* in = nullptr;
    // Builds the ACP transport. An empty factory means the standard streams.
    TransportFactory make_transport;
  };

  App();
  explicit App(Options options);

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Runs the command the invocation names and returns the exit code: 0 when
  // the command succeeded, 1 after an error was reported.
  int run(const cli::Invocation& invocation);

 private:
  // The runtime one session needs: the model, the context manager, the tool
  // set and the fragments for the system prompt.
  struct Runtime {
    context::ContextManagerConfig context_config;
    std::shared_ptr<model::ModelBackend> model;
    std::shared_ptr<context::ContextManager> context;
    std::shared_ptr<tools::ToolRegistry> tools;
    std::vector<context::Fragment> fragments;
  };

  // What one runtime is built for. A one-shot run uses the configured
  // values; an ACP session uses the ones the editor named.
  struct RuntimeSetup {
    std::string workspace;
    std::map<std::string, config::McpServerConfig> mcp_servers;
  };

  // The outcome of looking up the session a resume asked for. A completed
  // session is not a failure: its stored answer is printed and no model runs.
  enum class ResumeAction { Continue, AlreadyComplete, Failed };

  int runTask(const cli::Invocation& invocation);
  int resumeTask(const cli::Invocation& invocation);
  int listSessions();
  int runAcp(const cli::Invocation& invocation);

  // Runs the stored conversation after the state was loaded and repaired.
  int continueSession(const std::string& config_path, const config::Config& config,
                      const config::ModelConfig& selected, const std::shared_ptr<session::SessionStore>& store,
                      session::SessionState& state);

  // Loads the session a resume asked for and reports what to do with it.
  ResumeAction loadResumeSession(const session::SessionStore& store, const std::string& requested,
                                  session::SessionState& state, std::string& error);

  // Loads the config and picks the model. The rest of the run only needs the
  // selected model, so the checks and the progress lines live in one place.
  bool prepareModel(const std::string& config_path, config::Config& config, const config::ModelConfig*& selected);

  // Builds everything a run needs for one workspace, model and MCP server set.
  bool buildRuntimeFor(const std::string& config_path, const config::Config& config,
                       const config::ModelConfig& selected, const RuntimeSetup& setup, Runtime& out);
  bool buildRuntime(const std::string& config_path, const config::Config& config,
                    const config::ModelConfig& selected, Runtime& out);

  // The session directory named by the config, resolved next to the config
  // file.
  bool sessionDirectoryFor(const std::string& config_path, const config::Config& config, std::string& out);
  std::shared_ptr<session::SessionStore> openSessionStore(const std::string& directory);

  // Writes an error to the error stream and the log.
  void reportError(const std::string& message);

  Options options_;
  std::ostream& out_;
  std::ostream& err_;
  std::istream& in_;
};

}  // namespace app
}  // namespace bjorne_agent
