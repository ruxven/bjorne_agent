#include "http/HttpClient.h"

#include <memory>
#include <string>

#include <catch_amalgamated.hpp>

#include "http/LoopbackServer.h"

namespace {

class MockTransport final : public bjorne_agent::http::HttpTransport {
 public:
  bool send(const std::string& request, const std::string& host, const std::string& port,
            std::string& response, std::string& error) override {
    last_request_ = request;
    last_host_ = host;
    last_port_ = port;
    if (fail_) {
      fail_ = false;
      error = pending_error_;
      return false;
    }
    response = next_response_;
    error_.clear();
    return true;
  }

  std::string last_request_;
  std::string last_host_;
  std::string last_port_;
  std::string next_response_;
  std::string error_;
  std::string pending_error_;
  bool fail_ = false;
};

bjorne_agent::http::HttpClient makeClient(std::shared_ptr<bjorne_agent::http::HttpTransport> transport) {
  return bjorne_agent::http::HttpClient(std::move(transport));
}

bjorne_agent::http::HttpRequest makeRequest(const std::string& host, const std::string& port,
                                      const std::string& path = "/v1/chat/completions",
                                      const std::string& body = "{}") {
  bjorne_agent::http::HttpRequest request;
  request.host = host;
  request.port = port;
  request.path = path;
  request.body = body;
  return request;
}

}  // namespace

TEST_CASE("HTTP client sends serialized HTTP/1.1 request") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{}";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const std::string body = "{\"model\":\"gpt-4o\"}";
  const bool ok = client.request(makeRequest("api.example.com", "443", "/v1/chat/completions", body), response, error);
  REQUIRE(ok);
  REQUIRE(error.empty());

  REQUIRE(mock->last_request_.rfind("POST /v1/chat/completions HTTP/1.1\r\n", 0) == 0);
  REQUIRE(mock->last_request_.find("Host: api.example.com\r\n") != std::string::npos);
  REQUIRE(mock->last_request_.find("Content-Type: application/json\r\n") != std::string::npos);
  REQUIRE(mock->last_request_.find("Content-Length: " + std::to_string(body.size()) + "\r\n") != std::string::npos);
  REQUIRE(mock->last_request_.find("Connection: close\r\n\r\n") != std::string::npos);
  REQUIRE(mock->last_request_.find(body) != std::string::npos);
  REQUIRE(response.status == 200);
}

TEST_CASE("HTTP client parses response status line and body") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 16\r\n\r\n{\"ok\":true}";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE(ok);
  REQUIRE(response.status == 200);
  REQUIRE(response.status_line == "HTTP/1.1 200 OK");
  REQUIRE(response.body == "{\"ok\":true}");
}

TEST_CASE("HTTP client parses status without headers") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ = "HTTP/1.1 204 No Content\r\n\r\n";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE(ok);
  REQUIRE(response.status == 204);
  REQUIRE(response.status_line == "HTTP/1.1 204 No Content");
  REQUIRE(response.body.empty());
}

TEST_CASE("HTTP client surfaces transport failure") {
  auto mock = std::make_shared<MockTransport>();
  mock->fail_ = true;
  mock->pending_error_ = "mock TLS handshake failed";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE_FALSE(ok);
  REQUIRE(error == "mock TLS handshake failed");
}

TEST_CASE("HTTP client rejects malformed response without header terminator") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE_FALSE(ok);
  REQUIRE(error == "Malformed HTTP response: missing header terminator");
}

TEST_CASE("HTTP client rejects malformed status line") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ = "garbage\r\n\r\nbody";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE_FALSE(ok);
  REQUIRE(error == "Malformed HTTP status line");
}

TEST_CASE("HTTP client carries a non default port in the Host header") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ = "HTTP/1.1 200 OK\r\n\r\n{}";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "8443"), response, error);
  REQUIRE(ok);
  REQUIRE(mock->last_port_ == "8443");
  REQUIRE(mock->last_request_.find("Host: api.example.com:8443\r\n") != std::string::npos);
}

TEST_CASE("HTTP client decodes a chunked response body") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ =
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
      "b\r\n{\"ok\":true}\r\n0\r\n\r\n";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE(ok);
  REQUIRE(response.status == 200);
  REQUIRE(response.body == "{\"ok\":true}");
}

