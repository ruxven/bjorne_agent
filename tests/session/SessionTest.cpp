#include "session/SessionRecorder.h"
#include "session/Session.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

namespace {

using bjorne_agent::model::ModelMessage;
using bjorne_agent::model::ToolCall;

class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_session_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" +
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

 private:
  std::filesystem::path path_;
};

void writeFile(const std::string& path, const std::string& content) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream(path) << content;
}

std::string readFile(const std::string& path) {
  std::ifstream file(path);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

ModelMessage message(const std::string& role, const std::string& content) {
  ModelMessage value;
  value.role = role;
  value.content = content;
  return value;
}

ModelMessage toolResult(const std::string& id, const std::string& content) {
  ModelMessage value;
  value.role = "tool";
  value.tool_call_id = id;
  value.content = content;
  return value;
}

std::string summaryJson(const std::string& id, long long updated, const std::string& status, const std::string& task) {
  return "{\"version\":1,\"id\":\"" + id + "\",\"created\":1,\"updated\":" + std::to_string(updated) +
         ",\"task\":\"" + task + "\",\"status\":\"" + status + "\",\"messages\":[]}";
}

}  // namespace

TEST_CASE("a session round trips through the store") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);
  REQUIRE(error.empty());

  bjorne_agent::session::SessionState state;
  state.task = "fix the parser";
  state.workspace = "/workspace";
  state.model = "test-model";
  state.messages.push_back(message("system", "be careful"));
  state.messages.push_back(message("user", "fix the parser"));
  ModelMessage assistant = message("assistant", "reading");
  assistant.tool_calls.push_back(ToolCall{"call_1", "read_file", R"({"path":"a.txt"})"});
  state.messages.push_back(assistant);
  state.messages.push_back(toolResult("call_1", "contents"));

  REQUIRE(store->begin(state, error));
  REQUIRE_FALSE(state.id.empty());
  REQUIRE(bjorne_agent::session::isValidSessionId(state.id));
  REQUIRE(state.created > 0);
  REQUIRE(std::filesystem::exists(dir.str() + "/" + state.id + ".json"));

  bjorne_agent::session::SessionState loaded;
  REQUIRE(store->load(state.id, loaded, error));
  REQUIRE(loaded.id == state.id);
  REQUIRE(loaded.task == "fix the parser");
  REQUIRE(loaded.workspace == "/workspace");
  REQUIRE(loaded.model == "test-model");
  REQUIRE(loaded.status == "running");
  REQUIRE(loaded.messages.size() == 4);
  REQUIRE(loaded.messages[2].role == "assistant");
  REQUIRE(loaded.messages[2].tool_calls.size() == 1);
  REQUIRE(loaded.messages[2].tool_calls[0].id == "call_1");
  REQUIRE(loaded.messages[2].tool_calls[0].name == "read_file");
  REQUIRE(loaded.messages[2].tool_calls[0].arguments == R"({"path":"a.txt"})");
  REQUIRE(loaded.messages[3].tool_call_id == "call_1");
  REQUIRE(loaded.messages[3].content == "contents");
}

TEST_CASE("saving replaces the record and leaves no temporary file") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  bjorne_agent::session::SessionState state;
  state.task = "first";
  REQUIRE(store->begin(state, error));
  state.status = "complete";
  state.result = "finished";
  REQUIRE(store->save(state, error));

  REQUIRE_FALSE(std::filesystem::exists(dir.str() + "/" + state.id + ".json.tmp"));

  bjorne_agent::session::SessionState loaded;
  REQUIRE(store->load(state.id, loaded, error));
  REQUIRE(loaded.status == "complete");
  REQUIRE(loaded.result == "finished");
}

TEST_CASE("opening a store creates a missing directory") {
  TempDir dir;
  std::string error;
  const std::string nested = dir.str() + "/a/b/sessions";
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(nested, error);
  REQUIRE(store != nullptr);
  REQUIRE(std::filesystem::is_directory(nested));
}

TEST_CASE("opening a store rejects a path that is a file") {
  TempDir dir;
  const std::string path = dir.str() + "/sessions";
  writeFile(path, "not a directory");

  std::string error;
  REQUIRE(bjorne_agent::session::SessionStore::open(path, error) == nullptr);
  REQUIRE(error.find("not a directory") != std::string::npos);
}

