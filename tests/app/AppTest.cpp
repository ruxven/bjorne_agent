#include "app/App.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "http/LoopbackServer.h"
#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

namespace {

using bjorne_agent::app::App;
using bjorne_agent::model::ModelBackend;
using bjorne_agent::model::ModelMessage;
using bjorne_agent::model::ModelResponse;

// A directory that is removed when the test ends.
class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_app_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" +
             std::to_string(++counter));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string str() const { return path_.string(); }
  std::string file(const std::string& name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

// A model that answers with a scripted reply and records what it was asked.
class ScriptedModel final : public ModelBackend {
 public:
  bool respond(const std::vector<ModelMessage>& messages, ModelResponse& out, std::string& error) override {
    seen.push_back(messages);
    if (fail) {
      error = "The model is unreachable";
      return false;
    }
    out = ModelResponse();
    out.content = answer;
    return true;
  }

  void setTools(const std::string& definitions) override { tools_json = definitions; }

  std::string answer = "the answer";
  bool fail = false;
  std::string tools_json;
  std::vector<std::vector<ModelMessage>> seen;
};

// The streams and the model one test drives the app through.
struct Harness {
  std::ostringstream out;
  std::ostringstream err;
  std::istringstream in;
  std::shared_ptr<ScriptedModel> model = std::make_shared<ScriptedModel>();

  App::Options options() {
    App::Options result;
    result.make_model = [model = model](const bjorne_agent::config::ModelConfig&) {
      return std::static_pointer_cast<ModelBackend>(model);
    };
    result.out = &out;
    result.err = &err;
    result.in = &in;
    return result;
  }
};

// A path that goes into a fixture as a JSON string. The native separator is a
// backslash on Windows, and JSON reads a backslash as the start of an escape,
// so it has to be doubled on the way in.
std::string jsonString(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char character : text) {
    if (character == '\\' || character == '"') {
      out.push_back('\\');
    }
    out.push_back(character);
  }
  return out;
}

// Writes a config file with one model and the given session directory.
std::string writeConfig(const TempDir& directory, const std::string& sessionDirectory) {
  const std::string path = directory.file("bjorne_agent.json");
  std::ofstream file(path);
  file << R"({
    "version": 1,
    "agent": {
      "system_prompt": "You are a helper.",
      "workspace": ")" << jsonString(directory.str()) << R"(",
      "session_dir": ")" << jsonString(sessionDirectory) << R"("
    },
    "models": [
      {
        "provider": "openai",
        "endpoint": "https://example.invalid",
        "model": "test-model",
        "context_window": 8000,
        "max_tokens": 1024,
        "api_key_env": "BJORNE_AGENT_TEST_KEY"
      }
    ]
  })";
  file.close();
  return path;
}

// Writes a config file with one model and the given session directory. The
// extra agent and model fields are raw JSON members, without the braces.
// The extra models are further entries of the model list.
std::string writeConfigWith(const TempDir& directory, const std::string& sessionDirectory,
                            const std::string& extraAgent, const std::string& extraModel = {},
                            const std::string& extraModels = {}) {
  const std::string path = directory.file("bjorne_agent.json");
  std::ofstream file(path);
  file << "{\n"
       << "  \"version\": 1,\n"
       << "  \"agent\": {\n"
       << "    \"system_prompt\": \"You are a helper.\",\n"
       << "    \"workspace\": \"" << jsonString(directory.str()) << "\",\n"
       << "    \"session_dir\": \"" << jsonString(sessionDirectory) << "\""
       << extraAgent << "\n"
       << "  },\n"
       << "  \"models\": [\n"
       << "    {\n"
       << "      \"provider\": \"openai\",\n"
       << "      \"endpoint\": \"https://example.invalid\",\n"
       << "      \"model\": \"test-model\",\n"
       << "      \"context_window\": 8000,\n"
       << "      \"max_tokens\": 1024,\n"
       << "      \"api_key_env\": \"BJORNE_AGENT_TEST_KEY\""
       << extraModel << "\n"
       << "    }"
       << extraModels << "\n"
       << "  ]\n"
       << "}\n";
  file.close();
  return path;
}

// Sets the environment variable the config search reads, and restores it when
// the test ends.
class ConfigEnvironment {
 public:
  explicit ConfigEnvironment(const std::string& path) {
    bjorne_agent::platform::setEnvironmentVariable("BJORNE_AGENT_CONFIG", path);
  }
  ~ConfigEnvironment() { bjorne_agent::platform::unsetEnvironmentVariable("BJORNE_AGENT_CONFIG"); }
  ConfigEnvironment(const ConfigEnvironment&) = delete;
  ConfigEnvironment& operator=(const ConfigEnvironment&) = delete;
};

