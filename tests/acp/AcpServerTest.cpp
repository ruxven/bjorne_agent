#include "acp/AcpServer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

#include "json/Json.h"
#include "jsonrpc/Endpoint.h"
#include "session/Session.h"

using bjorne_agent::json::Value;
using bjorne_agent::jsonrpc::Endpoint;
using bjorne_agent::jsonrpc::ReadResult;
using bjorne_agent::jsonrpc::Transport;

namespace {

// A model that answers with a scripted reply. The loop asks it for the next
// answer every time it needs one.
class ScriptedModel final : public bjorne_agent::model::ModelBackend {
 public:
  bool respond(const std::vector<bjorne_agent::model::ModelMessage>& messages,
               bjorne_agent::model::ModelResponse& out, std::string& error) override {
    std::unique_lock<std::mutex> guard(mutex_);
    // A held model waits for the test to let it answer, so a test can act
    // while the turn is in flight.
    if (holding_) {
      waiting_ = true;
      condition_.notify_all();
      condition_.wait(guard, [this] { return !holding_; });
      waiting_ = false;
    }
    seen.push_back(messages);
    if (fail_) {
      error = "The model is unreachable";
      return false;
    }
    if (responses.empty()) {
      out = bjorne_agent::model::ModelResponse();
      return true;
    }
    out = responses.front();
    responses.erase(responses.begin());
    return true;
  }

  // Makes every later answer fail.
  void fail() {
    std::lock_guard<std::mutex> guard(mutex_);
    fail_ = true;
  }

  // Makes the next answer wait until the test releases it.
  void hold() {
    std::lock_guard<std::mutex> guard(mutex_);
    holding_ = true;
  }

  void release() {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      holding_ = false;
    }
    condition_.notify_all();
  }

  // True once the model is waiting for a release.
  bool waiting() {
    std::lock_guard<std::mutex> guard(mutex_);
    return waiting_;
  }

  void setTools(const std::string& definitions) override {
    std::lock_guard<std::mutex> guard(mutex_);
    tools_json = definitions;
  }

  void push(bjorne_agent::model::ModelResponse response) {
    std::lock_guard<std::mutex> guard(mutex_);
    responses.push_back(std::move(response));
  }

  std::vector<bjorne_agent::model::ModelMessage> lastMessages() {
    std::lock_guard<std::mutex> guard(mutex_);
    return seen.empty() ? std::vector<bjorne_agent::model::ModelMessage>() : seen.back();
  }

  std::string toolsJson() {
    std::lock_guard<std::mutex> guard(mutex_);
    return tools_json;
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool holding_ = false;
  bool waiting_ = false;
  bool fail_ = false;
  std::vector<bjorne_agent::model::ModelResponse> responses;
  std::vector<std::vector<bjorne_agent::model::ModelMessage>> seen;
  std::string tools_json;
};

// The editor side of the connection. It records everything the agent wrote
// and answers the requests the agent makes, so the test drives the protocol
// the way an editor would.
class EditorTransport final : public Transport {
 public:
  bool sendLine(const std::string& line, std::string& error) override {
    if (fail_sends) {
      error = "The connection is broken";
      return false;
    }
    Value message = bjorne_agent::json::parse(line);
    std::lock_guard<std::mutex> guard(mutex_);
    written.push_back(message);

    const Value* method = message.find("method");
    const Value* id = message.find("id");
    if (method == nullptr || id == nullptr) {
      return true;
    }
    // A request from the agent. The editor answers it right away.
    requests.push_back(message);
    if (method->string == "session/request_permission" && !auto_reply_permission) {
      // The editor stays quiet, so the test can watch the agent wait.
      condition_.notify_all();
      return true;
    }
    const std::string reply = replyFor(method->string, message);
    if (!reply.empty()) {
      toAgent.push_back(reply);
    }
    condition_.notify_all();
    return true;
  }

  ReadResult readLine(int timeout_ms, std::string& line, std::string& error) override {
    (void)error;
    std::unique_lock<std::mutex> lock(mutex_);
    if (!toAgent.empty()) {
      line = toAgent.front();
      toAgent.pop_front();
      return ReadResult::Line;
    }
    condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !toAgent.empty(); });
    if (!toAgent.empty()) {
      line = toAgent.front();
      toAgent.pop_front();
      return ReadResult::Line;
    }
    return ReadResult::Timeout;
  }

  // The answer the editor gives to a request from the agent. The test can
  // change any of them.
  std::string permission_reply =
      R"({"jsonrpc":"2.0","id":%ID%,"result":{"outcome":{"outcome":"selected","optionId":"allow_once"}}})";
  std::string read_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"content":"file body"}})";
  std::string write_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{}})";
  std::string elicitation_reply =
      R"({"jsonrpc":"2.0","id":%ID%,"result":{"action":"accept","content":{"answer":"yes"}}})";
  std::string terminal_create_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"terminalId":"term-1"}})";
  std::string terminal_exit_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"exitCode":0}})";
  std::string terminal_output_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"output":"hello"}})";
  std::string terminal_release_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{}})";

  // When false the editor leaves a permission request unanswered, so the test
  // can watch the agent wait for a human.
  bool auto_reply_permission = true;

  // When true every write from the agent fails.
  bool fail_sends = false;

  // The methods the editor answers with an error instead of a result.
  std::vector<std::string> error_methods;

  // Sends a request from the editor to the agent.
  void send(const std::string& method, const std::string& params, int id) {
    const std::string line = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")" + method +
                             R"(","params":)" + params + "}";
    {
      std::lock_guard<std::mutex> guard(mutex_);
      toAgent.push_back(line);
    }
    condition_.notify_all();
  }

  // Sends a notification from the editor to the agent.
  void sendNotification(const std::string& method, const std::string& params) {
    const std::string line = R"({"jsonrpc":"2.0","method":")" + method + R"(","params":)" + params + "}";
    {
      std::lock_guard<std::mutex> guard(mutex_);
      toAgent.push_back(line);
    }
    condition_.notify_all();
  }

  // Waits for the answer to a request the editor made. The agent sends its own
  // requests to the editor with ids from the same small range, so a message
  // that carries a method is not an answer and is skipped.
  Value awaitResponse(int id, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const Value& message : written) {
          const Value* found = message.find("id");
          if (found != nullptr && found->isNumber() && static_cast<int>(found->number) == id &&
              message.find("method") == nullptr) {
            return message;
          }
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return Value();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // The session updates the agent sent.
  std::vector<Value> updates() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Value> found;
    for (const Value& message : written) {
      const Value* method = message.find("method");
      if (method != nullptr && method->isString() && method->string == "session/update") {
        found.push_back(message);
      }
    }
    return found;
  }

  // Records that the agent handled a notification. The test waits for it, so
  // it knows the agent has seen the message before it acts.
  void noteNotification(const std::string& method) {
    std::lock_guard<std::mutex> guard(mutex_);
    handled.push_back(method);
  }

  bool notificationHandled(const std::string& method) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (const std::string& seen : handled) {
      if (seen == method) {
        return true;
      }
    }
    return false;
  }

  // The requests the agent made to the editor.
  std::vector<Value> agentRequests() {
    std::lock_guard<std::mutex> guard(mutex_);
    return requests;
  }

 private:
  std::string replyFor(const std::string& method, const Value& message) {
    std::string body;
    const Value* id = message.find("id");
    const std::string identifier = std::to_string(static_cast<int>(id->number));

    // The editor can refuse a request, so the agent sees a failure.
    for (const std::string& refused : error_methods) {
      if (refused == method) {
        return R"({"jsonrpc":"2.0","id":)" + identifier + R"(,"error":{"code":-32000,"message":"The editor refused"}})";
      }
    }

    if (method == "session/request_permission") {
      body = permission_reply;
    } else if (method == "fs/read_text_file") {
      body = read_reply;
    } else if (method == "fs/write_text_file") {
      body = write_reply;
    } else if (method == "elicitation/create") {
      body = elicitation_reply;
    } else if (method == "terminal/create") {
      body = terminal_create_reply;
    } else if (method == "terminal/wait_for_exit") {
      body = terminal_exit_reply;
    } else if (method == "terminal/output") {
      body = terminal_output_reply;
    } else if (method == "terminal/release") {
      body = terminal_release_reply;
    } else {
      return {};
    }
    size_t at = body.find("%ID%");
    while (at != std::string::npos) {
      body.replace(at, 4, identifier);
      at = body.find("%ID%");
    }
    return body;
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::string> toAgent;
  std::vector<Value> written;
  std::vector<Value> requests;
  std::vector<std::string> handled;
};

