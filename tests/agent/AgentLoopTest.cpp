#include "agent/AgentLoop.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

#include "log/LogCapture.h"

namespace {

using bjorne_agent::model::ModelMessage;
using bjorne_agent::model::ModelResponse;
using bjorne_agent_test::CaptureGuard;
using bjorne_agent_test::LevelGuard;

class MockModelBackend final : public bjorne_agent::model::ModelBackend {
 public:
  bool respond(const std::vector<ModelMessage>& messages, ModelResponse& out, std::string& error) override {
    seen.push_back(messages);
    if (fail) {
      error = fail_error;
      return false;
    }
    if (responses.empty()) {
      out = ModelResponse();
      return true;
    }
    out = responses.front();
    responses.erase(responses.begin());
    return true;
  }

  std::vector<std::vector<ModelMessage>> seen;
  std::vector<ModelResponse> responses;
  bool fail = false;
  std::string fail_error;
};

class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    // The path carries the process id so that two test binaries running at the
    // same time do not overwrite each other's fixture.
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" + std::to_string(++counter));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string str() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

std::shared_ptr<bjorne_agent::agent::AgentLoop> makeLoop(std::shared_ptr<MockModelBackend> backend, const std::string& workspace,
                                                   long long max_iterations = 25) {
  bjorne_agent::agent::AgentLoopConfig config;
  config.system_prompt = "You are a test agent.";
  config.workspace = workspace;
  config.max_iterations = max_iterations;

  bjorne_agent::context::ContextManagerConfig contextConfig;
  contextConfig.context_window = 0;
  auto context = std::make_shared<bjorne_agent::context::ContextManager>(contextConfig);
  auto tools = std::make_shared<bjorne_agent::tools::ToolRegistry>(workspace);
  return std::make_shared<bjorne_agent::agent::AgentLoop>(config, backend, context, tools);
}

ModelResponse toolResponse(const std::string& name, const std::string& arguments) {
  ModelResponse response;
  response.tool_calls.push_back(bjorne_agent::model::ToolCall{"call_1", name, arguments});
  return response;
}

ModelMessage message(const std::string& role, const std::string& content) {
  ModelMessage value;
  value.role = role;
  value.content = content;
  return value;
}

}  // namespace

TEST_CASE("Agent loop returns the final response") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(ModelResponse{"done", {}, {}});
  const auto loop = makeLoop(backend, "");

  std::string result;
  std::string error;
  REQUIRE(loop->run("say done", result, error));
  REQUIRE(error.empty());
  REQUIRE(result == "done");
  REQUIRE(backend->seen.size() == 1);
  REQUIRE(backend->seen[0].size() == 2);
  REQUIRE(backend->seen[0][0].role == "system");
  REQUIRE(backend->seen[0][0].content == "You are a test agent.");
  REQUIRE(backend->seen[0][1].role == "user");
  REQUIRE(backend->seen[0][1].content == "say done");
}

TEST_CASE("Agent loop executes tool calls and continues") {
  TempDir dir;
  std::ofstream(dir.str() + "/note.txt") << "hello";

  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(toolResponse("read_file", R"({"path":"note.txt"})"));
  backend->responses.push_back(ModelResponse{"finished", {}, {}});
  const auto loop = makeLoop(backend, dir.str());

  std::string result;
  std::string error;
  REQUIRE(loop->run("read the note", result, error));
  REQUIRE(error.empty());
  REQUIRE(result == "finished");
  REQUIRE(backend->seen.size() == 2);
  REQUIRE(backend->seen[1].size() == 4);
  REQUIRE(backend->seen[1][2].role == "assistant");
  REQUIRE(backend->seen[1][2].tool_calls.size() == 1);
  REQUIRE(backend->seen[1][3].role == "tool");
  REQUIRE(backend->seen[1][3].tool_call_id == "call_1");
  REQUIRE(backend->seen[1][3].content == "hello");
}

