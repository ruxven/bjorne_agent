#include "skills/Skills.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bjorne_agent {
namespace skills {

namespace {

constexpr const char* kSkillFileName = "SKILL.md";

bool readFile(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    return false;
  }
  out = buffer.str();
  return true;
}

std::string trim(const std::string& text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::string unquote(const std::string& value) {
  if (value.size() >= 2) {
    const char first = value.front();
    const char last = value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      return value.substr(1, value.size() - 2);
    }
  }
  return value;
}

// Reads one line without its terminator and moves position past it. Both LF
// and CRLF are accepted, so a file written on Windows works unchanged.
std::string nextLine(const std::string& text, std::size_t& position) {
  const std::size_t start = position;
  while (position < text.size() && text[position] != '\n') {
    ++position;
  }
  std::string line = text.substr(start, position - start);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (position < text.size()) {
    ++position;
  }
  return line;
}

// A minimal key: value scanner. The frontmatter is a flat map, which is all
// the skill format needs; a YAML library would breach the no dependency rule.
bool parseFrontmatter(const std::string& text, std::map<std::string, std::string>& values, std::string& body,
                      std::string& reason) {
  std::size_t position = 0;
  // An editor may write a UTF-8 byte order mark before the opening delimiter.
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
    position = 3;
  }
  if (trim(nextLine(text, position)) != "---") {
    reason = "missing frontmatter";
    return false;
  }

  bool closed = false;
  while (position < text.size()) {
    const std::string line = nextLine(text, position);
    if (trim(line) == "---") {
      closed = true;
      break;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = trim(line.substr(0, colon));
    if (key.empty()) {
      continue;
    }
    values[key] = unquote(trim(line.substr(colon + 1)));
  }
  if (!closed) {
    reason = "unterminated frontmatter";
    return false;
  }

  body = text.substr(position);
  if (body.rfind("\r\n", 0) == 0) {
    body.erase(0, 2);
  } else if (!body.empty() && body.front() == '\n') {
    body.erase(0, 1);
  }
  return true;
}

bool parseSkillFile(const std::string& path, Skill& skill, std::string& reason) {
  std::string text;
  if (!readFile(path, text)) {
    reason = "cannot read file";
    return false;
  }

  std::map<std::string, std::string> values;
  std::string body;
  if (!parseFrontmatter(text, values, body, reason)) {
    return false;
  }

  const auto name = values.find("name");
  if (name == values.end() || trim(name->second).empty()) {
    reason = "missing name";
    return false;
  }
  const auto description = values.find("description");
  if (description == values.end() || trim(description->second).empty()) {
    reason = "missing description";
    return false;
  }

  skill.name = trim(name->second);
  skill.description = trim(description->second);
  const std::size_t first = body.find_first_not_of("\r\n");
  if (first == std::string::npos) {
    skill.instructions.clear();
  } else {
    const std::size_t last = body.find_last_not_of(" \t\r\n");
    skill.instructions = body.substr(first, last - first + 1);
  }
  skill.path = path;
  return true;
}

}  // namespace

SkillLoad loadSkills(const std::string& directory) {
  SkillLoad result;
  if (directory.empty()) {
    return result;
  }

  std::error_code code;
  if (!std::filesystem::is_directory(directory, code) || code) {
    return result;
  }
  result.directory_found = true;

  std::vector<std::filesystem::path> skillDirectories;
  std::filesystem::directory_iterator entry(directory, code);
  const std::filesystem::directory_iterator end;
  while (!code && entry != end) {
    std::error_code entryCode;
    if (entry->is_directory(entryCode) && !entryCode) {
      skillDirectories.push_back(entry->path());
    }
    entry.increment(code);
  }
  std::sort(skillDirectories.begin(), skillDirectories.end());

  for (const std::filesystem::path& skillDirectory : skillDirectories) {
    const std::filesystem::path file = skillDirectory / kSkillFileName;
    std::error_code fileCode;
    if (!std::filesystem::is_regular_file(file, fileCode) || fileCode) {
      continue;
    }

    Skill skill;
    std::string reason;
    if (parseSkillFile(file.string(), skill, reason)) {
      result.skills.push_back(std::move(skill));
    } else {
      result.skipped.push_back(file.string() + ": " + reason);
    }
  }
  return result;
}

const Skill* findSkill(const std::vector<Skill>& skills, const std::string& name) {
  for (const Skill& skill : skills) {
    if (skill.name == name) {
      return &skill;
    }
  }
  return nullptr;
}

context::Fragment skillIndexFragment(const std::vector<Skill>& skills) {
  context::Fragment fragment;
  if (skills.empty()) {
    return fragment;
  }
  fragment.title = "Available skills";
  fragment.content =
      "The following skills are available. Call the load_skill tool with a skill name before using one; the tool "
      "returns the full instructions.";
  for (const Skill& skill : skills) {
    fragment.content += "\n- " + skill.name + ": " + skill.description;
  }
  return fragment;
}

void registerLoadSkillTool(tools::ToolRegistry& registry, std::shared_ptr<const std::vector<Skill>> skills) {
  if (!skills || skills->empty()) {
    return;
  }

  tools::ToolDefinition definition;
  definition.name = "load_skill";
  definition.description = "Read the full instructions for an available skill.";
  definition.parameters = tools::stringParameters({{"name", "Skill name from the available skills list."}}, {"name"});
  registry.registerTool(definition, [skills](const std::string& arguments) {
    json::Value args;
    if (!tools::parseToolArguments(arguments, args)) {
      return tools::ToolResult{false, "load_skill requires a JSON object"};
    }
    std::string name;
    if (!tools::toolStringArgument(args, "name", name)) {
      return tools::ToolResult{false, "load_skill requires name"};
    }
    const Skill* skill = findSkill(*skills, name);
    if (skill == nullptr) {
      return tools::ToolResult{false, "unknown skill: " + name};
    }
    return tools::ToolResult{true, skill->instructions};
  });
}

}  // namespace skills
}  // namespace bjorne_agent