TEST_CASE("HTTP client decodes a chunked body split over several chunks") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\n{\"ok\"\r\n6\r\n:true}\r\n0\r\n\r\n";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE(ok);
  REQUIRE(response.body == "{\"ok\":true}");
}

TEST_CASE("HTTP client decodes a chunked body that carries chunk extensions") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "b;name=value\r\n{\"ok\":true}\r\n0\r\n\r\n";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE(ok);
  REQUIRE(response.body == "{\"ok\":true}");
}

TEST_CASE("HTTP client truncates a body to the stated content length") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":true}trailing";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE(ok);
  REQUIRE(response.body == "{\"ok\":true}");
}

TEST_CASE("HTTP client rejects a truncated chunked body") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n20\r\n{\"ok\":true}";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE_FALSE(ok);
  REQUIRE(error == "Malformed chunked body: truncated chunk");
}

TEST_CASE("HTTP client rejects a chunked body with a bad chunk size") {
  auto mock = std::make_shared<MockTransport>();
  mock->next_response_ = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\ndata\r\n0\r\n\r\n";
  bjorne_agent::http::HttpClient client = makeClient(mock);

  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok = client.request(makeRequest("api.example.com", "443"), response, error);
  REQUIRE_FALSE(ok);
  REQUIRE(error == "Malformed chunked body: invalid chunk size");
}

TEST_CASE("plain TCP transport completes a request against a loopback server") {
  bjorne_agent_test::LoopbackServer server;
  server.setResponse("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"ok\":true}");

  auto client = bjorne_agent::http::HttpClient::createPlainTcp();
  bjorne_agent::http::HttpResponse response;
  std::string error;
  const bool ok =
      client->request(makeRequest("127.0.0.1", server.port(), "/v1/chat/completions", "{\"a\":1}"), response, error);
  REQUIRE(ok);
  REQUIRE(error.empty());
  REQUIRE(server.wait());
  REQUIRE(response.status == 200);
  REQUIRE(response.body == "{\"ok\":true}");
  REQUIRE(server.request().rfind("POST /v1/chat/completions HTTP/1.1\r\n", 0) == 0);
  REQUIRE(server.request().find("Host: 127.0.0.1:" + server.port() + "\r\n") != std::string::npos);
  REQUIRE(server.request().find("Content-Length: 7\r\n") != std::string::npos);
  REQUIRE(server.request().find("{\"a\":1}") != std::string::npos);
}

TEST_CASE("plain TCP transport reports a refused connection") {
  bjorne_agent::http::PlainTcpTransport transport;
  std::string response;
  std::string error;
  REQUIRE_FALSE(transport.send("GET / HTTP/1.1\r\n\r\n", "127.0.0.1", "1", response, error));
  REQUIRE(error == "Socket connect failed");
}

TEST_CASE("plain TCP transport reports a DNS failure") {
  bjorne_agent::http::PlainTcpTransport transport;
  std::string response;
  std::string error;
  REQUIRE_FALSE(transport.send("GET / HTTP/1.1\r\n\r\n", "bjorne_agent.invalid", "80", response, error));
  REQUIRE(error.find("DNS resolution failed") != std::string::npos);
}

#if BJORNE_AGENT_HAS_OPENSSL
TEST_CASE("OpenSSL transport completes a request against a loopback TLS server") {
  bjorne_agent_test::LoopbackServer server(bjorne_agent_test::LoopbackServer::Mode::Tls);
  server.setResponse("HTTP/1.1 200 OK\r\n\r\n{\"tls\":true}");

  // The loopback server presents a throwaway self-signed certificate, so the
  // transport has to be told to accept it.
  bjorne_agent::http::OpenSslTransport::Options options;
  options.ignore_self_signed = true;
  bjorne_agent::http::OpenSslTransport transport(options);
  std::string response;
  std::string error;
  const bool ok = transport.send("POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
                                 "127.0.0.1", server.port(), response, error);
  REQUIRE(ok);
  REQUIRE(error.empty());
  REQUIRE(server.wait());
  REQUIRE(response.find("{\"tls\":true}") != std::string::npos);
}

