#include "jsonrpc/Endpoint.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

#include "json/Json.h"

using bjorne_agent::json::Value;
using bjorne_agent::jsonrpc::Endpoint;
using bjorne_agent::jsonrpc::Error;
using bjorne_agent::jsonrpc::Message;
using bjorne_agent::jsonrpc::ReadResult;
using bjorne_agent::jsonrpc::Transport;

namespace {

// An in-memory transport. The test pushes the lines a peer would send and
// reads back what the endpoint wrote.
class FakeTransport final : public Transport {
 public:
  bool sendLine(const std::string& line, std::string& error) override {
    (void)error;
    std::lock_guard<std::mutex> guard(mutex_);
    sent.push_back(line);
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
    if (closed) {
      return ReadResult::Closed;
    }
    condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !incoming.empty() || closed; });
    if (!incoming.empty()) {
      line = incoming.front();
      incoming.pop_front();
      return ReadResult::Line;
    }
    return closed ? ReadResult::Closed : ReadResult::Timeout;
  }

  void push(const std::string& line) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      incoming.push_back(line);
    }
    condition_.notify_all();
  }

  void close() {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      closed = true;
    }
    condition_.notify_all();
  }

  std::vector<std::string> sentLines() {
    std::lock_guard<std::mutex> guard(mutex_);
    return sent;
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::string> incoming;
  std::vector<std::string> sent;
  bool closed = false;
};

}  // namespace

TEST_CASE("a request gets the matching response") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  std::thread responder([transport] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    transport->push(R"({"jsonrpc":"2.0","id":1,"result":{"value":42}})");
  });

  Value result;
  std::string error;
  REQUIRE(endpoint.request("ping", Value(), result, error, bjorne_agent::jsonrpc::RequestOptions{2000}));
  REQUIRE(result.find("value")->number == 42);
  REQUIRE(error.empty());
  responder.join();
}

TEST_CASE("an error response becomes the request error") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  std::thread responder([transport] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    transport->push(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"Method not found"}})");
  });

  Value result;
  std::string error;
  REQUIRE_FALSE(endpoint.request("nope", Value(), result, error, bjorne_agent::jsonrpc::RequestOptions{2000}));
  REQUIRE(error == "Method not found");
  responder.join();
}

TEST_CASE("a request times out when the peer stays silent") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  Value result;
  std::string error;
  REQUIRE_FALSE(endpoint.request("slow", Value(), result, error, bjorne_agent::jsonrpc::RequestOptions{100}));
  REQUIRE(error.find("timed out") != std::string::npos);
}

TEST_CASE("a closed connection fails the pending request") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  std::thread closer([transport] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    transport->close();
  });

  Value result;
  std::string error;
  REQUIRE_FALSE(endpoint.request("slow", Value(), result, error, bjorne_agent::jsonrpc::RequestOptions{2000}));
  REQUIRE(error.find("closed") != std::string::npos);
  REQUIRE(endpoint.closed());
  closer.join();
}

TEST_CASE("a notification is written without an id") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  std::string error;
  REQUIRE(endpoint.notify("ready", Value(), error));

  const std::vector<std::string> sent = transport->sentLines();
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].find("\"method\":\"ready\"") != std::string::npos);
  REQUIRE(sent[0].find("\"id\"") == std::string::npos);
}

TEST_CASE("a request from the peer is answered by the handler") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.setRequestHandler([](const std::string& method, const Value& params, Value& result, Error& error) {
    (void)params;
    if (method != "known") {
      error.code = -32601;
      error.message = "Method not found";
      return false;
    }
    result = Value();
    result.type = Value::Type::Object;
    result.object["ok"] = Value();
    result.object["ok"].type = Value::Type::Boolean;
    result.object["ok"].boolean = true;
    return true;
  });
  endpoint.start();

  transport->push(R"({"jsonrpc":"2.0","id":9,"method":"known","params":{}})");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const std::vector<std::string> sent = transport->sentLines();
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].find("\"id\":9") != std::string::npos);
  REQUIRE(sent[0].find("\"result\"") != std::string::npos);
}

