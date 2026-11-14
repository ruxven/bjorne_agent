#include "skills/Skills.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

#include "tools/Tools.h"

namespace {

class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_skills_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" +
             std::to_string(++counter));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string str() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

void writeFile(const std::string& path, const std::string& content) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream(path) << content;
}

}  // namespace

TEST_CASE("loadSkills reads name, description and instructions") {
  TempDir dir;
  const std::string skills = dir.str() + "/skills";
  writeFile(skills + "/alpha/SKILL.md",
            "---\nname: alpha\ndescription: Alpha skill\n---\nAlpha instructions.\n");
  writeFile(skills + "/beta/SKILL.md",
            "---\r\nname: \"beta\"\r\ndescription: 'Beta skill'\r\n---\r\nLine one.\r\nLine two.\r\n");

  const bjorne_agent::skills::SkillLoad load = bjorne_agent::skills::loadSkills(skills);

  REQUIRE(load.directory_found);
  REQUIRE(load.skipped.empty());
  REQUIRE(load.skills.size() == 2);
  REQUIRE(load.skills[0].name == "alpha");
  REQUIRE(load.skills[0].description == "Alpha skill");
  REQUIRE(load.skills[0].instructions == "Alpha instructions.");
  REQUIRE(load.skills[0].path.find("alpha/SKILL.md") != std::string::npos);
  REQUIRE(load.skills[1].name == "beta");
  REQUIRE(load.skills[1].description == "Beta skill");
  REQUIRE(load.skills[1].instructions == "Line one.\r\nLine two.");
}

TEST_CASE("a missing skills directory is not an error") {
  TempDir dir;
  const bjorne_agent::skills::SkillLoad load = bjorne_agent::skills::loadSkills(dir.str() + "/missing");

  REQUIRE_FALSE(load.directory_found);
  REQUIRE(load.skills.empty());
}

TEST_CASE("an empty skills directory reports that it was found") {
  TempDir dir;
  const std::string skills = dir.str() + "/skills";
  std::filesystem::create_directories(skills);

  const bjorne_agent::skills::SkillLoad load = bjorne_agent::skills::loadSkills(skills);

  REQUIRE(load.directory_found);
  REQUIRE(load.skills.empty());
  REQUIRE(load.skipped.empty());
}

TEST_CASE("a skill without frontmatter is skipped") {
  TempDir dir;
  const std::string skills = dir.str() + "/skills";
  writeFile(skills + "/plain/SKILL.md", "just text");

  const bjorne_agent::skills::SkillLoad load = bjorne_agent::skills::loadSkills(skills);

  REQUIRE(load.skills.empty());
  REQUIRE(load.skipped.size() == 1);
  REQUIRE(load.skipped[0].find("missing frontmatter") != std::string::npos);
}

TEST_CASE("a skill without a description is skipped") {
  TempDir dir;
  const std::string skills = dir.str() + "/skills";
  writeFile(skills + "/alpha/SKILL.md", "---\nname: alpha\n---\nbody");

  const bjorne_agent::skills::SkillLoad load = bjorne_agent::skills::loadSkills(skills);

  REQUIRE(load.skills.empty());
  REQUIRE(load.skipped.size() == 1);
  REQUIRE(load.skipped[0].find("missing description") != std::string::npos);
}

TEST_CASE("a skill without a name is skipped") {
  TempDir dir;
  const std::string skills = dir.str() + "/skills";
  writeFile(skills + "/alpha/SKILL.md", "---\ndescription: Alpha skill\n---\nbody");

  const bjorne_agent::skills::SkillLoad load = bjorne_agent::skills::loadSkills(skills);

  REQUIRE(load.skills.empty());
  REQUIRE(load.skipped.size() == 1);
  REQUIRE(load.skipped[0].find("missing name") != std::string::npos);
}