// A temporary directory that cleans itself up.
class TempDirectory {
 public:
  TempDirectory() {
    directory_ = std::filesystem::temp_directory_path() /
                 ("bjorne_acp_" + std::to_string(
                                     std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count()));
    std::filesystem::create_directories(directory_);
    // std::filesystem::path does not convert to std::string implicitly on
    // every toolchain, so cross it once here.
    path_ = directory_.string();
  }
  ~TempDirectory() {
    std::error_code code;
    std::filesystem::remove_all(directory_, code);
  }
  const std::string& path() const { return path_; }

 private:
  std::filesystem::path directory_;
  std::string path_;
};

// Everything a test needs to talk to an agent.
struct Harness {
  std::shared_ptr<EditorTransport> transport;
  std::shared_ptr<Endpoint> endpoint;
  std::shared_ptr<bjorne_agent::acp::AcpServer> server;
  std::shared_ptr<ScriptedModel> model;
  std::shared_ptr<bjorne_agent::session::SessionStore> store;
  // The runtime requests the server made, so a test can see what it asked for.
  std::shared_ptr<std::vector<bjorne_agent::acp::RuntimeRequest>> runtimeRequests;
  // When set, the runtime cannot be built. The test flips it to make the
  // agent fail the way it would when the model cannot be reached.
  std::shared_ptr<std::atomic<bool>> factory_fails;
  // The message the failed build reports. An empty message leaves the agent
  // to make one up.
  std::shared_ptr<std::string> factory_error;
  std::string directory;
};

// Builds an agent that talks over the in-memory transport, with a chosen set
// of models the editor may pick from.
Harness makeHarness(const std::string& directory, bool with_store,
                    const std::vector<std::string>& models) {
  Harness harness;
  harness.directory = directory;
  harness.transport = std::make_shared<EditorTransport>();
  harness.endpoint = std::make_shared<Endpoint>(harness.transport);
  harness.model = std::make_shared<ScriptedModel>();
  harness.runtimeRequests = std::make_shared<std::vector<bjorne_agent::acp::RuntimeRequest>>();
  harness.factory_fails = std::make_shared<std::atomic<bool>>(false);
  harness.factory_error = std::make_shared<std::string>();

  std::shared_ptr<bjorne_agent::session::SessionStore> store;
  if (with_store) {
    std::string error;
    store = bjorne_agent::session::SessionStore::open(directory, error);
  }

  const auto factory = [harness](const bjorne_agent::acp::RuntimeRequest& request,
                                 bjorne_agent::acp::SessionRuntime& runtime, std::string& error) {
    harness.runtimeRequests->push_back(request);
    if (harness.factory_fails->load()) {
      error = harness.factory_error->empty() ? "" : *harness.factory_error;
      return false;
    }
    runtime.model = harness.model;
    runtime.context = std::make_shared<bjorne_agent::context::ContextManager>(
        bjorne_agent::context::ContextManagerConfig());
    runtime.tools = std::make_shared<bjorne_agent::tools::ToolRegistry>(request.workspace);
    runtime.system_prompt = "You are the agent.";
    return true;
  };

  harness.server = std::make_shared<bjorne_agent::acp::AcpServer>(harness.endpoint, store, factory, models, "model-a");
  harness.endpoint->setRequestHandler([harness](const std::string& method, const Value& params, Value& result,
                                                bjorne_agent::jsonrpc::Error& error) {
    return harness.server->handle(method, params, result, error);
  });
  harness.endpoint->setNotificationHandler([harness](const std::string& method, const Value& params) {
    harness.transport->noteNotification(method);
    harness.server->notify(method, params);
  });
  harness.endpoint->start();
  return harness;
}

// Builds an agent that talks over the in-memory transport.
Harness makeHarness(const std::string& directory, bool with_store) {
  return makeHarness(directory, with_store, {"model-a", "model-b"});
}

// The initialize handshake, with the capabilities the editor advertises.
const char* kInitializeParams =
    R"({"protocolVersion":1,"clientCapabilities":{"fs":{"readTextFile":true,"writeTextFile":true},)"
    R"("terminal":true,"elicitation":{"form":{}}},"clientInfo":{"name":"editor","version":"1.0"}})";

}  // namespace

TEST_CASE("initialize negotiates the protocol version") {
  Harness harness = makeHarness("/tmp", false);

  harness.transport->send("initialize", kInitializeParams, 1);
  const Value response = harness.transport->awaitResponse(1);
  const Value* result = response.find("result");
  REQUIRE(result != nullptr);
  REQUIRE(static_cast<int>(result->find("protocolVersion")->number) == 1);
  REQUIRE(result->find("agentInfo")->find("name")->string == "bjorne_agent");
}

TEST_CASE("initialize answers an unsupported version with the latest one") {
  Harness harness = makeHarness("/tmp", false);

  harness.transport->send("initialize", R"({"protocolVersion":99})", 1);
  const Value response = harness.transport->awaitResponse(1);
  const Value* result = response.find("result");
  REQUIRE(result != nullptr);
  REQUIRE(static_cast<int>(result->find("protocolVersion")->number) == bjorne_agent::acp::AcpServer::kProtocolVersion);
}

