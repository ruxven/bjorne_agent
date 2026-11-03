#include "http/Url.h"

#include <string>

namespace bjorne_agent {
namespace http {

namespace {

bool allDigits(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

// Splits the authority into host and optional port. Bracketed IPv6 hosts keep
// their brackets so they can go straight back into getaddrinfo and the Host
// header.
bool splitAuthority(const std::string& authority, Url& url, std::string& error) {
  std::string host = authority;
  std::string port;

  if (!host.empty() && host.front() == '[') {
    const size_t close = host.find(']');
    if (close == std::string::npos) {
      error = "Endpoint has an unterminated IPv6 host: " + authority;
      return false;
    }
    const std::string after = host.substr(close + 1);
    if (!after.empty()) {
      if (after.front() != ':') {
        error = "Endpoint has an invalid host suffix: " + authority;
        return false;
      }
      port = after.substr(1);
    }
    host = host.substr(0, close + 1);
  } else {
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
      port = host.substr(colon + 1);
      host = host.substr(0, colon);
    }
  }

  if (host.empty() || host == "[]") {
    error = "Endpoint is missing a host: " + authority;
    return false;
  }
  if (!port.empty()) {
    if (!allDigits(port)) {
      error = "Endpoint port is not numeric: " + port;
      return false;
    }
    url.port = port;
  }
  url.host = host;
  return true;
}

}  // namespace

bool isDefaultPort(const std::string& port) { return port.empty() || port == "443" || port == "80"; }

bool parseUrl(const std::string& text, Url& out, std::string& error) {
  const size_t schemeEnd = text.find("://");
  if (schemeEnd == std::string::npos) {
    error = "Endpoint must include a scheme: " + text;
    return false;
  }

  Url url;
  url.scheme = text.substr(0, schemeEnd);
  if (url.scheme == "https") {
    url.tls = true;
    url.port = "443";
  } else if (url.scheme == "http") {
    url.port = "80";
  } else {
    error = "Unsupported endpoint scheme: " + url.scheme;
    return false;
  }

  std::string rest = text.substr(schemeEnd + 3);
  const size_t pathStart = rest.find('/');
  if (pathStart != std::string::npos) {
    url.path = rest.substr(pathStart);
    rest = rest.substr(0, pathStart);
  }
  while (!url.path.empty() && url.path.back() == '/') {
    url.path.pop_back();
  }

  if (!splitAuthority(rest, url, error)) {
    return false;
  }

  out = url;
  return true;
}

}  // namespace http
}  // namespace bjorne_agent