TEST_CASE("unterminated frontmatter is skipped") {
  TempDir dir;
  const std::string skills = dir.str() + "/skills";
  writeFile(skills + "/alpha/SKILL.md", "---\nname: alpha\ndescription: Alpha skill\nbody without a close");

  const bjorne_agent::skills::SkillLoad load = bjorne_agent::skills::loadSkills(skills);

  REQUIRE(load.skills.empty());
  REQUIRE(load.skipped.size() == 1);
  REQUIRE(load.skipped[0].find("unterminated frontmatter") != std::string::npos);
}

TEST_CASE("files and directories without SKILL.md are ignored") {
  TempDir dir;
  const std::string skills = dir.str() + "/skills";
  std::filesystem::create_directories(skills + "/empty");
  writeFile(skills + "/README.md", "not a skill");

  const bjorne_agent::skills::SkillLoad load = bjorne_agent::skills::loadSkills(skills);

  REQUIRE(load.skills.empty());
  REQUIRE(load.skipped.empty());
}

TEST_CASE("findSkill matches by name") {
  bjorne_agent::skills::Skill alpha;
  alpha.name = "alpha";
  const std::vector<bjorne_agent::skills::Skill> skills = {alpha};

  REQUIRE(bjorne_agent::skills::findSkill(skills, "alpha") == &skills[0]);
  REQUIRE(bjorne_agent::skills::findSkill(skills, "beta") == nullptr);
}

TEST_CASE("the skill index carries names and descriptions but not instructions") {
  bjorne_agent::skills::Skill alpha;
  alpha.name = "alpha";
  alpha.description = "Alpha skill";
  alpha.instructions = "Alpha instructions.";

  const bjorne_agent::context::Fragment fragment = bjorne_agent::skills::skillIndexFragment({alpha});

  REQUIRE(fragment.title == "Available skills");
  REQUIRE(fragment.content.find("load_skill") != std::string::npos);
  REQUIRE(fragment.content.find("- alpha: Alpha skill") != std::string::npos);
  REQUIRE(fragment.content.find("Alpha instructions.") == std::string::npos);
}

TEST_CASE("an empty skill catalog produces no index fragment") {
  const bjorne_agent::context::Fragment fragment = bjorne_agent::skills::skillIndexFragment({});
  REQUIRE(fragment.title.empty());
  REQUIRE(fragment.content.empty());
}

TEST_CASE("load_skill returns the full instructions") {
  bjorne_agent::skills::Skill alpha;
  alpha.name = "alpha";
  alpha.description = "Alpha skill";
  alpha.instructions = "Do the thing.";
  auto catalog = std::make_shared<std::vector<bjorne_agent::skills::Skill>>();
  catalog->push_back(alpha);

  bjorne_agent::tools::ToolRegistry registry("");
  bjorne_agent::skills::registerLoadSkillTool(registry, catalog);

  REQUIRE(registry.hasTool("load_skill"));
  REQUIRE(registry.execute("load_skill", R"({"name":"alpha"})").ok);
  REQUIRE(registry.execute("load_skill", R"({"name":"alpha"})").output == "Do the thing.");
  REQUIRE(registry.execute("load_skill", R"({"name":"beta"})").output == "unknown skill: beta");
  REQUIRE(registry.execute("load_skill", R"({"other":"alpha"})").output == "load_skill requires name");
  REQUIRE(registry.execute("load_skill", "not json").output == "load_skill requires a JSON object");
}

TEST_CASE("the load_skill tool is not registered for an empty catalog") {
  bjorne_agent::tools::ToolRegistry registry("");
  bjorne_agent::skills::registerLoadSkillTool(registry, nullptr);
  REQUIRE_FALSE(registry.hasTool("load_skill"));

  auto empty = std::make_shared<std::vector<bjorne_agent::skills::Skill>>();
  bjorne_agent::skills::registerLoadSkillTool(registry, empty);
  REQUIRE_FALSE(registry.hasTool("load_skill"));
}