TEST_CASE("OpenSSL transport rejects an untrusted certificate by default") {
  bjorne_agent_test::LoopbackServer server(bjorne_agent_test::LoopbackServer::Mode::Tls);

  bjorne_agent::http::OpenSslTransport transport;
  std::string response;
  std::string error;
  REQUIRE_FALSE(transport.send("GET / HTTP/1.1\r\n\r\n", "127.0.0.1", server.port(), response, error));
  REQUIRE(error.find("Certificate verification failed") != std::string::npos);
}
#endif

#if BJORNE_AGENT_HAS_OPENSSL
TEST_CASE("OpenSSL transport reports a TLS handshake failure") {
  bjorne_agent_test::LoopbackServer server(bjorne_agent_test::LoopbackServer::Mode::CloseImmediately);

  bjorne_agent::http::OpenSslTransport transport;
  std::string response;
  std::string error;
  REQUIRE_FALSE(transport.send("GET / HTTP/1.1\r\n\r\n", "127.0.0.1", server.port(), response, error));
  REQUIRE(error.find("TLS error:") != std::string::npos);
  REQUIRE(server.wait());
}
#endif

#if BJORNE_AGENT_HAS_OPENSSL
TEST_CASE("OpenSSL transport reports a refused connection") {
  bjorne_agent::http::OpenSslTransport transport;
  std::string response;
  std::string error;
  REQUIRE_FALSE(transport.send("GET / HTTP/1.1\r\n\r\n", "127.0.0.1", "1", response, error));
  REQUIRE(error == "Socket connect failed");
}

#endif
TEST_CASE("plain TCP transport reports a read failure") {
  bjorne_agent_test::LoopbackServer server(bjorne_agent_test::LoopbackServer::Mode::ResetAfterRead);

  bjorne_agent::http::PlainTcpTransport transport;
  std::string response;
  std::string error;
  REQUIRE_FALSE(transport.send("GET / HTTP/1.1\r\n\r\n", "127.0.0.1", server.port(), response, error));
  REQUIRE(error == "Socket read failed");
  REQUIRE(server.wait());
}

#if BJORNE_AGENT_HAS_OPENSSL
TEST_CASE("OpenSSL transport reports a read failure") {
  bjorne_agent_test::LoopbackServer server(bjorne_agent_test::LoopbackServer::Mode::TlsResetAfterRead);

  bjorne_agent::http::OpenSslTransport::Options options;
  options.ignore_self_signed = true;
  bjorne_agent::http::OpenSslTransport transport(options);
  std::string response;
  std::string error;
  REQUIRE_FALSE(transport.send("GET / HTTP/1.1\r\n\r\n", "127.0.0.1", server.port(), response, error));
  REQUIRE(error.find("TLS error:") != std::string::npos);
  REQUIRE(server.wait());
}
#endif

#if BJORNE_AGENT_HAS_OPENSSL
TEST_CASE("OpenSSL transport reports a DNS failure") {
  bjorne_agent::http::OpenSslTransport transport;
  std::string response;
  std::string error;
  REQUIRE_FALSE(transport.send("GET / HTTP/1.1\r\n\r\n", "bjorne_agent.invalid", "443", response, error));
  REQUIRE(error.find("DNS resolution failed") != std::string::npos);
}
#endif

#if BJORNE_AGENT_HAS_OPENSSL
TEST_CASE("HTTP client creates an OpenSSL transport") {
  const auto client = bjorne_agent::http::HttpClient::createOpenSsl();
  REQUIRE(client != nullptr);
}
#endif

#if !BJORNE_AGENT_HAS_OPENSSL
TEST_CASE("OpenSSL transport reports that TLS is unavailable") {
  bjorne_agent::http::OpenSslTransport transport;
  std::string response;
  std::string error;
  REQUIRE_FALSE(transport.send("GET / HTTP/1.1\r\n\r\n", "127.0.0.1", "443", response, error));
  REQUIRE(error == "TLS is not available: this build has no OpenSSL");

  auto client = bjorne_agent::http::HttpClient::createOpenSsl();
  REQUIRE(client != nullptr);
  bjorne_agent::http::HttpResponse out;
  REQUIRE_FALSE(client->request(makeRequest("127.0.0.1", "443", "/"), out, error));
  REQUIRE(error == "TLS is not available: this build has no OpenSSL");
}
#endif