TEST_CASE("an unsafe session id is refused") {
  for (const std::string& id : {std::string{}, std::string("."), std::string(".."), std::string("../escape"),
                                std::string("a/b"), std::string("a\\b"), std::string("with space")}) {
    REQUIRE_FALSE(bjorne_agent::session::isValidSessionId(id));
  }
  REQUIRE(bjorne_agent::session::isValidSessionId("1763072400123-1234-1"));
}

TEST_CASE("loading and saving reject an unsafe id") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  bjorne_agent::session::SessionState loaded;
  REQUIRE_FALSE(store->load("../escape", loaded, error));
  REQUIRE(error.find("Invalid session id") != std::string::npos);

  bjorne_agent::session::SessionState state;
  state.id = "../escape";
  REQUIRE_FALSE(store->save(state, error));
  REQUIRE(error.find("Invalid session id") != std::string::npos);
}

TEST_CASE("loading a missing session reports the id") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  bjorne_agent::session::SessionState state;
  REQUIRE_FALSE(store->load("no-such-session", state, error));
  REQUIRE(error.find("no-such-session") != std::string::npos);
}

TEST_CASE("a malformed session file is rejected") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  writeFile(dir.str() + "/broken.json", "{not json");
  bjorne_agent::session::SessionState state;
  REQUIRE_FALSE(store->load("broken", state, error));
  REQUIRE(error.find("Cannot parse session file") != std::string::npos);
}

TEST_CASE("an unsupported session version is rejected") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  writeFile(dir.str() + "/future.json", "{\"version\":99,\"id\":\"future\"}");
  bjorne_agent::session::SessionState state;
  REQUIRE_FALSE(store->load("future", state, error));
  REQUIRE(error.find("Unsupported session version") != std::string::npos);
}

TEST_CASE("the session file name is authoritative") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  writeFile(dir.str() + "/real.json", "{\"version\":1,\"id\":\"other\",\"status\":\"complete\"}");
  bjorne_agent::session::SessionState state;
  REQUIRE(store->load("real", state, error));
  REQUIRE(state.id == "real");
}

TEST_CASE("list is ordered by updated time, newest first") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  writeFile(dir.str() + "/older.json", summaryJson("older", 100, "interrupted", "old task"));
  writeFile(dir.str() + "/newer.json", summaryJson("newer", 200, "complete", "new task"));
  writeFile(dir.str() + "/broken.json", "{not json");
  writeFile(dir.str() + "/ignored.txt", "not a session");

  const std::vector<bjorne_agent::session::SessionSummary> summaries = store->list(error);
  REQUIRE(error.empty());
  REQUIRE(summaries.size() == 2);
  REQUIRE(summaries[0].id == "newer");
  REQUIRE(summaries[0].status == "complete");
  REQUIRE(summaries[0].task == "new task");
  REQUIRE(summaries[1].id == "older");
  REQUIRE(summaries[1].status == "interrupted");
}

TEST_CASE("latest returns the most recently updated session") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  REQUIRE(store->latest(error).empty());
  writeFile(dir.str() + "/older.json", summaryJson("older", 100, "error", "old"));
  writeFile(dir.str() + "/newer.json", summaryJson("newer", 200, "running", "new"));
  REQUIRE(store->latest(error) == "newer");
}

TEST_CASE("repair adds a result for a tool call that never finished") {
  std::vector<ModelMessage> messages;
  messages.push_back(message("user", "read both"));
  ModelMessage assistant = message("assistant", {});
  assistant.tool_calls.push_back(ToolCall{"call_1", "read_file", "{}"});
  assistant.tool_calls.push_back(ToolCall{"call_2", "read_file", "{}"});
  messages.push_back(assistant);
  messages.push_back(toolResult("call_1", "first"));

  REQUIRE(bjorne_agent::session::repairDanglingToolCalls(messages) == 1);
  REQUIRE(messages.size() == 4);
  REQUIRE(messages[2].tool_call_id == "call_1");
  REQUIRE(messages[3].role == "tool");
  REQUIRE(messages[3].tool_call_id == "call_2");
  REQUIRE(messages[3].content.find("Error") == 0);
}

TEST_CASE("repair leaves a complete tool turn alone") {
  std::vector<ModelMessage> messages;
  ModelMessage assistant = message("assistant", {});
  assistant.tool_calls.push_back(ToolCall{"call_1", "read_file", "{}"});
  messages.push_back(assistant);
  messages.push_back(toolResult("call_1", "contents"));

  REQUIRE(bjorne_agent::session::repairDanglingToolCalls(messages) == 0);
  REQUIRE(messages.size() == 2);
}