TEST_CASE("the capabilities follow the configuration") {
  TempDirectory directory;
  Harness withStore = makeHarness(directory.path(), true);
  withStore.transport->send("initialize", kInitializeParams, 1);
  const Value stored = withStore.transport->awaitResponse(1);
  const Value* storedResult = stored.find("result");
  REQUIRE(storedResult != nullptr);
  const Value* capabilities = storedResult->find("agentCapabilities");
  REQUIRE(capabilities != nullptr);
  REQUIRE(capabilities->find("loadSession")->boolean == true);
  REQUIRE(capabilities->find("sessionCapabilities")->find("list") != nullptr);
  REQUIRE(capabilities->find("mcpCapabilities")->find("http")->boolean == false);

  Harness withoutStore = makeHarness("/tmp", false);
  withoutStore.transport->send("initialize", kInitializeParams, 1);
  const Value plain = withoutStore.transport->awaitResponse(1);
  const Value* plainResult = plain.find("result");
  REQUIRE(plainResult != nullptr);
  REQUIRE(plainResult->find("agentCapabilities")->find("loadSession")->boolean == false);
  REQUIRE(plainResult->find("agentCapabilities")->find("sessionCapabilities") == nullptr);
}

TEST_CASE("a request before initialize is refused") {
  Harness harness = makeHarness("/tmp", false);

  harness.transport->send("session/new", R"({"cwd":"/tmp"})", 1);
  const Value response = harness.transport->awaitResponse(1);
  REQUIRE(response.find("error") != nullptr);
  REQUIRE(static_cast<int>(response.find("error")->find("code")->number) == -32600);
}

TEST_CASE("a session is created and the prompt reaches the model") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/new", R"({"cwd":"/tmp/work","mcpServers":[]})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const Value* result = created.find("result");
  REQUIRE(result != nullptr);
  const std::string session_id = result->find("sessionId")->string;
  REQUIRE_FALSE(session_id.empty());

  bjorne_agent::model::ModelResponse answer;
  answer.content = "The answer is 42";
  harness.model->push(answer);

  harness.transport->send("session/prompt", R"({"sessionId":")" + session_id +
                                                R"(","prompt":[{"type":"text","text":"What is the answer?"}]})",
                          3);
  const Value answered = harness.transport->awaitResponse(3);
  REQUIRE(answered.find("result") != nullptr);
  REQUIRE(answered.find("result")->find("stopReason")->string == "end_turn");

  const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].role == "system");
  REQUIRE(messages[1].content == "What is the answer?");
}

TEST_CASE("the agent answer arrives as a message chunk") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse answer;
  answer.content = "All done";
  harness.model->push(answer);
  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"go"}]})", 3);
  harness.transport->awaitResponse(3);

  bool found = false;
  for (const Value& update : harness.transport->updates()) {
    const Value* body = update.find("params")->find("update");
    if (body->find("sessionUpdate")->string == "agent_message_chunk" &&
        body->find("content")->find("text")->string == "All done") {
      found = true;
    }
  }
  REQUIRE(found);
}

TEST_CASE("a tool call asks the editor for permission") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Wrote the file";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write it"}]})", 3);
  const Value answered = harness.transport->awaitResponse(3);
  REQUIRE(answered.find("result") != nullptr);

  bool asked = false;
  for (const Value& request : harness.transport->agentRequests()) {
    if (request.find("method")->string == "session/request_permission") {
      asked = true;
      REQUIRE(request.find("params")->find("toolCall")->find("toolCallId")->string == "call-1");
    }
  }
  REQUIRE(asked);

  bool announced = false;
  for (const Value& update : harness.transport->updates()) {
    const Value* body = update.find("params")->find("update");
    if (body->find("sessionUpdate")->string == "tool_call" && body->find("toolCallId")->string == "call-1") {
      announced = true;
    }
  }
  REQUIRE(announced);
}

TEST_CASE("a denied permission ends the turn as a refusal") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->permission_reply =
      R"({"jsonrpc":"2.0","id":%ID%,"result":{"outcome":{"outcome":"selected","optionId":"reject_once"}}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt"})"});
  harness.model->push(withTool);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write it"}]})", 3);
  const Value answered = harness.transport->awaitResponse(3);
  REQUIRE(answered.find("result") != nullptr);
  REQUIRE(answered.find("result")->find("stopReason")->string == "refusal");
}

TEST_CASE("a cancelled turn answers with the cancelled stop reason") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  // The editor never answers the permission request, so the turn is waiting
  // when the cancellation arrives.
  harness.transport->permission_reply.clear();
  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt"})"});
  harness.model->push(withTool);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write it"}]})", 3);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  harness.transport->sendNotification("session/cancel", R"({"sessionId":")" + session_id + "\"}");

  const Value answered = harness.transport->awaitResponse(3, 5000);
  REQUIRE(answered.find("result") != nullptr);
  REQUIRE(answered.find("result")->find("stopReason")->string == "cancelled");
}

TEST_CASE("a file is read through the editor") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", "read_file", R"({"path":"notes.txt"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Read it";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"read"}]})", 3);
  harness.transport->awaitResponse(3);

  bool asked = false;
  for (const Value& request : harness.transport->agentRequests()) {
    if (request.find("method")->string == "fs/read_text_file") {
      asked = true;
      REQUIRE(request.find("params")->find("path")->string == "/tmp/work/notes.txt");
    }
  }
  REQUIRE(asked);

  const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
  REQUIRE(messages.back().role == "tool");
  REQUIRE(messages.back().content == "file body");
}

TEST_CASE("a session is stored, listed and deleted") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse answer;
  answer.content = "Done";
  harness.model->push(answer);
  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"work"}]})", 3);
  harness.transport->awaitResponse(3);

  harness.transport->send("session/list", R"({"cwd":"/tmp/work"})", 4);
  const Value listed = harness.transport->awaitResponse(4);
  const Value* sessions = listed.find("result")->find("sessions");
  REQUIRE(sessions != nullptr);
  REQUIRE(sessions->isArray());
  REQUIRE(sessions->array.size() == 1);
  REQUIRE(sessions->array[0].find("sessionId")->string == session_id);
  REQUIRE(sessions->array[0].find("cwd")->string == "/tmp/work");
  REQUIRE_FALSE(sessions->array[0].find("updatedAt")->string.empty());

  harness.transport->send("session/delete", R"({"sessionId":")" + session_id + "\"}", 5);
  const Value deleted = harness.transport->awaitResponse(5);
  REQUIRE(deleted.find("result") != nullptr);

  harness.transport->send("session/list", R"({})", 6);
  const Value empty = harness.transport->awaitResponse(6);
  REQUIRE(empty.find("result")->find("sessions")->array.empty());
}

TEST_CASE("loading a session replays the conversation") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse answer;
  answer.content = "The first answer";
  harness.model->push(answer);
  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"first question"}]})", 3);
  harness.transport->awaitResponse(3);

  harness.transport->send("session/load", R"({"sessionId":")" + session_id + R"(","cwd":"/tmp/work"})", 4);
  const Value loaded = harness.transport->awaitResponse(4);
  REQUIRE(loaded.find("result") != nullptr);

  bool user = false;
  bool agent = false;
  for (const Value& update : harness.transport->updates()) {
    const Value* body = update.find("params")->find("update");
    const std::string kind = body->find("sessionUpdate")->string;
    if (kind == "user_message_chunk" && body->find("content")->find("text")->string == "first question") {
      user = true;
    }
    if (kind == "agent_message_chunk" && body->find("content")->find("text")->string == "The first answer") {
      agent = true;
    }
  }
  REQUIRE(user);
  REQUIRE(agent);
}