TEST_CASE("Agent loop reports tool errors and continues") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(toolResponse("missing_tool", "{}"));
  backend->responses.push_back(ModelResponse{"finished", {}, {}});
  const auto loop = makeLoop(backend, "");

  std::string result;
  std::string error;
  REQUIRE(loop->run("try a missing tool", result, error));
  REQUIRE(error.empty());
  REQUIRE(result == "finished");
  REQUIRE(backend->seen[1][3].role == "tool");
  REQUIRE(backend->seen[1][3].content == "Error: unknown tool: missing_tool");
}

TEST_CASE("Agent loop shows the model's reasoning and narration before a tool runs") {
  const LevelGuard guard(bjorne_agent::log::Level::Normal);
  const CaptureGuard capture;

  auto backend = std::make_shared<MockModelBackend>();
  ModelResponse first;
  first.reasoning = "I should try the tool first.";
  first.content = "Trying a tool now.";
  first.tool_calls.push_back(bjorne_agent::model::ToolCall{"call_1", "missing_tool", "{}"});
  backend->responses.push_back(first);
  backend->responses.push_back(ModelResponse{"finished", {}, {}});
  const auto loop = makeLoop(backend, "");

  std::string result;
  std::string error;
  REQUIRE(loop->run("go", result, error));
  REQUIRE(result == "finished");

  const std::string text = capture.text();
  REQUIRE(text.find("thinking:\n  I should try the tool first.") != std::string::npos);
  REQUIRE(text.find("assistant:\n  Trying a tool now.") != std::string::npos);
  REQUIRE(text.find("arguments:\n  {}") != std::string::npos);
  // The narration is printed before the tool it introduces runs.
  REQUIRE(text.find("Trying a tool now.") < text.find("Running tool: missing_tool"));
}

TEST_CASE("Agent loop shows a successful tool result") {
  const LevelGuard guard(bjorne_agent::log::Level::Normal);
  const CaptureGuard capture;

  TempDir dir;
  std::ofstream(dir.str() + "/note.txt") << "hello";
  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(toolResponse("read_file", R"({"path":"note.txt"})"));
  backend->responses.push_back(ModelResponse{"finished", {}, {}});
  const auto loop = makeLoop(backend, dir.str());

  std::string result;
  std::string error;
  REQUIRE(loop->run("read the note", result, error));

  const std::string text = capture.text();
  REQUIRE(text.find("arguments:\n  {\"path\":\"note.txt\"}") != std::string::npos);
  REQUIRE(text.find("result:\n  hello") != std::string::npos);
}

TEST_CASE("Agent loop shows final reasoning but leaves the answer to stdout") {
  const LevelGuard guard(bjorne_agent::log::Level::Normal);
  const CaptureGuard capture;

  auto backend = std::make_shared<MockModelBackend>();
  ModelResponse finalResponse;
  finalResponse.reasoning = "The task is done.";
  finalResponse.content = "the final answer";
  backend->responses.push_back(finalResponse);
  const auto loop = makeLoop(backend, "");

  std::string result;
  std::string error;
  REQUIRE(loop->run("finish", result, error));
  REQUIRE(result == "the final answer");

  const std::string text = capture.text();
  REQUIRE(text.find("thinking:\n  The task is done.") != std::string::npos);
  // The final answer is written once, to stdout, by the caller.
  REQUIRE(text.find("the final answer") == std::string::npos);
}

TEST_CASE("Agent loop stops on model error") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->fail = true;
  backend->fail_error = "backend failure";
  const auto loop = makeLoop(backend, "");

  std::string result;
  std::string error;
  REQUIRE_FALSE(loop->run("fail", result, error));
  REQUIRE(error == "backend failure");
}

TEST_CASE("Agent loop stops at the iteration cap") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(toolResponse("read_file", R"({"path":"a.txt"})"));
  backend->responses.push_back(toolResponse("read_file", R"({"path":"b.txt"})"));
  const auto loop = makeLoop(backend, "", 2);

  std::string result;
  std::string error;
  REQUIRE_FALSE(loop->run("loop forever", result, error));
  REQUIRE(error == "Iteration cap reached without a final response");
  REQUIRE(backend->seen.size() == 2);
}

