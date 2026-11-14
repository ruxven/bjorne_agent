#include "context/AgentsFiles.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace bjorne_agent {
namespace context {

namespace {

constexpr const char* kAgentsFileName = "AGENTS.md";

std::filesystem::path absolutePath(const std::string& path) {
  std::error_code code;
  const std::filesystem::path absolute = std::filesystem::absolute(path, code);
  if (code || absolute.empty()) {
    return std::filesystem::path(path);
  }
  return absolute.lexically_normal();
}

bool isRegularFile(const std::filesystem::path& path) {
  std::error_code code;
  const bool regular = std::filesystem::is_regular_file(path, code);
  return !code && regular;
}

// Two candidates can name the same file through different paths, for example
// when the global config directory sits inside the workspace. Comparison uses
// the canonical path, so such a file is included once.
std::string comparisonKey(const std::filesystem::path& path) {
  std::error_code code;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, code);
  if (code) {
    return path.lexically_normal().string();
  }
  return canonical.string();
}

}  // namespace

AgentsFiles loadAgentsFiles(const std::string& workspace, const std::string& global_config_directory) {
  AgentsFiles result;

  std::vector<std::filesystem::path> candidates;
  if (!global_config_directory.empty()) {
    const std::filesystem::path global = std::filesystem::path(global_config_directory) / kAgentsFileName;
    if (isRegularFile(global)) {
      candidates.push_back(global);
    }
  }

  // Walking up from the workspace finds the nearest file first. The list is
  // reversed below so the nearest file comes last in the context.
  std::filesystem::path directory = absolutePath(workspace.empty() ? "." : workspace);
  std::vector<std::filesystem::path> nearestFirst;
  while (true) {
    const std::filesystem::path candidate = directory / kAgentsFileName;
    if (isRegularFile(candidate)) {
      nearestFirst.push_back(candidate);
    }
    const std::filesystem::path parent = directory.parent_path();
    if (parent.empty() || parent == directory) {
      break;
    }
    directory = parent;
  }
  for (auto it = nearestFirst.rbegin(); it != nearestFirst.rend(); ++it) {
    candidates.push_back(*it);
  }

  std::vector<std::string> seen;
  for (const std::filesystem::path& candidate : candidates) {
    const std::string key = comparisonKey(candidate);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
      continue;
    }
    seen.push_back(key);

    Fragment fragment;
    if (!readFragment(candidate.string(), fragment)) {
      result.unreadable.push_back(candidate.string());
      continue;
    }
    fragment.title = std::string(kAgentsFileName) + ": " + candidate.string();
    result.fragments.push_back(std::move(fragment));
  }
  return result;
}

}  // namespace context
}  // namespace bjorne_agent
