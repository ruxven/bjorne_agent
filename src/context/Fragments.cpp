#include "context/Fragments.h"

#include <fstream>
#include <sstream>

namespace bjorne_agent {
namespace context {

namespace {

constexpr const char* kTruncatedMarker = "\n\n[context truncated]";

// Backs up to the start of the character that contains `cut`, so a cut never
// splits a UTF-8 sequence. The model receives valid text even when the cap
// falls in the middle of a multi-byte character.
std::size_t utf8Boundary(const std::string& text, std::size_t cut) {
  if (cut >= text.size()) {
    return text.size();
  }
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return cut;
}

}  // namespace

bool readFragment(const std::string& path, Fragment& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    return false;
  }
  out.title = path;
  out.content = buffer.str();
  return true;
}

std::string renderFragments(const std::vector<Fragment>& fragments, FragmentLimits limits) {
  if (limits.max_bytes == 0) {
    return {};
  }

  std::string out;
  bool truncated = false;
  for (const Fragment& fragment : fragments) {
    if (fragment.content.empty()) {
      continue;
    }
    if (out.size() >= limits.max_bytes) {
      truncated = true;
      break;
    }

    std::string section;
    if (!out.empty()) {
      section += "\n\n";
    }
    section += "## " + fragment.title + "\n\n" + fragment.content;
    if (out.size() + section.size() <= limits.max_bytes) {
      out += section;
      continue;
    }

    const std::size_t room = limits.max_bytes - out.size();
    out += section.substr(0, utf8Boundary(section, room));
    truncated = true;
    break;
  }

  if (truncated) {
    out += kTruncatedMarker;
  }
  return out;
}

}  // namespace context
}  // namespace bjorne_agent