TEST_CASE("an unknown request from the peer is refused") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  transport->push(R"({"jsonrpc":"2.0","id":9,"method":"unknown","params":{}})");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const std::vector<std::string> sent = transport->sentLines();
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].find("\"error\"") != std::string::npos);
  REQUIRE(sent[0].find("Method not found") != std::string::npos);
}

TEST_CASE("a malformed line does not close the connection") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  transport->push("this is not json");
  transport->push(R"({"jsonrpc":"2.0","id":1,"result":{}})");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  REQUIRE_FALSE(endpoint.closed());
}

TEST_CASE("a notification from the peer reaches the handler") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  std::mutex mutex;
  std::vector<std::string> seen;
  endpoint.setNotificationHandler([&](const std::string& method, const Value& params) {
    (void)params;
    std::lock_guard<std::mutex> guard(mutex);
    seen.push_back(method);
  });
  endpoint.start();

  transport->push(R"({"jsonrpc":"2.0","method":"notifications/message","params":{}})");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::lock_guard<std::mutex> guard(mutex);
  REQUIRE(seen.size() == 1);
  REQUIRE(seen[0] == "notifications/message");
}

TEST_CASE("a request gives up when the abort predicate says so") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  std::atomic<bool> aborted{false};
  Value result;
  std::string error;
  bjorne_agent::jsonrpc::RequestOptions options;
  options.timeout_ms = 5000;
  options.abort = [&aborted]() { return aborted.load(); };
  const bool ok = endpoint.request("ask", Value(), result, error, options);
  REQUIRE_FALSE(ok);
  REQUIRE(error.find("timed out") != std::string::npos);

  // The peer answers too late, and the answer is ignored.
  transport->push(R"({"jsonrpc":"2.0","id":1,"result":{"value":1}})");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE_FALSE(endpoint.closed());
}

TEST_CASE("the abort predicate ends a waiting request early") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  std::atomic<bool> aborted{false};
  std::thread stopper([&aborted] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    aborted.store(true);
  });

  const auto started = std::chrono::steady_clock::now();
  Value result;
  std::string error;
  bjorne_agent::jsonrpc::RequestOptions options;
  options.timeout_ms = 10000;
  options.abort = [&aborted]() { return aborted.load(); };
  REQUIRE_FALSE(endpoint.request("ask", Value(), result, error, options));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  REQUIRE(elapsed.count() < 5000);
  stopper.join();
}

TEST_CASE("a handler may ask the peer something while it answers") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);

  // The handler turns a request from the peer into a request of its own. The
  // worker thread runs the handler, so the reader thread stays free to bring
  // the answer in.
  endpoint.setRequestHandler([&endpoint](const std::string& method, const Value& params, Value& result, Error& error) {
    (void)method;
    (void)params;
    Value answer;
    std::string innerError;
    if (!endpoint.request("ask", Value(), answer, innerError, bjorne_agent::jsonrpc::RequestOptions{2000})) {
      error.code = -32603;
      error.message = "The inner request failed: " + innerError;
      return false;
    }
    result = answer;
    return true;
  });
  endpoint.start();

  std::thread peer([transport] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    transport->push(R"({"jsonrpc":"2.0","id":1,"result":{"value":7}})");
  });

  transport->push(R"({"jsonrpc":"2.0","id":9,"method":"outer","params":{}})");
  peer.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const std::vector<std::string> sent = transport->sentLines();
  bool answered = false;
  for (const std::string& line : sent) {
    if (line.find("\"id\":9") != std::string::npos && line.find("\"value\":7") != std::string::npos) {
      answered = true;
    }
  }
  REQUIRE(answered);
}

TEST_CASE("waiting for the close returns once the peer is gone") {
  auto transport = std::make_shared<FakeTransport>();
  Endpoint endpoint(transport);
  endpoint.start();

  transport->close();
  endpoint.waitForClose();

  REQUIRE(endpoint.closed());
  REQUIRE(endpoint.closeReason() == "The connection closed.");
}
