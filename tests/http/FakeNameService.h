#pragma once

#include "http/Resolver.h"

#include "platform/Platform.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <cstring>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

namespace bjorne_agent_test {

// Builds an address from an IPv4 literal, so a fake name service can hand back
// a fixed answer.
inline bjorne_agent::http::ResolvedAddress ipv4Address(const char* text, unsigned short port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  REQUIRE(bjorne_agent::platform::addressToBinary(AF_INET, text, &address.sin_addr) == 1);
  bjorne_agent::http::ResolvedAddress resolved;
  std::memcpy(&resolved.storage, &address, sizeof(address));
  resolved.length = sizeof(address);
  return resolved;
}

// A name service with a fixed answer that records every lookup. A test uses it
// so resolution never asks the machine's DNS or reaches the network, and so it
// can prove whether the lookup happened at all.
class FakeNameService final : public bjorne_agent::http::NameService {
 public:
  bool resolve(const std::string& host, unsigned short port,
               std::vector<bjorne_agent::http::ResolvedAddress>& addresses, std::string& error) override {
    lookups.push_back(host + ":" + std::to_string(port));
    if (!answer) {
      error = failure;
      return false;
    }
    addresses.push_back(ipv4Address(address_text.c_str(), port));
    return true;
  }

  std::string address_text = "192.0.2.10";
  std::string failure = "DNS resolution failed: no such host";
  bool answer = true;
  std::vector<std::string> lookups;
};

}  // namespace bjorne_agent_test
