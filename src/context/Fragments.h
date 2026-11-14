#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bjorne_agent {
namespace context {

// One piece of context assembled before a task. The title names where the text
// came from, either a file path or a generated section, and goes into the
// heading so the model can tell the pieces apart.
struct Fragment {
  std::string title;
  std::string content;
};

struct FragmentLimits {
  // Total bytes of fragment text that may reach the system prompt. The cap
  // keeps a runaway AGENTS.md or a large skill index from crowding out the task
  // itself.
  std::size_t max_bytes = 64 * 1024;
};

// Reads a file into a fragment titled by its path. Returns false when the file
// cannot be read.
bool readFragment(const std::string& path, Fragment& out);

// Renders fragments as Markdown sections in the order they are given, so the
// caller controls precedence and the most specific fragment goes last. Text
// past the limit is cut at a character boundary and marked, and any later
// fragment is dropped.
std::string renderFragments(const std::vector<Fragment>& fragments, FragmentLimits limits = {});

}  // namespace context
}  // namespace bjorne_agent