TEST_CASE("Agent loop stops when the context cannot be trimmed") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(ModelResponse{"never used", {}, {}});

  bjorne_agent::agent::AgentLoopConfig config;
  config.system_prompt = "You are a test agent.";
  config.max_iterations = 3;

  bjorne_agent::context::ContextManagerConfig contextConfig;
  contextConfig.overflow_strategy = bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Compress;
  contextConfig.context_window = 1;
  auto context = std::make_shared<bjorne_agent::context::ContextManager>(contextConfig);
  auto tools = std::make_shared<bjorne_agent::tools::ToolRegistry>("");
  bjorne_agent::agent::AgentLoop loop(config, backend, context, tools);

  std::string result;
  std::string error;
  REQUIRE_FALSE(loop.run("too big", result, error));
  REQUIRE(!error.empty());
  REQUIRE(backend->seen.empty());
}

namespace {

class RecordingObserver final : public bjorne_agent::agent::LoopObserver {
 public:
  void onTurn(const std::vector<ModelMessage>& messages, long long turn) override {
    turn_numbers.push_back(turn);
    turn_sizes.push_back(messages.size());
  }
  void onComplete(const std::vector<ModelMessage>& messages, const std::string& finalResult) override {
    completed = true;
    completion = messages;
    result = finalResult;
  }
  void onFailure(const std::vector<ModelMessage>& messages, const std::string& error) override {
    failed = true;
    failure = error;
    failure_messages = messages;
  }
  void onInterrupt(const std::vector<ModelMessage>& messages) override {
    interrupted = true;
    interrupt_messages = messages;
  }

  std::vector<long long> turn_numbers;
  std::vector<std::size_t> turn_sizes;
  bool completed = false;
  std::vector<ModelMessage> completion;
  std::string result;
  bool failed = false;
  std::string failure;
  std::vector<ModelMessage> failure_messages;
  bool interrupted = false;
  std::vector<ModelMessage> interrupt_messages;
};

std::shared_ptr<bjorne_agent::agent::AgentLoop> makeObservedLoop(std::shared_ptr<MockModelBackend> backend,
                                                                 bjorne_agent::agent::AgentLoopConfig config,
                                                                 std::shared_ptr<RecordingObserver> observer) {
  bjorne_agent::context::ContextManagerConfig contextConfig;
  auto context = std::make_shared<bjorne_agent::context::ContextManager>(contextConfig);
  auto tools = std::make_shared<bjorne_agent::tools::ToolRegistry>(config.workspace);
  return std::make_shared<bjorne_agent::agent::AgentLoop>(config, backend, context, tools, observer);
}

}  // namespace

TEST_CASE("Agent loop reports each tool turn and the final answer to the observer") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(toolResponse("missing_tool", "{}"));
  backend->responses.push_back(ModelResponse{"finished", {}, {}});
  auto observer = std::make_shared<RecordingObserver>();

  bjorne_agent::agent::AgentLoopConfig config;
  config.system_prompt = "You are a test agent.";
  config.workspace = "";
  const auto loop = makeObservedLoop(backend, config, observer);

  std::string result;
  std::string error;
  REQUIRE(loop->run("use the tool", result, error));
  REQUIRE(result == "finished");
  REQUIRE(observer->turn_numbers.size() == 1);
  REQUIRE(observer->turn_numbers[0] == 1);
  REQUIRE(observer->turn_sizes[0] == 4);
  REQUIRE(observer->completed);
  REQUIRE(observer->result == "finished");
  REQUIRE(observer->completion.back().role == "assistant");
  REQUIRE(observer->completion.back().content == "finished");
}

TEST_CASE("Agent loop continues from stored history without a new task") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(ModelResponse{"continued", {}, {}});

  bjorne_agent::agent::AgentLoopConfig config;
  config.system_prompt = "fresh prompt that must not be added";
  config.history.push_back(message("system", "stored prompt"));
  config.history.push_back(message("user", "first task"));
  config.history.push_back(message("assistant", "first answer"));
  config.history.push_back(message("user", "second task"));

  bjorne_agent::context::ContextManagerConfig contextConfig;
  auto context = std::make_shared<bjorne_agent::context::ContextManager>(contextConfig);
  auto tools = std::make_shared<bjorne_agent::tools::ToolRegistry>("");
  bjorne_agent::agent::AgentLoop loop(config, backend, context, tools);

  std::string result;
  std::string error;
  REQUIRE(loop.run("", result, error));
  REQUIRE(result == "continued");
  REQUIRE(backend->seen.size() == 1);
  REQUIRE(backend->seen[0].size() == 4);
  REQUIRE(backend->seen[0][0].content == "stored prompt");
  REQUIRE(backend->seen[0][3].content == "second task");
}