TEST_CASE("resuming a session does not replay the conversation") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse answer;
  answer.content = "The first answer";
  harness.model->push(answer);
  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"first question"}]})", 3);
  harness.transport->awaitResponse(3);

  const std::size_t before = harness.transport->updates().size();
  harness.transport->send("session/resume", R"({"sessionId":")" + session_id + R"(","cwd":"/tmp/work"})", 4);
  const Value resumed = harness.transport->awaitResponse(4);
  REQUIRE(resumed.find("result") != nullptr);
  REQUIRE(harness.transport->updates().size() == before);
}

TEST_CASE("the model of a session can be changed") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  harness.transport->send("session/set_config_option",
                          R"({"sessionId":")" + session_id + R"(","configId":"model","value":"model-b"})", 3);
  const Value changed = harness.transport->awaitResponse(3);
  REQUIRE(changed.find("result") != nullptr);
  const Value* options = changed.find("result")->find("configOptions");
  REQUIRE(options != nullptr);
  REQUIRE(options->array[0].find("currentValue")->string == "model-b");
}

TEST_CASE("an unknown model is refused") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  harness.transport->send("session/set_config_option",
                          R"({"sessionId":")" + session_id + R"(","configId":"model","value":"model-z"})", 3);
  const Value refused = harness.transport->awaitResponse(3);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32602);
}

TEST_CASE("a prompt for an unknown session is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/prompt", R"({"sessionId":"sess_nope","prompt":[{"type":"text","text":"hi"}]})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32002);
  REQUIRE(refused.find("error")->find("data")->find("sessionId")->string == "sess_nope");
}

TEST_CASE("an unsupported prompt block is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"image","data":"xx"}]})", 3);
  const Value refused = harness.transport->awaitResponse(3);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32602);
}

TEST_CASE("a resource link reaches the model as text") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse answer;
  answer.content = "Seen";
  harness.model->push(answer);
  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id +
                              R"(","prompt":[{"type":"resource_link","uri":"file:///a.txt","name":"a.txt"}]})",
                          3);
  harness.transport->awaitResponse(3);

  const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
  REQUIRE(messages.back().content.find("a.txt") != std::string::npos);
}

TEST_CASE("the agent asks the editor through elicitation") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(
      bjorne_agent::model::ToolCall{"call-1", "request_input", R"({"question":"Proceed?"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Asked";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"ask"}]})", 3);
  harness.transport->awaitResponse(3);

  bool asked = false;
  for (const Value& request : harness.transport->agentRequests()) {
    if (request.find("method")->string == "elicitation/create") {
      asked = true;
      REQUIRE(request.find("params")->find("mode")->string == "form");
    }
  }
  REQUIRE(asked);

  const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
  REQUIRE(messages.back().role == "tool");
  REQUIRE(messages.back().content == "yes");
}

TEST_CASE("a file is written through the editor") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{
      "call-1", "write_file", R"({"path":"notes.txt","content":"hello there"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Written";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write"}]})", 3);
  harness.transport->awaitResponse(3);

  bool asked = false;
  for (const Value& request : harness.transport->agentRequests()) {
    if (request.find("method")->string == "fs/write_text_file") {
      asked = true;
      REQUIRE(request.find("params")->find("path")->string == "/tmp/work/notes.txt");
      REQUIRE(request.find("params")->find("content")->string == "hello there");
    }
  }
  REQUIRE(asked);

  const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
  REQUIRE(messages.back().role == "tool");
  REQUIRE(messages.back().content == "Wrote /tmp/work/notes.txt");
}

TEST_CASE("a shell command runs in the editor terminal") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", "run_shell", R"({"command":"echo hi"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Ran";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"run"}]})", 3);
  harness.transport->awaitResponse(3);

  bool created_terminal = false;
  bool waited = false;
  bool read = false;
  bool released = false;
  for (const Value& request : harness.transport->agentRequests()) {
    const std::string method = request.find("method")->string;
    if (method == "terminal/create") {
      created_terminal = true;
      REQUIRE(request.find("params")->find("cwd")->string == "/tmp/work");
    } else if (method == "terminal/wait_for_exit") {
      waited = true;
    } else if (method == "terminal/output") {
      read = true;
    } else if (method == "terminal/release") {
      released = true;
    }
  }
  REQUIRE(created_terminal);
  REQUIRE(waited);
  REQUIRE(read);
  REQUIRE(released);

  const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
  REQUIRE(messages.back().role == "tool");
  REQUIRE(messages.back().content.find("hello") != std::string::npos);
}

TEST_CASE("a session without a cwd is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/new", R"({})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32602);
}

TEST_CASE("the session methods need a session store") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/list", R"({})", 2);
  const Value listed = harness.transport->awaitResponse(2);
  REQUIRE(listed.find("error") != nullptr);
  REQUIRE(static_cast<int>(listed.find("error")->find("code")->number) == -32601);
}

TEST_CASE("an unknown method is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/nonsense", R"({})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32601);
}

TEST_CASE("an MCP server the editor named reaches the runtime") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send(
      "session/new",
      R"({"cwd":"/tmp/work","mcpServers":[{"name":"files","command":"/bin/true","args":["--fast"],)"
      R"("env":[{"name":"KEY","value":"VALUE"}]}]})",
      2);
  const Value created = harness.transport->awaitResponse(2);
  REQUIRE(created.find("result") != nullptr);

  REQUIRE(harness.runtimeRequests->size() == 1);
  const bjorne_agent::acp::RuntimeRequest& request = harness.runtimeRequests->front();
  REQUIRE(request.workspace == "/tmp/work");
  REQUIRE(request.mcp_servers.size() == 1);
  REQUIRE(request.mcp_servers.at("files").command == "/bin/true");
  REQUIRE(request.mcp_servers.at("files").args.size() == 1);
  REQUIRE(request.mcp_servers.at("files").env.at("KEY") == "VALUE");
}

TEST_CASE("a malformed MCP server entry is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/new", R"({"cwd":"/tmp/work","mcpServers":"nope"})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32602);
}

TEST_CASE("an MCP server without a command is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/new", R"({"cwd":"/tmp/work","mcpServers":[{"name":"files"}]})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32602);
}

TEST_CASE("an allowed tool is not asked about twice") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->permission_reply =
      R"({"jsonrpc":"2.0","id":%ID%,"result":{"outcome":{"outcome":"selected","optionId":"allow_always"}}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Done";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write"}]})", 3);
  harness.transport->awaitResponse(3);

  int asked = 0;
  for (const Value& request : harness.transport->agentRequests()) {
    if (request.find("method")->string == "session/request_permission") {
      ++asked;
    }
  }
  REQUIRE(asked == 1);

  // The second turn uses the same tool. The editor is not asked again.
  bjorne_agent::model::ModelResponse withTool2;
  withTool2.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-2", "write_file", R"({"path":"b.txt"})"});
  harness.model->push(withTool2);
  bjorne_agent::model::ModelResponse answer2;
  answer2.content = "Done again";
  harness.model->push(answer2);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write more"}]})", 4);
  harness.transport->awaitResponse(4);

  asked = 0;
  for (const Value& request : harness.transport->agentRequests()) {
    if (request.find("method")->string == "session/request_permission") {
      ++asked;
    }
  }
  REQUIRE(asked == 1);
}

