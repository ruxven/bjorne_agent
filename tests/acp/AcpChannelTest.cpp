#include "acp/AcpChannel.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <catch_amalgamated.hpp>

#include "json/Json.h"
#include "jsonrpc/Endpoint.h"

using bjorne_agent::json::Value;
using bjorne_agent::jsonrpc::Endpoint;
using bjorne_agent::jsonrpc::ReadResult;
using bjorne_agent::jsonrpc::Transport;

namespace {

// A transport that answers the permission request with a scripted outcome and
// records the request the channel wrote.
class ScriptedEditor final : public Transport {
 public:
  bool sendLine(const std::string& line, std::string& error) override {
    (void)error;
    Value message = bjorne_agent::json::parse(line);
    std::lock_guard<std::mutex> guard(mutex_);
    written.push_back(message);

    const Value* method = message.find("method");
    const Value* id = message.find("id");
    if (method == nullptr || id == nullptr) {
      return true;
    }
    // The answer the editor gives. The test changes it to script a refusal.
    incoming.push_back(R"({"jsonrpc":"2.0","id":)" + std::to_string(static_cast<int>(id->number)) +
                       R"(,"result":)" + outcome + "}");
    condition_.notify_all();
    return true;
  }

  ReadResult readLine(int timeout_ms, std::string& line, std::string& error) override {
    (void)error;
    std::unique_lock<std::mutex> lock(mutex_);
    if (!incoming.empty()) {
      line = incoming.front();
      incoming.pop_front();
      return ReadResult::Line;
    }
    condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !incoming.empty(); });
    if (!incoming.empty()) {
      line = incoming.front();
      incoming.pop_front();
      return ReadResult::Line;
    }
    return ReadResult::Timeout;
  }

  std::vector<Value> written;
  // The result the editor answers the permission request with.
  std::string outcome = R"({"outcome":{"outcome":"selected","optionId":"allow_once"}})";

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::string> incoming;
};

}  // namespace

TEST_CASE("the permission choices are the four the editor offers") {
  const std::vector<bjorne_agent::acp::PermissionChoice>& choices = bjorne_agent::acp::permissionChoices();
  REQUIRE(choices.size() == 4);

  REQUIRE(choices[0].id == "allow_once");
  REQUIRE(choices[0].label == "Allow once");
  REQUIRE(choices[0].granted);
  REQUIRE_FALSE(choices[0].remember);

  REQUIRE(choices[1].id == "allow_always");
  REQUIRE(choices[1].label == "Allow always");
  REQUIRE(choices[1].granted);
  REQUIRE(choices[1].remember);

  REQUIRE_FALSE(choices[2].granted);
  REQUIRE_FALSE(choices[3].granted);
}

TEST_CASE("the permission question is a constrained choice") {
  const bjorne_agent::interaction::Request request = bjorne_agent::acp::permissionRequest("write_file");
  REQUIRE(request.question == "Allow write_file to run?");
  REQUIRE(request.options.size() == 4);
  REQUIRE(request.options[0] == "Allow once");
  REQUIRE(request.options[3] == "Reject always");
}

TEST_CASE("a label names the choice it stands for") {
  const bjorne_agent::acp::PermissionChoice choice = bjorne_agent::acp::permissionChoiceForLabel("Allow always");
  REQUIRE(choice.id == "allow_always");
  REQUIRE(choice.granted);
  REQUIRE(choice.remember);

  // A label that is not on offer grants nothing.
  const bjorne_agent::acp::PermissionChoice unknown = bjorne_agent::acp::permissionChoiceForLabel("Maybe");
  REQUIRE(unknown.id.empty());
  REQUIRE_FALSE(unknown.granted);
}

TEST_CASE("the channel asks the editor and returns the chosen label") {
  auto transport = std::make_shared<ScriptedEditor>();
  auto endpoint = std::make_shared<Endpoint>(transport);
  endpoint->start();

  bjorne_agent::acp::AcpChannel channel(endpoint, "sess_1", "call-1", [] { return false; });
  REQUIRE(channel.available());

  std::string answer;
  std::string error;
  REQUIRE(channel.ask(bjorne_agent::acp::permissionRequest("write_file"), answer, error));
  REQUIRE(error.empty());
  REQUIRE(answer == "Allow once");
}

TEST_CASE("the channel reports a refusal from the editor") {
  auto transport = std::make_shared<ScriptedEditor>();
  transport->outcome = R"({"outcome":{"outcome":"cancelled"}})";
  auto endpoint = std::make_shared<Endpoint>(transport);
  endpoint->start();

  bjorne_agent::acp::AcpChannel channel(endpoint, "sess_1", "call-1", [] { return false; });

  std::string answer;
  std::string error;
  REQUIRE_FALSE(channel.ask(bjorne_agent::acp::permissionRequest("write_file"), answer, error));
  REQUIRE(error == "The user cancelled the request");
}

TEST_CASE("the channel reports a permission request that failed") {
  auto transport = std::make_shared<ScriptedEditor>();
  transport->outcome = R"({"outcome":{"outcome":"selected","optionId":"not_an_option"}})";
  auto endpoint = std::make_shared<Endpoint>(transport);
  endpoint->start();

  bjorne_agent::acp::AcpChannel channel(endpoint, "sess_1", "call-1", [] { return false; });

  std::string answer;
  std::string error;
  REQUIRE_FALSE(channel.ask(bjorne_agent::acp::permissionRequest("write_file"), answer, error));
  REQUIRE(error == "The permission outcome names an option that is not on offer");
}

TEST_CASE("the channel stops waiting when the turn is cancelled") {
  auto transport = std::make_shared<ScriptedEditor>();
  // The editor never answers.
  transport->outcome = "";
  auto endpoint = std::make_shared<Endpoint>(transport);
  endpoint->start();

  std::atomic<bool> cancelled{false};
  bjorne_agent::acp::AcpChannel channel(endpoint, "sess_1", "call-1", [&cancelled] { return cancelled.load(); });

  std::atomic<bool> done{false};
  std::string answer;
  std::string error;
  std::thread worker([&] {
    channel.ask(bjorne_agent::acp::permissionRequest("write_file"), answer, error);
    done = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  cancelled = true;

  worker.join();
  REQUIRE(done);
  REQUIRE(error == "The turn was cancelled");
}
