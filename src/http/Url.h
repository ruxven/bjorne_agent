#pragma once

#include <string>

namespace bjorne_agent {
namespace http {

// Parsed base URL for a model endpoint. The scheme picks the transport:
// https uses TLS, http uses plain TCP. Ports default from the scheme.
struct Url {
  std::string scheme;
  std::string host;
  std::string port;
  std::string path;  // path prefix, empty or starting with '/', no trailing '/'
  bool tls = false;
};

// Parses a base URL such as https://api.example.com/v1 or
// http://127.0.0.1:8080/v1. IPv6 hosts use brackets: http://[::1]:8080/v1.
// Rejects a missing scheme, an unsupported scheme, an empty host, and a
// non numeric port.
bool parseUrl(const std::string& text, Url& out, std::string& error);

// True for a port that is the default for its scheme. Used to decide whether
// the Host header carries the port.
bool isDefaultPort(const std::string& port);

}  // namespace http
}  // namespace bjorne_agent
