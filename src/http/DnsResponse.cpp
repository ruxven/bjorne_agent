#include "http/DnsResponse.h"

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <vector>

namespace bjorne_agent {
namespace http {

namespace {

constexpr int kHeaderSize = 12;
constexpr unsigned short kClassInternet = 1;
constexpr unsigned short kTypeA = 1;
constexpr unsigned short kTypeAaaa = 28;

unsigned short readU16(const unsigned char* data) {
  return static_cast<unsigned short>((static_cast<unsigned short>(data[0]) << 8) | data[1]);
}

// Returns the offset just past the name at offset, or -1 when the record is
// truncated. A compression pointer ends the name in this record, so the labels
// it points at are never visited here.
int skipName(const unsigned char* data, int size, int offset) {
  int position = offset;
  while (position < size) {
    const unsigned char length = data[position];
    if ((length & 0xC0) == 0xC0) {
      return position + 2 <= size ? position + 2 : -1;
    }
    if (length == 0) {
      return position + 1;
    }
    position += 1 + static_cast<int>(length);
  }
  return -1;
}

bool appendRecord(const unsigned char* data, int size, int offset, unsigned short port,
                  std::vector<ResolvedAddress>& addresses) {
  if (offset + 10 > size) {
    return false;
  }
  const unsigned short type = readU16(data + offset);
  const unsigned short klass = readU16(data + offset + 2);
  const unsigned short length = readU16(data + offset + 8);
  const int start = offset + 10;
  if (start + static_cast<int>(length) > size) {
    return false;
  }
  ResolvedAddress address;
  if (klass == kClassInternet && type == kTypeA && length == 4) {
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(port);
    std::memcpy(&v4.sin_addr, data + start, 4);
    std::memcpy(&address.storage, &v4, sizeof(v4));
    address.length = sizeof(v4);
  } else if (klass == kClassInternet && type == kTypeAaaa && length == 16) {
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(port);
    std::memcpy(&v6.sin6_addr, data + start, 16);
    std::memcpy(&address.storage, &v6, sizeof(v6));
    address.length = sizeof(v6);
  } else {
    return true;
  }
  addresses.push_back(address);
  return true;
}

}  // namespace

bool parseDnsResponse(const unsigned char* data, int size, unsigned short port,
                      std::vector<ResolvedAddress>& addresses) {
  if (size < kHeaderSize) {
    return false;
  }
  const int questions = readU16(data + 4);
  const int records = readU16(data + 6);
  int offset = kHeaderSize;
  for (int i = 0; i < questions; ++i) {
    offset = skipName(data, size, offset);
    if (offset < 0 || offset + 4 > size) {
      return false;
    }
    offset += 4;
  }
  for (int i = 0; i < records; ++i) {
    offset = skipName(data, size, offset);
    if (offset < 0 || !appendRecord(data, size, offset, port, addresses)) {
      return false;
    }
    offset += 10 + static_cast<int>(readU16(data + offset + 8));
  }
  return true;
}

}  // namespace http
}  // namespace bjorne_agent