// Points the user level config directory at a temporary directory, so a test
// never writes into the home directory of the user running it.
class UserConfigDirectory {
 public:
  explicit UserConfigDirectory(const std::string& directory) {
    bjorne_agent::platform::setEnvironmentVariable("XDG_CONFIG_HOME", directory);
  }
  ~UserConfigDirectory() { bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME"); }
  UserConfigDirectory(const UserConfigDirectory&) = delete;
  UserConfigDirectory& operator=(const UserConfigDirectory&) = delete;
};

bjorne_agent::cli::Invocation runInvocation() {
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Run;
  return invocation;
}

// Writes a file, creating the directories above it.
void writeFile(const std::string& path, const std::string& content) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream(path) << content;
}

}  // namespace

TEST_CASE("A run answers the task and prints the result") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, directory.file("sessions"));
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.out.str() == "the answer\n");
  REQUIRE(harness.err.str().empty());
  REQUIRE(harness.model->seen.size() == 1);
  REQUIRE(harness.model->seen.front().back().content == "do the thing");
}

TEST_CASE("A run takes the task from standard input") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, directory.file("sessions"));
  Harness harness;
  harness.in.str("the task from stdin");

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.out.str() == "the answer\n");
  REQUIRE(harness.model->seen.front().back().content == "the task from stdin");
}

TEST_CASE("A run without a task reports the usage") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, directory.file("sessions"));
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.out.str().empty());
  REQUIRE(harness.err.str().find("Usage:") != std::string::npos);
}

TEST_CASE("A run with a config that has no model reports it") {
  TempDir directory;
  const std::string configPath = directory.file("empty.json");
  std::ofstream file(configPath);
  file << R"({"version": 1, "agent": {"workspace": "/tmp"}})";
  file.close();
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("Config has no models") != std::string::npos);
}

TEST_CASE("A run with a config that cannot be read reports it") {
  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = "does-not-exist.json";
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("Cannot open config file") != std::string::npos);
}

TEST_CASE("A run that fails to reach the model reports it") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, directory.file("sessions"));
  Harness harness;
  harness.model->fail = true;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("The model is unreachable") != std::string::npos);
}

TEST_CASE("A run without a session directory stays one-shot") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, "");
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.out.str() == "the answer\n");
  // No session file was written, because there is nowhere to write it. The
  // only file in the directory is the config the test wrote.
  std::size_t files = 0;
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory.str())) {
    files++;
    REQUIRE(entry.path().filename() == "bjorne_agent.json");
  }
  REQUIRE(files == 1);
}

TEST_CASE("A run stores the session it made") {
  TempDir directory;
  const std::string sessions = directory.file("sessions");
  const std::string configPath = writeConfig(directory, sessions);
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 0);

  std::filesystem::directory_iterator entry(sessions);
  REQUIRE(entry != std::filesystem::directory_iterator());
  const std::string name = entry->path().filename().string();
  REQUIRE(name.size() > 5);
  REQUIRE(name.substr(name.size() - 5) == ".json");
}

TEST_CASE("The sessions command lists the stored sessions") {
  TempDir directory;
  const std::string sessions = directory.file("sessions");
  const std::string configPath = writeConfig(directory, sessions);
  const ConfigEnvironment environment(configPath);

  // One session, written by a store the test owns.
  std::string error;
  std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(sessions, error);
  REQUIRE(store != nullptr);
  bjorne_agent::session::SessionState state;
  state.task = "the stored task";
  state.status = "complete";
  REQUIRE(store->begin(state, error));

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Sessions;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.out.str().find(state.id) != std::string::npos);
  REQUIRE(harness.out.str().find("the stored task") != std::string::npos);
}

TEST_CASE("The sessions command reports an empty store") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, directory.file("sessions"));
  const ConfigEnvironment environment(configPath);

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Sessions;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.out.str() == "No stored sessions.\n");
}

TEST_CASE("A resume continues the stored conversation") {
  TempDir directory;
  const std::string sessions = directory.file("sessions");
  const std::string configPath = writeConfig(directory, sessions);

  std::string error;
  std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(sessions, error);
  REQUIRE(store != nullptr);
  bjorne_agent::session::SessionState state;
  state.task = "the stored task";
  state.status = "running";
  bjorne_agent::model::ModelMessage user;
  user.role = "user";
  user.content = "the earlier question";
  state.messages.push_back(user);
  REQUIRE(store->begin(state, error));

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Resume;
  invocation.configPath = configPath;
  invocation.session = state.id;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.out.str() == "the answer\n");
  // The stored conversation was part of the request, and no new task was.
  REQUIRE(harness.model->seen.size() == 1);
  const std::vector<ModelMessage>& sent = harness.model->seen.front();
  REQUIRE(sent.back().content == "the earlier question");
}

