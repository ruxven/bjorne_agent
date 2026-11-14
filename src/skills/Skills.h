#pragma once

#include <memory>
#include <string>
#include <vector>

#include "context/Fragments.h"
#include "tools/Tools.h"

namespace bjorne_agent {
namespace skills {

// One skill read from <directory>/<name>/SKILL.md. The YAML frontmatter
// supplies the name and description; the rest of the file is the instructions
// that the load_skill tool returns on demand.
struct Skill {
  std::string name;
  std::string description;
  std::string instructions;
  std::string path;
};

struct SkillLoad {
  std::vector<Skill> skills;
  // Files that look like skills but were skipped, as "path: reason".
  std::vector<std::string> skipped;
  bool directory_found = false;
};

// Loads every <directory>/<name>/SKILL.md. Entries are ordered by directory
// name so the prompt is stable from run to run. A missing directory yields no
// skills rather than an error.
SkillLoad loadSkills(const std::string& directory);

const Skill* findSkill(const std::vector<Skill>& skills, const std::string& name);

// The system prompt section that tells the model which skills exist. Only the
// name and description appear; the instructions stay behind load_skill.
context::Fragment skillIndexFragment(const std::vector<Skill>& skills);

// Registers load_skill, which returns a skill's full instructions. Call only
// when the catalog is non-empty, so the model never sees a tool with nothing
// to load.
void registerLoadSkillTool(tools::ToolRegistry& registry, std::shared_ptr<const std::vector<Skill>> skills);

}  // namespace skills
}  // namespace bjorne_agent
