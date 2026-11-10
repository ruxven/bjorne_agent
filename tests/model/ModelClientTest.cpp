#include "model/ModelClient.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

#include "http/LoopbackServer.h"

namespace {

const char* kApiKeyEnv = "BJORNE_AGENT_TEST_API_KEY";

class MockTransport final : public bjorne_agent::http::HttpTransport {
 public:
  bool send(const std::string& request, const std::string& host, const std::string& port,
            std::string& response, std::string& error) override {
    last_request = request;
    last_host = host;
    last_port = port;
    if (fail) {
      error = pending_error;
      return false;
    }
    response = next_response;
    return true;
  }

  std::string last_request;
  std::string last_host;
  std::string last_port;
  std::string next_response;
  bool fail = false;
  std::string pending_error;
};

std::shared_ptr<MockTransport> makeMock(const std::string& response) {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response = response;
  return mock;
}

std::string okResponse(const std::string& body) {
  return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + body;
}

bjorne_agent::model::ModelMessage message(const std::string& role, const std::string& content) {
  bjorne_agent::model::ModelMessage result;
  result.role = role;
  result.content = content;
  return result;
}

}  // namespace

TEST_CASE("Model client sends auth header and parses content") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(R"({"choices":[{"message":{"role":"assistant","content":"hello"}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);
  client.setTools(R"([{"type":"function"}])");

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(error.empty());
  REQUIRE(response.content == "hello");
  REQUIRE(response.tool_calls.empty());
  REQUIRE(mock->last_host == "api.example.com");
  REQUIRE(mock->last_request.rfind("POST /v1/chat/completions HTTP/1.1\r\n", 0) == 0);
  REQUIRE(mock->last_request.find("Authorization: Bearer test-key\r\n") != std::string::npos);
  REQUIRE(mock->last_request.find("\"model\":\"test-model\"") != std::string::npos);
  REQUIRE(mock->last_request.find("\"tools\":[{\"type\":\"function\"}]") != std::string::npos);
  REQUIRE(mock->last_request.find("\"content\":\"hi\"") != std::string::npos);
}

TEST_CASE("Model client parses reasoning_content") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(
      R"({"choices":[{"message":{"role":"assistant","content":"answer","reasoning_content":"step by step"}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(response.content == "answer");
  REQUIRE(response.reasoning == "step by step");
}

TEST_CASE("Model client parses reasoning under its other field name") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(
      R"({"choices":[{"message":{"role":"assistant","content":"answer","reasoning":"thinking"}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(response.reasoning == "thinking");
}

TEST_CASE("Model client ignores a reasoning field that is not a string") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(
      R"({"choices":[{"message":{"role":"assistant","content":"answer","reasoning":{"unexpected":true}}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(response.reasoning.empty());
}

TEST_CASE("Model client parses tool calls") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(
      R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.txt\"}"}}]}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "read a.txt")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(response.content.empty());
  REQUIRE(response.tool_calls.size() == 1);
  REQUIRE(response.tool_calls[0].id == "call_1");
  REQUIRE(response.tool_calls[0].name == "read_file");
  REQUIRE(response.tool_calls[0].arguments == R"({"path":"a.txt"})");
}

TEST_CASE("Model client serializes tool result messages") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(R"({"choices":[{"message":{"role":"assistant","content":"done"}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelMessage toolResult;
  toolResult.role = "tool";
  toolResult.tool_call_id = "call_9";
  toolResult.content = "file contents";
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "go"), toolResult};

  bjorne_agent::model::ModelResponse response;
  std::string error;
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(mock->last_request.find("\"tool_call_id\":\"call_9\"") != std::string::npos);
  REQUIRE(mock->last_request.find("\"role\":\"tool\"") != std::string::npos);
}

TEST_CASE("Model client reports a missing API key") {
  bjorne_agent::platform::unsetEnvironmentVariable(kApiKeyEnv);
  auto mock = makeMock(okResponse("{}"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE_FALSE(client.respond(messages, response, error));
  REQUIRE(error == "Missing API key environment variable: BJORNE_AGENT_TEST_API_KEY");
  REQUIRE(mock->last_request.empty());
}

TEST_CASE("Model client reports a non-2xx status") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock("HTTP/1.1 500 Internal Server Error\r\n\r\nboom");
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE_FALSE(client.respond(messages, response, error));
  REQUIRE(error.find("status 500") != std::string::npos);
  REQUIRE(error.find("boom") != std::string::npos);
}

TEST_CASE("Model client reports a missing choices array") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(R"({})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE_FALSE(client.respond(messages, response, error));
  REQUIRE(error == "Model response missing choices");
}

TEST_CASE("Model client reports a missing message in the choice") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(R"({"choices":[{"finish_reason":"stop"}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE_FALSE(client.respond(messages, response, error));
  REQUIRE(error == "Model response missing message");
}

TEST_CASE("Model client reports malformed response JSON") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse("not json"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE_FALSE(client.respond(messages, response, error));
  REQUIRE(error.find("Model response JSON parse error") != std::string::npos);
}

TEST_CASE("Model client surfaces transport failure") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock("");
  mock->fail = true;
  mock->pending_error = "transport down";
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE_FALSE(client.respond(messages, response, error));
  REQUIRE(error == "transport down");
}