TEST_CASE("Agent loop adds the configured prompt when history has no system message") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(ModelResponse{"done", {}, {}});

  bjorne_agent::agent::AgentLoopConfig config;
  config.system_prompt = "configured prompt";
  config.history.push_back(message("user", "an earlier task"));

  bjorne_agent::context::ContextManagerConfig contextConfig;
  auto context = std::make_shared<bjorne_agent::context::ContextManager>(contextConfig);
  auto tools = std::make_shared<bjorne_agent::tools::ToolRegistry>("");
  bjorne_agent::agent::AgentLoop loop(config, backend, context, tools);

  std::string result;
  std::string error;
  REQUIRE(loop.run("", result, error));
  REQUIRE(backend->seen[0].size() == 2);
  REQUIRE(backend->seen[0][0].role == "system");
  REQUIRE(backend->seen[0][0].content == "configured prompt");
  REQUIRE(backend->seen[0][1].content == "an earlier task");
}

TEST_CASE("Agent loop reports an empty context as a failure") {
  auto backend = std::make_shared<MockModelBackend>();
  auto observer = std::make_shared<RecordingObserver>();

  bjorne_agent::agent::AgentLoopConfig config;
  const auto loop = makeObservedLoop(backend, config, observer);

  std::string result;
  std::string error;
  REQUIRE_FALSE(loop->run("", result, error));
  REQUIRE(error == "No task and no session history to continue");
  REQUIRE(backend->seen.empty());
  REQUIRE(observer->failed);
}

TEST_CASE("Agent loop interrupts before the first model call") {
  auto backend = std::make_shared<MockModelBackend>();
  auto observer = std::make_shared<RecordingObserver>();

  bjorne_agent::agent::AgentLoopConfig config;
  config.system_prompt = "test";
  config.stop_requested = [] { return true; };
  const auto loop = makeObservedLoop(backend, config, observer);

  std::string result;
  std::string error;
  REQUIRE_FALSE(loop->run("go", result, error));
  REQUIRE(error == "Interrupted by a shutdown signal");
  REQUIRE(backend->seen.empty());
  REQUIRE(observer->interrupted);
  REQUIRE(observer->interrupt_messages.size() == 2);
}

TEST_CASE("Agent loop interrupts after a model call without recording the response") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->responses.push_back(toolResponse("read_file", R"({"path":"a.txt"})"));
  auto observer = std::make_shared<RecordingObserver>();

  int checks = 0;
  bjorne_agent::agent::AgentLoopConfig config;
  config.system_prompt = "test";
  config.stop_requested = [&checks] { return ++checks > 1; };
  const auto loop = makeObservedLoop(backend, config, observer);

  std::string result;
  std::string error;
  REQUIRE_FALSE(loop->run("go", result, error));
  REQUIRE(error == "Interrupted by a shutdown signal");
  REQUIRE(backend->seen.size() == 1);
  REQUIRE(observer->interrupted);
  REQUIRE(observer->interrupt_messages.size() == 2);
  REQUIRE(observer->interrupt_messages[1].content == "go");
  REQUIRE(observer->turn_numbers.empty());
}

TEST_CASE("Agent loop reports a model failure to the observer") {
  auto backend = std::make_shared<MockModelBackend>();
  backend->fail = true;
  backend->fail_error = "backend failure";
  auto observer = std::make_shared<RecordingObserver>();

  bjorne_agent::agent::AgentLoopConfig config;
  config.system_prompt = "test";
  const auto loop = makeObservedLoop(backend, config, observer);

  std::string result;
  std::string error;
  REQUIRE_FALSE(loop->run("fail", result, error));
  REQUIRE(error == "backend failure");
  REQUIRE(observer->failed);
  REQUIRE(observer->failure == "backend failure");
  REQUIRE(observer->failure_messages.size() == 2);
}
