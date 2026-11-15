#include "app/App.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "jsonrpc/Endpoint.h"
#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

namespace {

using bjorne_agent::app::App;
using bjorne_agent::jsonrpc::ReadResult;
using bjorne_agent::jsonrpc::Transport;
using bjorne_agent::model::ModelBackend;
using bjorne_agent::model::ModelMessage;
using bjorne_agent::model::ModelResponse;

// A directory that is removed when the test ends.
class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_app_acp_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" +
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

// The stream the editor talks to the agent over. The test scripts the lines
// the agent receives and records the lines it sends.
class ScriptedTransport final : public Transport {
 public:
  bool sendLine(const std::string& line, std::string& error) override {
    (void)error;
    std::lock_guard<std::mutex> guard(mutex_);
    sent.push_back(line);
    // The last scripted request closes the connection, the way an editor
    // that got what it wanted hangs up.
    if (close_after_id_ != 0 && line.find("\"id\":" + std::to_string(close_after_id_)) != std::string::npos) {
      closed_ = true;
    }
    condition_.notify_all();
    return true;
  }

  ReadResult readLine(int timeout_ms, std::string& line, std::string& error) override {
    (void)error;
    std::unique_lock<std::mutex> guard(mutex_);
    if (!incoming.empty()) {
      line = incoming.front();
      incoming.pop_front();
      return ReadResult::Line;
    }
    condition_.wait_for(guard, std::chrono::milliseconds(timeout_ms),
                        [this] { return closed_ || !incoming.empty(); });
    if (!incoming.empty()) {
      line = incoming.front();
      incoming.pop_front();
      return ReadResult::Line;
    }
    return closed_ ? ReadResult::Closed : ReadResult::Timeout;
  }

  // Queues a line the agent reads next.
  void push(std::string line) {
    std::lock_guard<std::mutex> guard(mutex_);
    incoming.push_back(std::move(line));
    condition_.notify_all();
  }

  // The line the agent sent for the given request id. Empty when there is
  // none yet.
  std::string answerFor(int id) {
    std::lock_guard<std::mutex> guard(mutex_);
    const std::string needle = "\"id\":" + std::to_string(id);
    for (const std::string& line : sent) {
      if (line.find(needle) != std::string::npos) {
        return line;
      }
    }
    return {};
  }

  // Closes the connection after the answer to this request id was sent.
  void closeAfter(int id) {
    std::lock_guard<std::mutex> guard(mutex_);
    close_after_id_ = id;
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::string> incoming;
  std::vector<std::string> sent;
  bool closed_ = false;
  int close_after_id_ = 0;
};

// A model that answers with a scripted reply.
class ScriptedModel final : public ModelBackend {
 public:
  bool respond(const std::vector<ModelMessage>& messages, ModelResponse& out, std::string& error) override {
    (void)messages;
    (void)error;
    out = ModelResponse();
    out.content = "the answer";
    return true;
  }

  void setTools(const std::string& definitions) override { tools_json = definitions; }

  std::string tools_json;
};

// Writes a config file with one model and the given session directory.
std::string writeConfig(const TempDir& directory, const std::string& sessionDirectory) {
  const std::string path = directory.file("bjorne_agent.json");
  std::ofstream file(path);
  file << R"({
    "version": 1,
    "agent": {
      "system_prompt": "You are a helper.",
      "workspace": ")" << directory.str() << R"(",
      "session_dir": ")" << sessionDirectory << R"("
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

}  // namespace

TEST_CASE("The ACP command serves the editor and answers its requests") {
  TempDir directory;
  const std::string configPath = writeConfig(directory, directory.file("sessions"));

  auto transport = std::make_shared<ScriptedTransport>();
  transport->closeAfter(2);
  transport->push(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1}})");
  transport->push(R"({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":")" +
                   directory.str() + R"("}})");

  std::ostringstream out;
  std::ostringstream err;
  auto model = std::make_shared<ScriptedModel>();

  App::Options options;
  options.out = &out;
  options.err = &err;
  options.make_model = [model](const bjorne_agent::config::ModelConfig&) {
    return std::static_pointer_cast<ModelBackend>(model);
  };
  options.make_transport = [transport] { return std::static_pointer_cast<Transport>(transport); };

  App app(options);
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Acp;
  invocation.configPath = configPath;

  REQUIRE(app.run(invocation) == 0);

  const std::string answer = transport->answerFor(2);
  REQUIRE(answer.find("sess_") != std::string::npos);
  REQUIRE(answer.find("\"error\"") == std::string::npos);
}

TEST_CASE("The ACP command reports a config that cannot be read") {
  std::ostringstream out;
  std::ostringstream err;

  App::Options options;
  options.out = &out;
  options.err = &err;

  App app(options);
  bjorne_agent::cli::Invocation invocation;
  invocation.command = bjorne_agent::cli::Command::Acp;
  invocation.configPath = "does-not-exist.json";

  REQUIRE(app.run(invocation) == 1);
  REQUIRE(err.str().find("Cannot open config file") != std::string::npos);
}