TEST_CASE("the plan and the tool status are announced") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(
      bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt","content":"body"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Done";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write"}]})", 3);
  harness.transport->awaitResponse(3);

  bool planned = false;
  bool in_progress = false;
  bool completed = false;
  for (const Value& update : harness.transport->updates()) {
    const Value* body = update.find("params")->find("update");
    const std::string kind = body->find("sessionUpdate")->string;
    if (kind == "plan" && !body->find("entries")->array.empty()) {
      planned = true;
    }
    if (kind == "tool_call_update" && body->find("status")->string == "in_progress") {
      in_progress = true;
    }
    if (kind == "tool_call_update" && body->find("status")->string == "completed") {
      completed = true;
    }
  }
  REQUIRE(planned);
  REQUIRE(in_progress);
  REQUIRE(completed);
}

TEST_CASE("loading an unknown session is refused") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/load", R"({"sessionId":"sess_nope"})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32002);
}

TEST_CASE("deleting a session that is not there succeeds") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  // The store removes the file when it is there and does nothing when it is
  // not, so the editor does not have to know the difference.
  harness.transport->send("session/delete", R"({"sessionId":"sess_nope"})", 2);
  const Value deleted = harness.transport->awaitResponse(2);
  REQUIRE(deleted.find("result") != nullptr);
}

TEST_CASE("deleting a session with an unusable id is refused") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/delete", R"({"sessionId":"../escape"})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32602);
}

TEST_CASE("a configuration option other than the model is refused") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  harness.transport->send("session/set_config_option",
                          R"({"sessionId":")" + session_id + R"(","configId":"mode","value":"fast"})", 3);
  const Value refused = harness.transport->awaitResponse(3);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32602);
}

TEST_CASE("a resource block reaches the model as text") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse answer;
  answer.content = "Seen";
  harness.model->push(answer);
  harness.transport->send(
      "session/prompt",
      R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"resource","resource":{"text":"embedded body"}}]})", 3);
  harness.transport->awaitResponse(3);

  const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
  REQUIRE(messages.back().content == "embedded body");
}

TEST_CASE("a prompt block without a type is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"text":"no type"}]})", 3);
  const Value refused = harness.transport->awaitResponse(3);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32602);
}

TEST_CASE("a prompt without a text block is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text"}]})", 3);
  const Value refused = harness.transport->awaitResponse(3);
  REQUIRE(refused.find("error") != nullptr);
  REQUIRE(static_cast<int>(refused.find("error")->find("code")->number) == -32602);
}

namespace {

// The text of the last message of the given kind the agent sent.
std::string lastMessageText(const std::vector<Value>& updates, const std::string& kind) {
  std::string text;
  for (const Value& update : updates) {
    const Value* body = update.find("params")->find("update");
    const Value* found = body->find("sessionUpdate");
    if (found == nullptr || found->string != kind) {
      continue;
    }
    const Value* content = body->find("content");
    if (content == nullptr) {
      continue;
    }
    const Value* value = content->find("text");
    if (value != nullptr && value->isString()) {
      text = value->string;
    }
  }
  return text;
}

// The error code of a refused request.
int errorCode(const Value& response) {
  const Value* error = response.find("error");
  if (error == nullptr) {
    return 0;
  }
  const Value* code = error->find("code");
  return code != nullptr ? static_cast<int>(code->number) : 0;
}

// Drives one tool call and returns the text the tool produced.
std::string runTool(const Harness& harness, const std::string& session_id, const std::string& tool,
                    const std::string& arguments, int id = 9) {
  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", tool, arguments});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Done";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"go"}]})", id);
  harness.transport->awaitResponse(id);
  const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
  return messages.back().content;
}

// Makes a session and returns its id.
std::string makeSession(const Harness& harness) {
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  return created.find("result")->find("sessionId")->string;
}

}  // namespace

TEST_CASE("a second initialize is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("initialize", kInitializeParams, 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(errorCode(refused) == bjorne_agent::jsonrpc::kInvalidRequest);
}

TEST_CASE("session/load needs a session store") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/load", R"({"sessionId":"sess_x"})", 2);
  REQUIRE(errorCode(harness.transport->awaitResponse(2)) == bjorne_agent::jsonrpc::kMethodNotFound);
}

TEST_CASE("session/delete needs a session store") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/delete", R"({"sessionId":"sess_x"})", 2);
  REQUIRE(errorCode(harness.transport->awaitResponse(2)) == bjorne_agent::jsonrpc::kMethodNotFound);
}

TEST_CASE("session/delete without a session id is refused") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/delete", R"({})", 2);
  REQUIRE(errorCode(harness.transport->awaitResponse(2)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("session/prompt without a session id is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/prompt", R"({"prompt":[{"type":"text","text":"go"}]})", 2);
  REQUIRE(errorCode(harness.transport->awaitResponse(2)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("a prompt for a session that is not there is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/prompt",
                          R"({"sessionId":"sess_nope","prompt":[{"type":"text","text":"go"}]})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(errorCode(refused) == bjorne_agent::acp::kResourceNotFound);
  REQUIRE(refused.find("error")->find("data")->find("sessionId")->string == "sess_nope");
}

TEST_CASE("session/set_config_option without a session id is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/set_config_option", R"({"configId":"model","value":"model-a"})", 2);
  REQUIRE(errorCode(harness.transport->awaitResponse(2)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("a configuration option for an unknown session is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/set_config_option",
                          R"({"sessionId":"sess_nope","configId":"model","value":"model-a"})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(errorCode(refused) == bjorne_agent::acp::kResourceNotFound);
}

TEST_CASE("the model option takes a string value") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  harness.transport->send("session/set_config_option",
                          R"({"sessionId":")" + session_id + R"(","configId":"model","value":7})", 3);
  REQUIRE(errorCode(harness.transport->awaitResponse(3)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("a model that is not on the list is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  harness.transport->send("session/set_config_option",
                          R"({"sessionId":")" + session_id + R"(","configId":"model","value":"model-z"})", 3);
  REQUIRE(errorCode(harness.transport->awaitResponse(3)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("the model option changes the session") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  harness.transport->send("session/set_config_option",
                          R"({"sessionId":")" + session_id + R"(","configId":"model","value":"model-b"})", 3);
  const Value changed = harness.transport->awaitResponse(3);
  REQUIRE(changed.find("result") != nullptr);
  const Value* options = changed.find("result")->find("configOptions");
  REQUIRE(options != nullptr);
  REQUIRE(options->array.at(0).find("currentValue")->string == "model-b");

  // The runtime was rebuilt for the new model.
  REQUIRE(harness.runtimeRequests->size() == 2);
  REQUIRE(harness.runtimeRequests->back().model == "model-b");
}

TEST_CASE("session/list refuses a cwd that is not a string") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/list", R"({"cwd":7})", 2);
  REQUIRE(errorCode(harness.transport->awaitResponse(2)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("session/list filters by the workspace") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  makeSession(harness);

  harness.transport->send("session/list", R"({"cwd":"/tmp/elsewhere"})", 3);
  const Value listed = harness.transport->awaitResponse(3);
  REQUIRE(listed.find("result")->find("sessions")->array.empty());

  harness.transport->send("session/list", R"({"cwd":"/tmp/work"})", 4);
  const Value matching = harness.transport->awaitResponse(4);
  REQUIRE(matching.find("result")->find("sessions")->array.size() == 1);
}

TEST_CASE("a prompt that carries no text is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":""}]})", 3);
  REQUIRE(errorCode(harness.transport->awaitResponse(3)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("the read tool wants a path") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "read_file", "{}");
  REQUIRE(answer.find("read_file requires path") != std::string::npos);
}

TEST_CASE("the write tool wants a JSON object") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "write_file", "not json");
  REQUIRE(answer.find("write_file requires a JSON object") != std::string::npos);
}

TEST_CASE("the write tool wants a path") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "write_file", R"({"content":"body"})");
  REQUIRE(answer.find("write_file requires path") != std::string::npos);
}

TEST_CASE("the write tool wants content") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "write_file", R"({"path":"a.txt"})");
  REQUIRE(answer.find("write_file requires content") != std::string::npos);
}

TEST_CASE("the shell tool wants a command") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "run_shell", "{}");
  REQUIRE(answer.find("run_shell requires command") != std::string::npos);
}

