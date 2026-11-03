#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <string>
#include <vector>

namespace bjorne_agent {
namespace http {

// One address to connect to, already carrying the port.
struct ResolvedAddress {
  sockaddr_storage storage{};
  socklen_t length = 0;
};

// Only the static build has a use for this. A static glibc cannot load the NSS
// modules getaddrinfo needs, so the static build resolves through /etc/hosts
// and the libc stub resolver instead. hosts_path lets tests point at a fixture
// rather than the real file.
struct ResolverOptions {
  std::string hosts_path = "/etc/hosts";
};

// Resolves host and port into the addresses to try, in preference order.
// Numeric addresses work in every build. On failure error starts with
// "DNS resolution failed".
bool resolveHost(const std::string& host, const std::string& port, std::vector<ResolvedAddress>& addresses,
                 std::string& error, const ResolverOptions& options = ResolverOptions());

}  // namespace http
}  // namespace bjorne_agent
