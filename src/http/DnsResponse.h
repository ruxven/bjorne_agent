#pragma once

#include "http/Resolver.h"

#include <vector>

namespace bjorne_agent {
namespace http {

// Parses a DNS response into the A and AAAA addresses it carries. Records of
// other types are skipped. Returns false when the response is truncated.
bool parseDnsResponse(const unsigned char* data, int size, unsigned short port,
                      std::vector<ResolvedAddress>& addresses);

}  // namespace http
}  // namespace bjorne_agent
