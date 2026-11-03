#include "http/Resolver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <catch_amalgamated.hpp>

namespace {

std::string addressText(const bjorne_agent::http::ResolvedAddress& address) {
  char buffer[INET6_ADDRSTRLEN] = {};
  const void* raw = nullptr;
  if (address.storage.ss_family == AF_INET) {
    raw = &reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_addr;
  } else {
    raw = &reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_addr;
  }
  ::inet_ntop(address.storage.ss_family, raw, buffer, sizeof(buffer));
  return buffer;
}

unsigned short portOf(const bjorne_agent::http::ResolvedAddress& address) {
  if (address.storage.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_port);
  }
  return ntohs(reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_port);
}

}  // namespace

TEST_CASE("Resolver accepts a numeric IPv4 host") {
  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE(bjorne_agent::http::resolveHost("127.0.0.1", "8080", addresses, error));
  REQUIRE(error.empty());
  REQUIRE(addresses.size() == 1);
  REQUIRE(addresses[0].storage.ss_family == AF_INET);
  REQUIRE(addressText(addresses[0]) == "127.0.0.1");
  REQUIRE(portOf(addresses[0]) == 8080);
}

TEST_CASE("Resolver accepts a bracketed numeric IPv6 host") {
  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE(bjorne_agent::http::resolveHost("[::1]", "443", addresses, error));
  REQUIRE(error.empty());
  REQUIRE(addresses.size() == 1);
  REQUIRE(addresses[0].storage.ss_family == AF_INET6);
  REQUIRE(addressText(addresses[0]) == "::1");
  REQUIRE(portOf(addresses[0]) == 443);
}

TEST_CASE("Resolver rejects a port that is not numeric") {
  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::http::resolveHost("127.0.0.1", "https", addresses, error));
  REQUIRE(error == "Invalid port: https");
  REQUIRE(addresses.empty());
}

TEST_CASE("Resolver rejects a port outside the valid range") {
  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::http::resolveHost("127.0.0.1", "70000", addresses, error));
  REQUIRE(error == "Invalid port: 70000");
}

TEST_CASE("Resolver rejects a missing port") {
  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::http::resolveHost("127.0.0.1", "", addresses, error));
  REQUIRE(error == "Invalid port: ");
}

TEST_CASE("Resolver rejects port zero") {
  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::http::resolveHost("127.0.0.1", "0", addresses, error));
  REQUIRE(error == "Invalid port: 0");
}

TEST_CASE("Resolver resolves a name from the system database") {
  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE(bjorne_agent::http::resolveHost("localhost", "80", addresses, error));
  REQUIRE(error.empty());
  REQUIRE_FALSE(addresses.empty());
  for (const auto& address : addresses) {
    REQUIRE((address.storage.ss_family == AF_INET || address.storage.ss_family == AF_INET6));
    REQUIRE(portOf(address) == 80);
  }
}

TEST_CASE("Resolver drops addresses from a previous call") {
  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE(bjorne_agent::http::resolveHost("127.0.0.1", "80", addresses, error));
  REQUIRE(addresses.size() == 1);
  REQUIRE_FALSE(bjorne_agent::http::resolveHost("127.0.0.1", "bad", addresses, error));
  REQUIRE(addresses.empty());
}

#if BJORNE_AGENT_USE_STUB_RESOLVER

namespace {

// The path carries the process id so that two test binaries running at the
// same time do not overwrite each other's fixture.
std::string hostsPath() {
  return (std::filesystem::temp_directory_path() / ("bjorne_agent_resolver_hosts_" + std::to_string(::getpid())))
      .string();
}

std::string writeHosts(const std::string& contents) {
  const std::string path = hostsPath();
  std::ofstream file(path);
  file << contents;
  return path;
}

bjorne_agent::http::ResolverOptions hostsOptions() {
  bjorne_agent::http::ResolverOptions options;
  options.hosts_path = hostsPath();
  return options;
}

}  // namespace

TEST_CASE("Stub resolver reads a name from the hosts file") {
  writeHosts("127.0.0.1 localhost\n10.1.2.3 api.example.test alias.example.test\n");

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE(bjorne_agent::http::resolveHost("api.example.test", "8443", addresses, error, hostsOptions()));
  REQUIRE(error.empty());
  REQUIRE(addresses.size() == 1);
  REQUIRE(addressText(addresses[0]) == "10.1.2.3");
  REQUIRE(portOf(addresses[0]) == 8443);
}

TEST_CASE("Stub resolver matches aliases, comments, and case") {
  writeHosts(
      "# leading comment\n"
      "\n"
      "10.9.8.7 API.Example.Test\n"
      "::1\tipv6.example.test   # trailing comment\n"
      "single-field-line\n"
      "not-an-address broken.example.test\n");

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE(bjorne_agent::http::resolveHost("api.example.test", "80", addresses, error, hostsOptions()));
  REQUIRE(addresses.size() == 1);
  REQUIRE(addressText(addresses[0]) == "10.9.8.7");

  addresses.clear();
  REQUIRE(bjorne_agent::http::resolveHost("ipv6.example.test", "80", addresses, error, hostsOptions()));
  REQUIRE(addresses.size() == 1);
  REQUIRE(addresses[0].storage.ss_family == AF_INET6);
  REQUIRE(addressText(addresses[0]) == "::1");

  addresses.clear();
  REQUIRE_FALSE(bjorne_agent::http::resolveHost("broken.example.test", "80", addresses, error, hostsOptions()));
  REQUIRE(addresses.empty());
}

TEST_CASE("Stub resolver falls back to DNS when the hosts file is missing") {
  bjorne_agent::http::ResolverOptions options;
  options.hosts_path = "/tmp/opencode/definitely_missing_bjorne_agent_hosts";

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::http::resolveHost("bjorne_agent.invalid", "80", addresses, error, options));
  REQUIRE(addresses.empty());
  REQUIRE(error.rfind("DNS resolution failed", 0) == 0);
}

TEST_CASE("Stub resolver falls back to DNS when the hosts path cannot be read") {
  // Opening a directory succeeds; reading it fails, which is the read error
  // path in the file loader.
  bjorne_agent::http::ResolverOptions options;
  options.hosts_path = std::filesystem::temp_directory_path().string();

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  std::string error;
  REQUIRE_FALSE(bjorne_agent::http::resolveHost("bjorne_agent.invalid", "80", addresses, error, options));
  REQUIRE(addresses.empty());
  REQUIRE(error.rfind("DNS resolution failed", 0) == 0);
}

#endif