TEST_CASE("Model client serializes assistant tool calls") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(R"({"choices":[{"message":{"role":"assistant","content":"done"}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelMessage assistant = message("assistant", "checking");
  assistant.tool_calls.push_back(bjorne_agent::model::ToolCall{"call_1", "read_file", R"({"path":"a.txt"})"});
  assistant.tool_calls.push_back(bjorne_agent::model::ToolCall{"call_2", "run_shell", ""});
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "go"), assistant};

  bjorne_agent::model::ModelResponse response;
  std::string error;
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(mock->last_request.find(
              R"("tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.txt\"}"}},{"id":"call_2","type":"function","function":{"name":"run_shell","arguments":""}}])") !=
          std::string::npos);
}

TEST_CASE("Model client escapes request strings") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test\"model", kApiKeyEnv, mock);

  const std::vector<bjorne_agent::model::ModelMessage> messages = {
      message("user", "tab\there\rreturn\nquote\"back\\slash" + std::string(1, '\x01') + "end")};

  bjorne_agent::model::ModelResponse response;
  std::string error;
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(mock->last_request.find("\"model\":\"test\\\"model\"") != std::string::npos);
  REQUIRE(mock->last_request.find("tab\\there\\rreturn\\nquote\\\"back\\\\slash\\u0001end") != std::string::npos);
}

TEST_CASE("Model client skips non-object tool call entries") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(
      R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[42,{"id":"call_1","function":{"name":"read_file","arguments":"{}"}}]}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(response.tool_calls.size() == 1);
  REQUIRE(response.tool_calls[0].id == "call_1");
  REQUIRE(response.tool_calls[0].name == "read_file");
}

TEST_CASE("Model client rejects an endpoint without a scheme") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse("{}"));
  bjorne_agent::model::ModelClient client("api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE_FALSE(client.respond(messages, response, error));
  REQUIRE(error == "Endpoint must include a scheme: api.example.com/v1");
  REQUIRE(mock->last_request.empty());
}

TEST_CASE("Model client rejects an unsupported endpoint scheme") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse("{}"));
  bjorne_agent::model::ModelClient client("ftp://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE_FALSE(client.respond(messages, response, error));
  REQUIRE(error == "Unsupported endpoint scheme: ftp");
}

TEST_CASE("Model client uses plain TCP for http endpoints") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  bjorne_agent_test::LoopbackServer server;
  server.setResponse(
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
      R"({"choices":[{"message":{"role":"assistant","content":"local"}}]})");

  bjorne_agent::model::ModelClient client("http://127.0.0.1:" + server.port() + "/v1", "local-model", kApiKeyEnv);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(error.empty());
  REQUIRE(server.wait());
  REQUIRE(response.content == "local");
  REQUIRE(server.request().rfind("POST /v1/chat/completions HTTP/1.1\r\n", 0) == 0);
  REQUIRE(server.request().find("Host: 127.0.0.1:" + server.port() + "\r\n") != std::string::npos);
  REQUIRE(server.request().find("Authorization: Bearer test-key\r\n") != std::string::npos);
  REQUIRE(server.request().find("\"model\":\"local-model\"") != std::string::npos);
}

TEST_CASE("Model client passes the endpoint port to the transport") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com:8443/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(mock->last_host == "api.example.com");
  REQUIRE(mock->last_port == "8443");
  REQUIRE(mock->last_request.find("Host: api.example.com:8443\r\n") != std::string::npos);
}

TEST_CASE("Model client defaults the https port to 443") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  auto mock = makeMock(okResponse(R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})"));
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv, mock);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(mock->last_port == "443");
  REQUIRE(mock->last_request.find("Host: api.example.com\r\n") != std::string::npos);
}

#if BJORNE_AGENT_HAS_OPENSSL
TEST_CASE("Model client uses TLS for https endpoints") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  bjorne_agent_test::LoopbackServer server(bjorne_agent_test::LoopbackServer::Mode::Tls);
  server.setResponse(
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
      R"({"choices":[{"message":{"role":"assistant","content":"secure"}}]})");

  // The loopback server presents a throwaway self-signed certificate, so the
  // client has to be told to accept it.
  bjorne_agent::model::ModelClientOptions options;
  options.endpoint = "https://127.0.0.1:" + server.port() + "/v1";
  options.model = "secure-model";
  options.api_key_env = kApiKeyEnv;
  options.ignore_self_signed = true;
  bjorne_agent::model::ModelClient client(options);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE(client.respond(messages, response, error));
  REQUIRE(error.empty());
  REQUIRE(server.wait());
  REQUIRE(response.content == "secure");
  REQUIRE(server.request().rfind("POST /v1/chat/completions HTTP/1.1\r\n", 0) == 0);
  REQUIRE(server.request().find("Host: 127.0.0.1:" + server.port() + "\r\n") != std::string::npos);
}
#endif

#if !BJORNE_AGENT_HAS_OPENSSL
TEST_CASE("Model client rejects an https endpoint when built without TLS") {
  bjorne_agent::platform::setEnvironmentVariable(kApiKeyEnv, "test-key");
  bjorne_agent::model::ModelClient client("https://api.example.com/v1", "test-model", kApiKeyEnv);

  bjorne_agent::model::ModelResponse response;
  std::string error;
  const std::vector<bjorne_agent::model::ModelMessage> messages = {message("user", "hi")};
  REQUIRE_FALSE(client.respond(messages, response, error));
  REQUIRE(error == "Endpoint https://api.example.com/v1 uses https, but this build has no TLS support");
}
#endif
