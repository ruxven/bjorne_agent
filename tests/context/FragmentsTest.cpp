#include "context/Fragments.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

namespace {

class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_fragments_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" +
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

bjorne_agent::context::Fragment fragment(const std::string& title, const std::string& content) {
  bjorne_agent::context::Fragment value;
  value.title = title;
  value.content = content;
  return value;
}

}  // namespace

TEST_CASE("renderFragments returns an empty string for no fragments") {
  REQUIRE(bjorne_agent::context::renderFragments({}).empty());
}

TEST_CASE("renderFragments keeps the caller order") {
  const std::vector<bjorne_agent::context::Fragment> fragments = {fragment("global", "first"),
                                                                  fragment("workspace", "second")};
  const std::string text = bjorne_agent::context::renderFragments(fragments);

  const std::size_t global = text.find("## global");
  const std::size_t workspace = text.find("## workspace");
  REQUIRE(global != std::string::npos);
  REQUIRE(workspace != std::string::npos);
  REQUIRE(global < workspace);
  REQUIRE(text.find("first") < text.find("second"));
}

TEST_CASE("renderFragments skips an empty fragment") {
  const std::string text = bjorne_agent::context::renderFragments({fragment("empty", ""), fragment("kept", "body")});
  REQUIRE(text.find("## empty") == std::string::npos);
  REQUIRE(text.find("## kept") != std::string::npos);
}

TEST_CASE("renderFragments caps the total and marks the cut") {
  bjorne_agent::context::FragmentLimits limits;
  limits.max_bytes = 20;
  const std::string text =
      bjorne_agent::context::renderFragments({fragment("source", "0123456789012345678901234567890")}, limits);

  REQUIRE(text.find("## source") != std::string::npos);
  REQUIRE(text.find("[context truncated]") != std::string::npos);
  REQUIRE(text.size() < 80);
}

TEST_CASE("renderFragments drops fragments after the cap") {
  bjorne_agent::context::FragmentLimits limits;
  limits.max_bytes = 30;
  const std::string text = bjorne_agent::context::renderFragments(
      {fragment("first", "one"), fragment("second", "two two two two two"), fragment("third", "never")}, limits);

  REQUIRE(text.find("## first") != std::string::npos);
  REQUIRE(text.find("## second") != std::string::npos);
  REQUIRE(text.find("## third") == std::string::npos);
  REQUIRE(text.find("never") == std::string::npos);
}

TEST_CASE("renderFragments does not split a UTF-8 character") {
  bjorne_agent::context::FragmentLimits limits;
  // "## t\n\na" is seven bytes, then the two byte character starts. The cut
  // lands on the second byte, so the whole character has to be dropped.
  limits.max_bytes = 8;
  const std::string text = bjorne_agent::context::renderFragments({fragment("t", "a\xC3\xA9")}, limits);

  REQUIRE(text.find('\xC3') == std::string::npos);
  REQUIRE(text.find("[context truncated]") != std::string::npos);
}

TEST_CASE("renderFragments with a zero cap returns nothing") {
  bjorne_agent::context::FragmentLimits limits;
  limits.max_bytes = 0;
  REQUIRE(bjorne_agent::context::renderFragments({fragment("source", "body")}, limits).empty());
}

TEST_CASE("readFragment reads a file and titles it by path") {
  TempDir dir;
  const std::string path = dir.str() + "/AGENTS.md";
  std::ofstream(path) << "contents";

  bjorne_agent::context::Fragment value;
  REQUIRE(bjorne_agent::context::readFragment(path, value));
  REQUIRE(value.title == path);
  REQUIRE(value.content == "contents");
}

TEST_CASE("readFragment reports a missing file") {
  TempDir dir;
  bjorne_agent::context::Fragment value;
  REQUIRE_FALSE(bjorne_agent::context::readFragment(dir.str() + "/missing.md", value));
}