TEST_CASE("the ask tool wants a question") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "request_input", "{}");
  REQUIRE(answer.find("request_input requires question") != std::string::npos);
}

TEST_CASE("the ask tool wants non-empty options") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer =
      runTool(harness, session_id, "request_input", R"({"question":"Which?","options":["a",""]})");
  REQUIRE(answer.find("request_input options must be non-empty strings") != std::string::npos);
}

TEST_CASE("the read tool wants a response with content") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->read_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "read_file", R"({"path":"a.txt"})");
  REQUIRE(answer.find("carries no content") != std::string::npos);
}

TEST_CASE("the terminal needs an id") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->terminal_create_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "run_shell", R"({"command":"echo hi"})");
  REQUIRE(answer.find("carries no terminalId") != std::string::npos);
}

TEST_CASE("the terminal output may be empty") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->terminal_output_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "run_shell", R"({"command":"echo hi"})");
  REQUIRE(answer.find("Exit code: 0") != std::string::npos);
}

TEST_CASE("a question the editor did not answer") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->elicitation_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"action":"decline"}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "request_input", R"({"question":"Which?"})");
  REQUIRE(answer.find("did not answer the question") != std::string::npos);
}

TEST_CASE("an answer without content") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->elicitation_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"action":"accept"}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "request_input", R"({"question":"Which?"})");
  REQUIRE(answer.find("carries no content") != std::string::npos);
}

TEST_CASE("an answer without an answer") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->elicitation_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"action":"accept","content":{}}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "request_input", R"({"question":"Which?"})");
  REQUIRE(answer.find("carries no answer") != std::string::npos);
}

TEST_CASE("a permission response without an outcome") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->permission_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(
      bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt","content":"body"})"});
  harness.model->push(withTool);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write"}]})", 3);
  const Value response = harness.transport->awaitResponse(3);
  REQUIRE(response.find("result")->find("stopReason")->string == "refusal");
  REQUIRE(lastMessageText(harness.transport->updates(), "agent_message_chunk").find("no outcome") !=
          std::string::npos);
}

TEST_CASE("a permission response without an outcome kind") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->permission_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"outcome":{}}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(
      bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt","content":"body"})"});
  harness.model->push(withTool);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write"}]})", 3);
  const Value response = harness.transport->awaitResponse(3);
  REQUIRE(response.find("result")->find("stopReason")->string == "refusal");
  REQUIRE(lastMessageText(harness.transport->updates(), "agent_message_chunk").find("outcome kind") !=
          std::string::npos);
}

TEST_CASE("a cancelled permission request") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->permission_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"outcome":{"outcome":"cancelled"}}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(
      bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt","content":"body"})"});
  harness.model->push(withTool);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write"}]})", 3);
  const Value response = harness.transport->awaitResponse(3);
  REQUIRE(response.find("result")->find("stopReason")->string == "refusal");
  REQUIRE(lastMessageText(harness.transport->updates(), "agent_message_chunk").find("cancelled") !=
          std::string::npos);
}

TEST_CASE("a permission outcome that is not understood") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->permission_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"outcome":{"outcome":"maybe"}}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(
      bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt","content":"body"})"});
  harness.model->push(withTool);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write"}]})", 3);
  const Value response = harness.transport->awaitResponse(3);
  REQUIRE(response.find("result")->find("stopReason")->string == "refusal");
  REQUIRE(lastMessageText(harness.transport->updates(), "agent_message_chunk").find("not understood") !=
          std::string::npos);
}

TEST_CASE("a permission response without an option") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->permission_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"outcome":{"outcome":"selected"}}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(
      bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt","content":"body"})"});
  harness.model->push(withTool);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write"}]})", 3);
  const Value response = harness.transport->awaitResponse(3);
  REQUIRE(response.find("result")->find("stopReason")->string == "refusal");
  REQUIRE(lastMessageText(harness.transport->updates(), "agent_message_chunk").find("names no option") !=
          std::string::npos);
}

TEST_CASE("a denied permission request ends the turn") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->permission_reply =
      R"({"jsonrpc":"2.0","id":%ID%,"result":{"outcome":{"outcome":"selected","optionId":"reject_once"}}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(
      bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt","content":"body"})"});
  harness.model->push(withTool);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write"}]})", 3);
  const Value response = harness.transport->awaitResponse(3);
  REQUIRE(response.find("result")->find("stopReason")->string == "refusal");
  REQUIRE(lastMessageText(harness.transport->updates(), "agent_message_chunk").find("did not allow") !=
          std::string::npos);
}

TEST_CASE("a model failure ends the turn") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  harness.model->fail();
  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"go"}]})", 3);
  const Value response = harness.transport->awaitResponse(3);
  REQUIRE(errorCode(response) == bjorne_agent::jsonrpc::kInternalError);
  REQUIRE(response.find("error")->find("message")->string == "The model is unreachable");
}

TEST_CASE("the reasoning reaches the editor") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse thinking;
  thinking.reasoning = "I should answer";
  thinking.content = "Here is the answer";
  harness.model->push(thinking);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"go"}]})", 3);
  harness.transport->awaitResponse(3);

  REQUIRE(lastMessageText(harness.transport->updates(), "agent_thought_chunk") == "I should answer");
  REQUIRE(lastMessageText(harness.transport->updates(), "agent_message_chunk") == "Here is the answer");
}

