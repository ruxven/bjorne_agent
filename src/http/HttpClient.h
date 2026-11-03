#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bjorne_agent {
namespace http {

struct HttpResponse {
  int status = 0;
  std::string status_line;
  std::string body;
};

// One outbound request. Grouping the fields keeps the transport and client
// signatures short, and lets a caller fill in what it has and leave the rest at
// the defaults.
struct HttpRequest {
  std::string method = "POST";
  std::string host;
  std::string port;
  std::string path;
  std::string body;
  std::string content_type = "application/json";
  std::vector<std::pair<std::string, std::string>> headers;
};

// Transport abstraction for the outbound wire. Real deployments use
// OpenSslTransport or PlainTcpTransport. Tests substitute a mock to cover
// request serialization, response parsing, and error paths without touching
// real sockets or TLS.
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual bool send(const std::string& request, const std::string& host, const std::string& port,
                    std::string& response, std::string& error) = 0;
};

class OpenSslTransport final : public HttpTransport {
 public:
  // TLS connection options. Configure certificate verification and SNI
  // separately so callers can accept self-signed certs explicitly when needed.
  struct Options {
    // Accept self-signed / untrusted certificates. Disabled by default:
    // peer verification is enabled and hostname checked against the cert.
    bool ignore_self_signed = false;
    // SNI server name. Empty = use `host` as the SNI name.
    std::string hostname;
  };

  // Fails with a clear error when the build has no OpenSSL, rather than
  // silently downgrading to plain TCP.
  OpenSslTransport() = default;
  OpenSslTransport(Options options);
  bool send(const std::string& request, const std::string& host, const std::string& port,
            std::string& response, std::string& error) override;

 private:
  Options options_;
};

class PlainTcpTransport final : public HttpTransport {
 public:
  bool send(const std::string& request, const std::string& host, const std::string& port,
            std::string& response, std::string& error) override;
};

class HttpClient final {
 public:
  static std::unique_ptr<HttpClient> createOpenSsl();
  static std::unique_ptr<HttpClient> createPlainTcp();

  // Create an OpenSslTransport with explicit TLS options.
  // Defaults: verify peer certificates and use the host as SNI name.
  static std::unique_ptr<HttpClient> createOpenSsl(OpenSslTransport::Options options);

  explicit HttpClient(std::shared_ptr<HttpTransport> transport);

  // Port is required so callers stay explicit about where the request goes.
  // Pass "443" or "80" for the scheme defaults.
  bool request(const HttpRequest& request, HttpResponse& out, std::string& error);

 private:
  std::shared_ptr<HttpTransport> transport_;
};

}  // namespace http
}  // namespace bjorne_agent
