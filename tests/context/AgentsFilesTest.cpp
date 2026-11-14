#include "context/AgentsFiles.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

namespace {

class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_agents_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" +
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

TEST_CASE("AGENTS.md files are global first and nearest last") {
  TempDir dir;
  const std::string repo = dir.str() + "/repo";
  const std::string workspace = repo + "/sub/deep";
  const std::string global = dir.str() + "/global";
  std::filesystem::create_directories(workspace);
  writeFile(global + "/AGENTS.md", "global instructions");
  writeFile(repo + "/AGENTS.md", "root instructions");
  writeFile(repo + "/sub/AGENTS.md", "sub instructions");

  const bjorne_agent::context::AgentsFiles files = bjorne_agent::context::loadAgentsFiles(workspace, global);

  REQUIRE(files.fragments.size() == 3);
  REQUIRE(files.fragments[0].content == "global instructions");
  REQUIRE(files.fragments[0].title.find("global") != std::string::npos);
  REQUIRE(files.fragments[1].content == "root instructions");
  REQUIRE(files.fragments[1].title.find("repo") != std::string::npos);
  REQUIRE(files.fragments[2].content == "sub instructions");
  REQUIRE(files.fragments[2].title.find("sub") != std::string::npos);
  REQUIRE(files.unreadable.empty());
}

TEST_CASE("AGENTS.md search with no files finds nothing") {
  TempDir dir;
  std::filesystem::create_directories(dir.str() + "/workspace");

  const bjorne_agent::context::AgentsFiles files =
      bjorne_agent::context::loadAgentsFiles(dir.str() + "/workspace", "");

  REQUIRE(files.fragments.empty());
  REQUIRE(files.unreadable.empty());
}

TEST_CASE("AGENTS.md candidates that are directories are ignored") {
  TempDir dir;
  std::filesystem::create_directories(dir.str() + "/workspace/AGENTS.md");

  const bjorne_agent::context::AgentsFiles files =
      bjorne_agent::context::loadAgentsFiles(dir.str() + "/workspace", "");

  REQUIRE(files.fragments.empty());
}

TEST_CASE("a global AGENTS.md inside the workspace is included once") {
  TempDir dir;
  const std::string workspace = dir.str() + "/repo";
  std::filesystem::create_directories(workspace);
  writeFile(workspace + "/AGENTS.md", "only once");

  const bjorne_agent::context::AgentsFiles files = bjorne_agent::context::loadAgentsFiles(workspace, workspace);

  REQUIRE(files.fragments.size() == 1);
  REQUIRE(files.fragments[0].content == "only once");
}

TEST_CASE("the workspace itself is searched before its parents") {
  TempDir dir;
  const std::string parent = dir.str() + "/parent";
  const std::string workspace = parent + "/workspace";
  std::filesystem::create_directories(workspace);
  writeFile(parent + "/AGENTS.md", "parent");
  writeFile(workspace + "/AGENTS.md", "workspace");

  const bjorne_agent::context::AgentsFiles files = bjorne_agent::context::loadAgentsFiles(workspace, "");

  REQUIRE(files.fragments.size() == 2);
  REQUIRE(files.fragments[0].content == "parent");
  REQUIRE(files.fragments[1].content == "workspace");
}