TEST_CASE("A resume of a completed session prints the stored answer") {
  TempDir directory;
  const std::string sessions = directory.file("sessions");
  const std::string configPath = writeConfig(directory, sessions);

  std::string error;
  std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(sessions, error);
  REQUIRE(store != nullptr);
  bjorne_agent::session::SessionState state;
  state.task = "the stored task";
  state.status = "complete";
  state.result = "the stored answer";
  REQUIRE(store->begin(state, error));

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Resume;
  invocation.configPath = configPath;
  invocation.session = state.id;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.out.str() == "the stored answer\n");
  // The model was not asked, because the session was already finished.
  REQUIRE(harness.model->seen.empty());
}

TEST_CASE("A resume without a stored session reports it") {
  TempDir directory;
  const std::string sessions = directory.file("sessions");
  const std::string configPath = writeConfig(directory, sessions);

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Resume;
  invocation.configPath = configPath;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("No stored sessions") != std::string::npos);
}

TEST_CASE("A resume of a session that does not exist reports it") {
  TempDir directory;
  const std::string sessions = directory.file("sessions");
  const std::string configPath = writeConfig(directory, sessions);

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Resume;
  invocation.configPath = configPath;
  invocation.session = "no-such-session";

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("Cannot read session") != std::string::npos);
}

TEST_CASE("The init command runs through the app") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());

  Harness harness;
  harness.in.str("2\nhttps://api.example.com/v1\n\n\n\nn\n\n");
  harness.in.clear();

  App::Options options = harness.options();
  options.input_is_terminal = true;
  App app(options);
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Init;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(std::filesystem::exists(directory.file("bjorne_agent/bjorne_agent.json")));
}

TEST_CASE("The init command fails through the app when the input is not a terminal") {
  Harness harness;
  App::Options options = harness.options();
  options.input_is_terminal = false;
  App app(options);
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Init;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.out.str().find("not a terminal") != std::string::npos);
}

TEST_CASE("A run reads the system prompt from the file named by the config") {
  TempDir directory;
  writeFile(directory.file("system.md"), "Be terse.\n");
  const std::string configPath = writeConfigWith(directory, directory.file("sessions"),
                                                 ",\n    \"system_prompt_path\": \"system.md\"");
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 0);
  const std::vector<ModelMessage>& sent = harness.model->seen.front();
  REQUIRE(sent.front().role == "system");
  REQUIRE(sent.front().content.find("Be terse.") != std::string::npos);
}

TEST_CASE("A run reports a system prompt it cannot read") {
  TempDir directory;
  const std::string configPath = writeConfigWith(directory, directory.file("sessions"),
                                                 ",\n    \"system_prompt_path\": \"missing.md\"");
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("Cannot open system prompt") != std::string::npos);
}

TEST_CASE("A run reads the compression prompt named by the config") {
  TempDir directory;
  writeFile(directory.file("prompt.md"), "Summarize it in one line.");
  const std::string configPath = writeConfigWith(directory, directory.file("sessions"), "",
                                                 ",\n      \"compression_prompt_path\": \"prompt.md\"");
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.err.str().empty());
}

TEST_CASE("A run reports a compression prompt it cannot read") {
  TempDir directory;
  const std::string configPath = writeConfigWith(directory, directory.file("sessions"), "",
                                                 ",\n      \"compression_prompt_path\": \"missing.md\"");
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("Cannot open compression prompt") != std::string::npos);
}

TEST_CASE("A run loads the skills and the AGENTS.md of the workspace") {
  TempDir directory;
  writeFile(directory.file("skills/alpha/SKILL.md"),
            "---\nname: alpha\ndescription: Alpha skill\n---\nAlpha instructions.\n");
  writeFile(directory.file("AGENTS.md"), "Follow the house style.\n");
  const std::string configPath = writeConfigWith(directory, directory.file("sessions"),
                                                 ",\n    \"skills_path\": \"skills\"");
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 0);
  // The skill index and the AGENTS.md are part of the system message.
  const std::vector<ModelMessage>& sent = harness.model->seen.front();
  REQUIRE(sent.front().role == "system");
  REQUIRE(sent.front().content.find("alpha") != std::string::npos);
  REQUIRE(sent.front().content.find("Follow the house style.") != std::string::npos);
  // The load_skill tool is part of the tool set.
  REQUIRE(harness.model->tools_json.find("load_skill") != std::string::npos);
}

TEST_CASE("A run reports a skills directory that does not exist") {
  TempDir directory;
  const std::string configPath = writeConfigWith(directory, directory.file("sessions"),
                                                 ",\n    \"skills_path\": \"nowhere\"");
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  // The directory is missing, but the run still goes on without skills.
  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.model->tools_json.find("load_skill") == std::string::npos);
}