TEST_CASE("repair fills an assistant message at the end of the conversation") {
  std::vector<ModelMessage> messages;
  ModelMessage assistant = message("assistant", "working");
  assistant.tool_calls.push_back(ToolCall{"call_1", "run_shell", "{}"});
  assistant.tool_calls.push_back(ToolCall{"call_2", "run_shell", "{}"});
  messages.push_back(assistant);

  REQUIRE(bjorne_agent::session::repairDanglingToolCalls(messages) == 2);
  REQUIRE(messages.size() == 3);
  REQUIRE(messages[1].tool_call_id == "call_1");
  REQUIRE(messages[2].tool_call_id == "call_2");
}

TEST_CASE("repair inserts missing results before the next turn") {
  std::vector<ModelMessage> messages;
  ModelMessage assistant = message("assistant", {});
  assistant.tool_calls.push_back(ToolCall{"call_1", "read_file", "{}"});
  messages.push_back(assistant);
  messages.push_back(message("user", "are you there?"));

  REQUIRE(bjorne_agent::session::repairDanglingToolCalls(messages) == 1);
  REQUIRE(messages.size() == 3);
  REQUIRE(messages[1].role == "tool");
  REQUIRE(messages[1].tool_call_id == "call_1");
  REQUIRE(messages[2].role == "user");
}

TEST_CASE("repair ignores a plain conversation") {
  std::vector<ModelMessage> messages;
  messages.push_back(message("system", "be careful"));
  messages.push_back(message("user", "hello"));
  messages.push_back(message("assistant", "hi"));

  REQUIRE(bjorne_agent::session::repairDanglingToolCalls(messages) == 0);
  REQUIRE(messages.size() == 3);
}

TEST_CASE("timestamps are formatted in UTC") {
  REQUIRE(bjorne_agent::session::formatTimestamp(0) == "1970-01-01 00:00:00");
  REQUIRE(bjorne_agent::session::formatTimestamp(1763072400) == "2025-11-13 22:20:00");
}

TEST_CASE("generated session ids are unique and safe") {
  const std::string first = bjorne_agent::session::generateSessionId();
  const std::string second = bjorne_agent::session::generateSessionId();
  REQUIRE(first != second);
  REQUIRE(bjorne_agent::session::isValidSessionId(first));
  REQUIRE(bjorne_agent::session::isValidSessionId(second));
}

TEST_CASE("the recorder persists running, complete, error and interrupted states") {
  TempDir dir;
  std::string error;
  const std::shared_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  bjorne_agent::session::SessionState state;
  state.task = "persist me";
  REQUIRE(store->begin(state, error));

  bjorne_agent::session::SessionRecorder recorder(store, state);
  std::vector<ModelMessage> messages;
  messages.push_back(message("user", "persist me"));

  recorder.onTurn(messages, 1);
  bjorne_agent::session::SessionState loaded;
  REQUIRE(store->load(state.id, loaded, error));
  REQUIRE(loaded.status == "running");
  REQUIRE(loaded.turns == 1);
  REQUIRE(loaded.messages.size() == 1);

  recorder.onComplete(messages, "the answer");
  REQUIRE(store->load(state.id, loaded, error));
  REQUIRE(loaded.status == "complete");
  REQUIRE(loaded.result == "the answer");

  recorder.onFailure(messages, "backend failure");
  REQUIRE(store->load(state.id, loaded, error));
  REQUIRE(loaded.status == "error");
  REQUIRE(loaded.error == "backend failure");

  recorder.onInterrupt(messages);
  REQUIRE(store->load(state.id, loaded, error));
  REQUIRE(loaded.status == "interrupted");
}

TEST_CASE("a session file is plain JSON with the expected fields") {
  TempDir dir;
  std::string error;
  const std::unique_ptr<bjorne_agent::session::SessionStore> store =
      bjorne_agent::session::SessionStore::open(dir.str(), error);
  REQUIRE(store != nullptr);

  bjorne_agent::session::SessionState state;
  state.task = "a task";
  state.messages.push_back(message("user", "a task"));
  REQUIRE(store->begin(state, error));

  const std::string text = readFile(dir.str() + "/" + state.id + ".json");
  REQUIRE(text.find("\"version\":1") != std::string::npos);
  REQUIRE(text.find("\"task\":\"a task\"") != std::string::npos);
  REQUIRE(text.find("\"status\":\"running\"") != std::string::npos);
  REQUIRE(text.find("\"messages\":[") != std::string::npos);
}
