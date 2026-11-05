#include "http/DnsResponse.h"

#include "platform/Platform.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

namespace {

void pushU16(std::vector<unsigned char>& out, unsigned int value) {
  out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
  out.push_back(static_cast<unsigned char>(value & 0xFF));
}

void pushName(std::vector<unsigned char>& out, const std::string& name) {
  size_t start = 0;
  while (start < name.size()) {
    const size_t dot = name.find('.', start);
    const size_t end = dot == std::string::npos ? name.size() : dot;
    out.push_back(static_cast<unsigned char>(end - start));
    out.insert(out.end(), name.begin() + static_cast<std::ptrdiff_t>(start),
               name.begin() + static_cast<std::ptrdiff_t>(end));
    start = end + 1;
  }
  out.push_back(0);
}

std::vector<unsigned char> header(int questions, int answers) {
  std::vector<unsigned char> out;
  pushU16(out, 0x1234);
  pushU16(out, 0x8180);
  pushU16(out, static_cast<unsigned int>(questions));
  pushU16(out, static_cast<unsigned int>(answers));
  pushU16(out, 0);
  pushU16(out, 0);
  return out;
}

// The question section every answer below points back at with 0xC00C.
void pushQuestion(std::vector<unsigned char>& out) {
  pushName(out, "example.com");
  pushU16(out, 1);
  pushU16(out, 1);
}

void pushRecordHeader(std::vector<unsigned char>& out, unsigned int type, unsigned int klass,
                      unsigned int length) {
  out.push_back(0xC0);
  out.push_back(0x0C);
  pushU16(out, type);
  pushU16(out, klass);
  pushU16(out, 0);
  pushU16(out, 60);
  pushU16(out, length);
}

std::string addressText(const bjorne_agent::http::ResolvedAddress& address) {
  char buffer[INET6_ADDRSTRLEN] = {};
  const void* raw = nullptr;
  if (address.storage.ss_family == AF_INET) {
    raw = &reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_addr;
  } else {
    raw = &reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_addr;
  }
  bjorne_agent::platform::addressToText(address.storage.ss_family, raw, buffer, sizeof(buffer));
  return buffer;
}

unsigned short portOf(const bjorne_agent::http::ResolvedAddress& address) {
  if (address.storage.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_port);
  }
  return ntohs(reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_port);
}

}  // namespace

TEST_CASE("DNS response yields an IPv4 address") {
  std::vector<unsigned char> packet = header(1, 1);
  pushQuestion(packet);
  pushRecordHeader(packet, 1, 1, 4);
  packet.insert(packet.end(), {93, 184, 216, 34});

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 443, addresses));
  REQUIRE(addresses.size() == 1);
  REQUIRE(addresses[0].storage.ss_family == AF_INET);
  REQUIRE(addressText(addresses[0]) == "93.184.216.34");
  REQUIRE(portOf(addresses[0]) == 443);
}

TEST_CASE("DNS response yields an IPv6 address") {
  std::vector<unsigned char> packet = header(1, 1);
  pushQuestion(packet);
  pushRecordHeader(packet, 28, 1, 16);
  const std::array<unsigned char, 16> raw = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  packet.insert(packet.end(), raw.begin(), raw.end());

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 80, addresses));
  REQUIRE(addresses.size() == 1);
  REQUIRE(addresses[0].storage.ss_family == AF_INET6);
  REQUIRE(addressText(addresses[0]) == "2001:db8::1");
  REQUIRE(portOf(addresses[0]) == 80);
}

TEST_CASE("DNS response skips records that are not addresses") {
  std::vector<unsigned char> packet = header(1, 3);
  pushQuestion(packet);
  pushRecordHeader(packet, 16, 1, 5);
  packet.insert(packet.end(), {'h', 'e', 'l', 'l', 'o'});
  pushRecordHeader(packet, 1, 3, 4);  // CHAOS class, not Internet
  packet.insert(packet.end(), {10, 0, 0, 1});
  pushRecordHeader(packet, 1, 1, 4);
  packet.insert(packet.end(), {10, 0, 0, 2});

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 8080, addresses));
  REQUIRE(addresses.size() == 1);
  REQUIRE(addressText(addresses[0]) == "10.0.0.2");
}

TEST_CASE("DNS response accepts an answer name with a label before the pointer") {
  std::vector<unsigned char> packet = header(1, 1);
  pushQuestion(packet);
  packet.insert(packet.end(), {3, 'w', 'w', 'w', 0xC0, 0x0C});
  pushU16(packet, 1);
  pushU16(packet, 1);
  pushU16(packet, 0);
  pushU16(packet, 60);
  pushU16(packet, 4);
  packet.insert(packet.end(), {10, 0, 0, 3});

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 80, addresses));
  REQUIRE(addresses.size() == 1);
  REQUIRE(addressText(addresses[0]) == "10.0.0.3");
}

TEST_CASE("DNS response rejects a truncated header") {
  const std::vector<unsigned char> packet = header(0, 0);
  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE_FALSE(bjorne_agent::http::parseDnsResponse(packet.data(), 8, 80, addresses));
  REQUIRE(addresses.empty());
}

TEST_CASE("DNS response rejects a truncated question") {
  std::vector<unsigned char> packet = header(1, 0);
  packet.insert(packet.end(), {7, 'e', 'x', 'a', 'm'});  // name runs past the buffer

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE_FALSE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 80, addresses));
}

TEST_CASE("DNS response rejects a question without type and class") {
  std::vector<unsigned char> packet = header(1, 0);
  pushName(packet, "example.com");
  pushU16(packet, 1);

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE_FALSE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 80, addresses));
}

TEST_CASE("DNS response rejects an answer whose data runs past the buffer") {
  std::vector<unsigned char> packet = header(1, 1);
  pushQuestion(packet);
  pushRecordHeader(packet, 1, 1, 4);
  packet.insert(packet.end(), {10, 0});

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE_FALSE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 80, addresses));
}

TEST_CASE("DNS response rejects a truncated answer name") {
  std::vector<unsigned char> packet = header(1, 1);
  pushQuestion(packet);
  packet.insert(packet.end(), {9, 'b', 'r', 'o'});  // name runs past the buffer

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE_FALSE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 80, addresses));
}

TEST_CASE("DNS response rejects an answer with no room for the record header") {
  std::vector<unsigned char> packet = header(1, 1);
  pushQuestion(packet);
  packet.push_back(0);  // empty name, then nothing

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE_FALSE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 80, addresses));
}

TEST_CASE("DNS response rejects a dangling compression pointer") {
  std::vector<unsigned char> packet = header(1, 1);
  pushQuestion(packet);
  packet.push_back(0xC0);  // pointer with no second byte

  std::vector<bjorne_agent::http::ResolvedAddress> addresses;
  REQUIRE_FALSE(bjorne_agent::http::parseDnsResponse(packet.data(), static_cast<int>(packet.size()), 80, addresses));
}
