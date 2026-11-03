#include "http/Resolver.h"

#include "http/DnsResponse.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// A static glibc cannot resolve names through getaddrinfo: the call needs the
// NSS modules, which are shared objects, and loading one from a static binary
// crashes. The static build therefore resolves through /etc/hosts and the libc
// stub resolver, which talks to the nameservers in /etc/resolv.conf directly.
// BJORNE_AGENT_USE_STUB_RESOLVER comes from the build and follows BJORNE_AGENT_STATIC.
#if BJORNE_AGENT_USE_STUB_RESOLVER
#include <resolv.h>
#endif

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace bjorne_agent {
namespace http {

namespace {

bool parsePort(const std::string& port, unsigned short& value) {
  if (port.empty()) {
    return false;
  }
  unsigned long parsed = 0;
  for (const char c : port) {
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + static_cast<unsigned long>(c - '0');
    if (parsed > 65535) {
      return false;
    }
  }
  if (parsed == 0) {
    return false;
  }
  value = static_cast<unsigned short>(parsed);
  return true;
}

// The endpoint parser keeps the brackets on an IPv6 literal so the Host header
// stays correct. inet_pton wants the bare form.
std::string withoutBrackets(const std::string& host) {
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    return host.substr(1, host.size() - 2);
  }
  return host;
}

bool numericAddress(const std::string& host, unsigned short port, ResolvedAddress& out) {
  const std::string bare = withoutBrackets(host);
  sockaddr_in v4{};
  if (::inet_pton(AF_INET, bare.c_str(), &v4.sin_addr) == 1) {
    v4.sin_family = AF_INET;
    v4.sin_port = htons(port);
    std::memcpy(&out.storage, &v4, sizeof(v4));
    out.length = sizeof(v4);
    return true;
  }
  sockaddr_in6 v6{};
  if (::inet_pton(AF_INET6, bare.c_str(), &v6.sin6_addr) == 1) {
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(port);
    std::memcpy(&out.storage, &v6, sizeof(v6));
    out.length = sizeof(v6);
    return true;
  }
  return false;
}

#if BJORNE_AGENT_USE_STUB_RESOLVER

bool readFile(const std::string& path, std::string& contents) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  char buffer[4096];
  for (;;) {
    const ssize_t bytes = ::read(fd, buffer, sizeof(buffer));
    if (bytes < 0) {
      ::close(fd);
      return false;
    }
    if (bytes == 0) {
      break;
    }
    contents.append(buffer, static_cast<size_t>(bytes));
  }
  ::close(fd);
  return true;
}

std::vector<std::string> splitFields(const std::string& line) {
  std::vector<std::string> fields;
  size_t position = 0;
  while (position < line.size()) {
    while (position < line.size() && std::isspace(static_cast<unsigned char>(line[position])) != 0) {
      ++position;
    }
    const size_t start = position;
    while (position < line.size() && std::isspace(static_cast<unsigned char>(line[position])) == 0) {
      ++position;
    }
    if (start != position) {
      fields.push_back(line.substr(start, position - start));
    }
  }
  return fields;
}

// One /etc/hosts line: an address, then one or more names for it.
bool hostsLineMatches(const std::string& line, const std::string& host, std::string& address) {
  const size_t comment = line.find('#');
  const std::vector<std::string> fields = splitFields(comment == std::string::npos ? line : line.substr(0, comment));
  if (fields.size() < 2) {
    return false;
  }
  for (size_t i = 1; i < fields.size(); ++i) {
    if (::strcasecmp(fields[i].c_str(), host.c_str()) == 0) {
      address = fields[0];
      return true;
    }
  }
  return false;
}

bool lookupHosts(const std::string& path, const std::string& host, unsigned short port,
                 std::vector<ResolvedAddress>& addresses) {
  std::string contents;
  if (!readFile(path, contents)) {
    return false;
  }
  size_t start = 0;
  for (;;) {
    const size_t end = contents.find('\n', start);
    const std::string line = contents.substr(start, end == std::string::npos ? std::string::npos : end - start);
    std::string address;
    ResolvedAddress resolved;
    if (hostsLineMatches(line, host, address) && numericAddress(address, port, resolved)) {
      addresses.push_back(resolved);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return !addresses.empty();
}

bool queryDns(const std::string& host, int type, unsigned short port, std::vector<ResolvedAddress>& addresses) {
  unsigned char response[4096];
  const int size = ::res_search(host.c_str(), C_IN, type, response, static_cast<int>(sizeof(response)));
  if (size < 0) {
    return false;
  }
  return parseDnsResponse(response, size, port, addresses);
}

bool resolveStub(const std::string& host, unsigned short port, std::vector<ResolvedAddress>& addresses,
                 std::string& error, const ResolverOptions& options) {
  if (lookupHosts(options.hosts_path, host, port, addresses)) {
    return true;
  }
  int code = NO_DATA;
  if (!queryDns(host, T_A, port, addresses)) {
    code = h_errno;
  }
  if (!queryDns(host, T_AAAA, port, addresses)) {
    code = h_errno;
  }
  if (!addresses.empty()) {
    return true;
  }
  error = std::string("DNS resolution failed: ") + ::hstrerror(code);
  return false;
}

#else

bool resolveWithGetaddrinfo(const std::string& host, const std::string& port,
                            std::vector<ResolvedAddress>& addresses, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
  if (rc != 0) {
    error = std::string("DNS resolution failed: ") + ::gai_strerror(rc);
    return false;
  }
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    if (entry->ai_addrlen > sizeof(ResolvedAddress::storage)) {
      continue;
    }
    ResolvedAddress address;
    std::memcpy(&address.storage, entry->ai_addr, entry->ai_addrlen);
    address.length = static_cast<socklen_t>(entry->ai_addrlen);
    addresses.push_back(address);
  }
  ::freeaddrinfo(results);
  if (addresses.empty()) {
    error = "DNS resolution failed: no addresses";
    return false;
  }
  return true;
}

#endif

}  // namespace

bool resolveHost(const std::string& host, const std::string& port, std::vector<ResolvedAddress>& addresses,
                 std::string& error, const ResolverOptions& options) {
  addresses.clear();
  unsigned short number = 0;
  if (!parsePort(port, number)) {
    error = "Invalid port: " + port;
    return false;
  }
  ResolvedAddress literal;
  if (numericAddress(host, number, literal)) {
    addresses.push_back(literal);
    return true;
  }
#if BJORNE_AGENT_USE_STUB_RESOLVER
  return resolveStub(host, number, addresses, error, options);
#else
  (void)options;
  return resolveWithGetaddrinfo(host, port, addresses, error);
#endif
}

}  // namespace http
}  // namespace bjorne_agent