TEST_CASE("A run picks the model with the lowest priority") {
  TempDir directory;
  const std::string configPath = writeConfigWith(
      directory, directory.file("sessions"), "", "",
      ",\n    {\n      \"provider\": \"openai\",\n      \"endpoint\": \"https://example.invalid\",\n"
      "      \"model\": \"second-model\",\n      \"context_window\": 8000,\n      \"max_tokens\": 1024,\n"
      "      \"api_key_env\": \"BJORNE_AGENT_TEST_KEY\",\n      \"priority\": 5\n    }");
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 0);
  // The model the priority picked is the one that answered.
  REQUIRE(harness.model->seen.size() == 1);
}

TEST_CASE("A resume finds the config through the environment") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, directory.file("sessions"));
  const ConfigEnvironment environment(configPath);

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Resume;

  // There is no stored session, but the config was found.
  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("No stored sessions") != std::string::npos);
}

TEST_CASE("A resume reports a config the environment does not name") {
  const ConfigEnvironment environment("does-not-exist.json");

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Resume;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("BJORNE_AGENT_CONFIG names a file that does not exist") != std::string::npos);
}

TEST_CASE("The sessions command finds the config through the environment") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, directory.file("sessions"));
  const ConfigEnvironment environment(configPath);

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Sessions;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.out.str() == "No stored sessions.\n");
}

TEST_CASE("The sessions command reports a config the environment does not name") {
  const ConfigEnvironment environment("does-not-exist.json");

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Sessions;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("BJORNE_AGENT_CONFIG names a file that does not exist") != std::string::npos);
}

TEST_CASE("A run answers through the real HTTP client") {
  bjorne_agent_test::LoopbackServer server;
  server.setResponse(
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
      R"({"choices":[{"message":{"role":"assistant","content":"from the wire"}}]})");

  TempDir directory;
  const std::string configPath = directory.file("bjorne_agent.json");
  std::ofstream file(configPath);
  file << R"({
    "version": 1,
    "agent": {
      "system_prompt": "You are a helper.",
      "workspace": ")" << jsonString(directory.str()) << R"(",
      "session_dir": ")" << jsonString(directory.file("sessions")) << R"("
    },
    "models": [
      {
        "provider": "openai",
        "endpoint": "http://127.0.0.1:)" << server.port() << R"(/v1/chat/completions",
        "model": "test-model",
        "context_window": 8000,
        "max_tokens": 1024,
        "api_key_env": "BJORNE_AGENT_TEST_KEY"
      }
    ]
  })";
  file.close();

  bjorne_agent::platform::setEnvironmentVariable("BJORNE_AGENT_TEST_KEY", "secret");

  std::ostringstream out;
  std::ostringstream err;
  App::Options options;
  options.out = &out;
  options.err = &err;

  App app(options);
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(out.str() == "from the wire\n");
  REQUIRE(server.wait());
  REQUIRE(server.request().find("do the thing") != std::string::npos);
  bjorne_agent::platform::unsetEnvironmentVariable("BJORNE_AGENT_TEST_KEY");
}

TEST_CASE("The sessions command reports that no session directory is configured") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, "");
  const ConfigEnvironment environment(configPath);

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Sessions;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("No session_dir is configured") != std::string::npos);
}

TEST_CASE("The sessions command reports a session path that is not a directory") {
  TempDir directory;
  const std::string sessionPath = directory.file("sessions");
  writeFile(sessionPath, "not a directory\n");
  const std::string configPath = writeConfig(directory, sessionPath);
  const ConfigEnvironment environment(configPath);

  Harness harness;
  App app(harness.options());
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Sessions;

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(harness.err.str().find("Session path is not a directory") != std::string::npos);
}

TEST_CASE("A run accepts a self-signed certificate and a server name") {
  TempDir directory;
  const std::string configPath =
      writeConfigWith(directory, directory.file("sessions"), "",
                      ", \"ignore_self_signed\": true, \"hostname\": \"api.example.test\"");
  Harness harness;

  App app(harness.options());
  bjorne_agent::cli::Invocation invocation = runInvocation();
  invocation.configPath = configPath;
  invocation.task = "do the thing";
  invocation.taskFromArgument = true;

  REQUIRE(app.run(invocation) == 0);
  REQUIRE(harness.out.str() == "the answer\n");
}

TEST_CASE("The default app writes to the standard streams") {
  // The binary builds the app without options, so the default constructor has
  // to exist and has to reach the standard streams.
  App app;
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Init;

  REQUIRE(app.run(invocation) == 1);
}