TEST_CASE("a cancelled turn is interrupted") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse answer;
  answer.content = "Too late";
  harness.model->push(answer);
  harness.model->hold();

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"go"}]})", 3);
  for (int attempt = 0; attempt < 200 && !harness.model->waiting(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(harness.model->waiting());

  harness.transport->sendNotification("session/cancel", R"({"sessionId":")" + session_id + R"("})");
  // The agent has to see the notification before the turn may end, so the
  // test waits for it to be handled.
  for (int attempt = 0; attempt < 200 && !harness.transport->notificationHandled("session/cancel"); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(harness.transport->notificationHandled("session/cancel"));
  harness.model->release();

  const Value response = harness.transport->awaitResponse(3);
  REQUIRE(response.find("result")->find("stopReason")->string == "cancelled");
}

TEST_CASE("loading a stored session replays the tool calls") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse withTool;
  withTool.content = "I will read the file";
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", "read_file", "{}"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "All done";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"read it"}]})", 3);
  harness.transport->awaitResponse(3);

  harness.transport->send("session/load", R"({"sessionId":")" + session_id + R"("})", 4);
  const Value loaded = harness.transport->awaitResponse(4);
  REQUIRE(loaded.find("result") != nullptr);

  const std::vector<Value> updates = harness.transport->updates();
  REQUIRE(lastMessageText(updates, "user_message_chunk") == "read it");
  REQUIRE(lastMessageText(updates, "agent_message_chunk") == "All done");

  bool saw_call = false;
  bool saw_failure = false;
  for (const Value& update : updates) {
    const Value* body = update.find("params")->find("update");
    const Value* kind = body->find("sessionUpdate");
    if (kind == nullptr) {
      continue;
    }
    if (kind->string == "tool_call" && body->find("toolCallId")->string == "call-1") {
      saw_call = true;
    }
    if (kind->string == "tool_call_update" && body->find("status")->string == "failed") {
      saw_failure = true;
    }
  }
  REQUIRE(saw_call);
  REQUIRE(saw_failure);
}

TEST_CASE("an agent without models offers no configuration option") {
  Harness harness = makeHarness("/tmp", false, {});
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  REQUIRE(created.find("result") != nullptr);
  REQUIRE(created.find("result")->find("configOptions") == nullptr);
}

TEST_CASE("an MCP server entry that is malformed is refused") {
  const std::vector<std::string> broken = {
      R"([5])",
      R"([{"command":"/bin/true"}])",
      R"([{"name":"files","command":"/bin/true","args":[5]}])",
      R"([{"name":"files","command":"/bin/true","env":[{"name":"KEY"}]}])",
  };
  for (const std::string& entry : broken) {
    Harness harness = makeHarness("/tmp", false);
    harness.transport->send("initialize", kInitializeParams, 1);
    harness.transport->awaitResponse(1);
    harness.transport->send("session/new", R"({"cwd":"/tmp/work","mcpServers":)" + entry + "}", 2);
    REQUIRE(errorCode(harness.transport->awaitResponse(2)) == bjorne_agent::jsonrpc::kInvalidParams);
  }
}

TEST_CASE("an MCP server may leave its arguments and environment out") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new",
                          R"({"cwd":"/tmp/work","mcpServers":[{"name":"files","command":"/bin/true",)"
                          R"("args":7,"env":9}]})",
                          2);
  const Value created = harness.transport->awaitResponse(2);
  REQUIRE(created.find("result") != nullptr);
}

TEST_CASE("a notification the agent does not know is ignored") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->sendNotification("session/something_else", R"({"sessionId":"sess_nope"})");
  harness.transport->sendNotification("session/cancel", R"({})");
  harness.transport->sendNotification("session/cancel", R"({"sessionId":"sess_nope"})");
  for (int attempt = 0; attempt < 200 && !harness.transport->notificationHandled("session/cancel"); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(harness.transport->notificationHandled("session/cancel"));
}

TEST_CASE("an editor without capabilities gets no client tools") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", R"({"protocolVersion":1,"clientCapabilities":{"fs":7,"elicitation":7}})", 1);
  const Value response = harness.transport->awaitResponse(1);
  REQUIRE(response.find("result") != nullptr);

  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const std::string session_id = makeSession(harness);
  REQUIRE(!session_id.empty());

  // The agent keeps its own tools, because the editor offers none.
  const std::string answer = runTool(harness, session_id, "read_file", R"({"path":"a.txt"})");
  REQUIRE(answer.find("Error:") != std::string::npos);
}

TEST_CASE("session/load without a session id is refused") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.transport->send("session/load", R"({})", 2);
  REQUIRE(errorCode(harness.transport->awaitResponse(2)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("session/load refuses a malformed MCP server") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  harness.transport->send("session/load",
                          R"({"sessionId":")" + session_id + R"(","mcpServers":[5]})", 3);
  REQUIRE(errorCode(harness.transport->awaitResponse(3)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("a session that cannot be built is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);

  harness.factory_fails->store(true);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value refused = harness.transport->awaitResponse(2);
  REQUIRE(errorCode(refused) == bjorne_agent::jsonrpc::kInternalError);
  REQUIRE(refused.find("error")->find("message")->string == "The session runtime could not be built");
}

TEST_CASE("a session that cannot be rebuilt is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  harness.factory_fails->store(true);
  harness.factory_error->assign("The model is gone");
  harness.transport->send("session/set_config_option",
                          R"({"sessionId":")" + session_id + R"(","configId":"model","value":"model-b"})", 3);
  const Value refused = harness.transport->awaitResponse(3);
  REQUIRE(errorCode(refused) == bjorne_agent::jsonrpc::kInternalError);
  REQUIRE(refused.find("error")->find("message")->string == "The model is gone");
}

TEST_CASE("a prompt without a prompt array is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  harness.transport->send("session/prompt", R"({"sessionId":")" + session_id + R"(","prompt":[]})", 3);
  REQUIRE(errorCode(harness.transport->awaitResponse(3)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("a resource block may carry only a location") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse answer;
  answer.content = "Seen";
  harness.model->push(answer);
  harness.transport->send(
      "session/prompt",
      R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"resource","resource":{"uri":"file:///x"}}]})", 3);
  harness.transport->awaitResponse(3);

  const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
  REQUIRE(messages.back().content == "Resource: file:///x\n");
}

TEST_CASE("a resource block with nothing in it is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  harness.transport->send(
      "session/prompt",
      R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"resource","resource":{}}]})", 3);
  REQUIRE(errorCode(harness.transport->awaitResponse(3)) == bjorne_agent::jsonrpc::kInvalidParams);
}

TEST_CASE("a tool that walks out of the workspace is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string read = runTool(harness, session_id, "read_file", R"({"path":"../escape.txt"})", 9);
  REQUIRE(read.find("parent directories") != std::string::npos);

  const std::string write =
      runTool(harness, session_id, "write_file", R"({"path":"../escape.txt","content":"x"})", 10);
  REQUIRE(write.find("parent directories") != std::string::npos);
}

