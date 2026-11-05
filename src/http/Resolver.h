#pragma once

#include "platform/Platform.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <string>
#include <vector>

namespace bjorne_agent {
namespace http {

// One address to connect to, already carrying the port.
struct ResolvedAddress {
  sockaddr_storage storage{};
  platform::SocketLength length = 0;
};

// Only the static build has a use for this. A static glibc cannot resolve names through getaddrinfo: the call needs the NSS
// modules, which are shared objects, and loading one from a static binary
// crashes. The static build therefore resolves through /etc/hosts and the libc
// stub resolver, which talks to the nameservers in /etc/resolv.conf directly.
// hosts_path lets tests point at a fixture rather than the real file.
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
