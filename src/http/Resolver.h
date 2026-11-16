#pragma once

#include "platform/Platform.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <memory>
#include <string>
#include <vector>

namespace bjorne_agent {
namespace http {

// One address to connect to, already carrying the port.
struct ResolvedAddress {
  sockaddr_storage storage{};
  platform::SocketLength length = 0;
};

// The network half of name resolution: turns a name into the addresses to try.
// A build that links dynamically asks the operating system. Only the static
// build has a use for an alternative: a static glibc cannot resolve names
// through getaddrinfo because the call needs the NSS modules, which are shared
// objects, and loading one from a static binary crashes. That build asks the
// nameservers in /etc/resolv.conf directly instead.
//
// Tests substitute a fake, so a unit test never depends on the machine's DNS
// configuration and never leaves the machine.
class NameService {
 public:
  virtual ~NameService();

  // Fills addresses with the addresses for host and port, in preference order.
  // Returns false and sets error when the name does not resolve. A failing
  // implementation leaves addresses untouched.
  virtual bool resolve(const std::string& host, unsigned short port, std::vector<ResolvedAddress>& addresses,
                       std::string& error) = 0;
};

// The name service this build uses when the caller supplies none. It exists as
// a factory so that resolution has exactly one default and a test can reach
// the real adapter without substituting anything.
std::unique_ptr<NameService> makePlatformNameService();

// The static build resolves names through /etc/hosts before it asks the name
// service. hosts_path lets tests point at a fixture rather than the real file.
struct ResolverOptions {
  std::string hosts_path = "/etc/hosts";
  // Null means the platform lookup. Tests set a fake.
  NameService* name_service = nullptr;
};

// Resolves host and port into the addresses to try, in preference order.
// Numeric addresses work in every build. On failure error starts with
// "DNS resolution failed".
bool resolveHost(const std::string& host, const std::string& port, std::vector<ResolvedAddress>& addresses,
                 std::string& error, const ResolverOptions& options = ResolverOptions());

}  // namespace http
}  // namespace bjorne_agent