TEST_CASE("a tool that is not a JSON object is refused") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string read = runTool(harness, session_id, "read_file", "not json", 9);
  REQUIRE(read.find("read_file requires path") != std::string::npos);

  const std::string ask = runTool(harness, session_id, "request_input", "not json", 10);
  REQUIRE(ask.find("request_input requires a JSON object") != std::string::npos);
}

TEST_CASE("the editor may refuse a file request") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->error_methods = {"fs/read_text_file", "fs/write_text_file"};
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string read = runTool(harness, session_id, "read_file", R"({"path":"a.txt"})");
  REQUIRE(read.find("The editor refused") != std::string::npos);

  const std::string write = runTool(harness, session_id, "write_file", R"({"path":"a.txt","content":"x"})");
  REQUIRE(write.find("The editor refused") != std::string::npos);
}

TEST_CASE("the editor may refuse a terminal request") {
  // A refused create or wait stops the command. A refused output or release
  // only costs the text, so the command still reports its exit code.
  const std::vector<std::pair<std::string, std::string>> refused = {
      {"terminal/create", "The editor refused"},
      {"terminal/wait_for_exit", "The editor refused"},
      {"terminal/output", "Exit code: 0"},
      {"terminal/release", "Exit code: 0"},
  };
  for (const auto& [method, expected] : refused) {
    Harness harness = makeHarness("/tmp", false);
    harness.transport->error_methods = {method};
    harness.transport->send("initialize", kInitializeParams, 1);
    harness.transport->awaitResponse(1);
    const std::string session_id = makeSession(harness);

    const std::string answer = runTool(harness, session_id, "run_shell", R"({"command":"echo hi"})");
    REQUIRE(answer.find(expected) != std::string::npos);
  }
}

TEST_CASE("the editor may refuse a question") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->error_methods = {"elicitation/create"};
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "request_input", R"({"question":"Which?"})");
  REQUIRE(answer.find("The editor refused") != std::string::npos);
}

TEST_CASE("a command that fails reports its exit code") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->terminal_exit_reply = R"({"jsonrpc":"2.0","id":%ID%,"result":{"exitCode":3}})";
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer = runTool(harness, session_id, "run_shell", R"({"command":"false"})");
  REQUIRE(answer.find("Exit code: 3") != std::string::npos);
}

TEST_CASE("the choices of a question reach the editor") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  const std::string answer =
      runTool(harness, session_id, "request_input", R"({"question":"Which?","options":["red","green"]})");
  REQUIRE(answer.find("yes") != std::string::npos);

  for (const Value& request : harness.transport->agentRequests()) {
    if (request.find("method")->string != "elicitation/create") {
      continue;
    }
    const Value* schema = request.find("params")->find("requestedSchema");
    REQUIRE(schema != nullptr);
    const Value* values = schema->find("properties")->find("answer")->find("enum");
    REQUIRE(values != nullptr);
    REQUIRE(values->array.size() == 2);
    REQUIRE(values->array.at(0).string == "red");
    REQUIRE(values->array.at(1).string == "green");
  }
}

TEST_CASE("a tool call is announced with its title") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  runTool(harness, session_id, "edit_file", R"({"path":"notes.txt"})");

  bool titled = false;
  for (const Value& update : harness.transport->updates()) {
    const Value* body = update.find("params")->find("update");
    const Value* kind = body->find("sessionUpdate");
    if (kind != nullptr && kind->string == "tool_call") {
      if (body->find("title")->string == "Edit notes.txt") {
        titled = true;
      }
    }
  }
  REQUIRE(titled);
}

TEST_CASE("a failed update does not stop the turn") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(
      bjorne_agent::model::ToolCall{"call-1", "read_file", R"({"path":"a.txt"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Done";
  harness.model->push(answer);

  // The editor stops listening, so the updates cannot be delivered.
  harness.transport->fail_sends = true;
  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"read"}]})", 3);

  // The turn still runs, because a lost update is not a lost turn.
  bool saw_tool = false;
  for (int attempt = 0; attempt < 200 && !saw_tool; ++attempt) {
    const std::vector<bjorne_agent::model::ModelMessage> messages = harness.model->lastMessages();
    for (const bjorne_agent::model::ModelMessage& message : messages) {
      if (message.role == "tool") {
        saw_tool = true;
      }
    }
    if (!saw_tool) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  REQUIRE(saw_tool);
}

TEST_CASE("an empty assistant message is replayed as nothing") {
  TempDirectory directory;
  Harness harness = makeHarness(directory.path(), true);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  const std::string session_id = makeSession(harness);

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", "read_file", R"({"path":"a.txt"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "All done";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"read it"}]})", 3);
  harness.transport->awaitResponse(3);

  harness.transport->send("session/load", R"({"sessionId":")" + session_id + R"("})", 4);
  const Value loaded = harness.transport->awaitResponse(4);
  REQUIRE(loaded.find("result") != nullptr);
}

TEST_CASE("the permission question travels as a constrained choice") {
  Harness harness = makeHarness("/tmp", false);
  harness.transport->send("initialize", kInitializeParams, 1);
  harness.transport->awaitResponse(1);
  harness.transport->send("session/new", R"({"cwd":"/tmp/work"})", 2);
  const Value created = harness.transport->awaitResponse(2);
  const std::string session_id = created.find("result")->find("sessionId")->string;

  bjorne_agent::model::ModelResponse withTool;
  withTool.tool_calls.push_back(bjorne_agent::model::ToolCall{"call-1", "write_file", R"({"path":"a.txt"})"});
  harness.model->push(withTool);
  bjorne_agent::model::ModelResponse answer;
  answer.content = "Wrote the file";
  harness.model->push(answer);

  harness.transport->send("session/prompt",
                          R"({"sessionId":")" + session_id + R"(","prompt":[{"type":"text","text":"write it"}]})", 3);
  harness.transport->awaitResponse(3);

  bool asked = false;
  for (const Value& request : harness.transport->agentRequests()) {
    if (request.find("method")->string != "session/request_permission") {
      continue;
    }
    asked = true;
    const Value* params = request.find("params");
    // The question is the title of the tool call, so the editor can show it.
    REQUIRE(params->find("toolCall")->find("title")->string == "Allow write_file to run?");

    // The options carry the id, the kind and the label the user reads.
    const Value* options = params->find("options");
    REQUIRE(options->isArray());
    REQUIRE(options->array.size() == 4);
    REQUIRE(options->array[0].find("optionId")->string == "allow_once");
    REQUIRE(options->array[0].find("kind")->string == "allow_once");
    REQUIRE(options->array[0].find("name")->string == "Allow once");
    REQUIRE(options->array[1].find("optionId")->string == "allow_always");
    REQUIRE(options->array[1].find("name")->string == "Allow always");
    REQUIRE(options->array[2].find("optionId")->string == "reject_once");
    REQUIRE(options->array[2].find("name")->string == "Reject once");
    REQUIRE(options->array[3].find("optionId")->string == "reject_always");
    REQUIRE(options->array[3].find("name")->string == "Reject always");
  }
  REQUIRE(asked);
}
