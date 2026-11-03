#include "tools/Tools.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <catch_amalgamated.hpp>

#include "json/Json.h"

namespace {

class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    // The path carries the process id so that two test binaries running at the
    // same time do not overwrite each other's fixture.
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_tools_test_" + std::to_string(::getpid()) + "_" + std::to_string(++counter));
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

}  // namespace

TEST_CASE("read_file returns file content") {
  TempDir dir;
  std::ofstream(dir.str() + "/hello.txt") << "hello world";

  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult result = registry.execute("read_file", R"({"path":"hello.txt"})");
  REQUIRE(result.ok);
  REQUIRE(result.output == "hello world");
}

TEST_CASE("write_file creates nested paths") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult write = registry.execute("write_file", R"({"path":"nested/note.txt","content":"written"})");
  REQUIRE(write.ok);

  const bjorne_agent::tools::ToolResult read = registry.execute("read_file", R"({"path":"nested/note.txt"})");
  REQUIRE(read.ok);
  REQUIRE(read.output == "written");
}

TEST_CASE("edit_file replaces the first occurrence") {
  TempDir dir;
  std::ofstream(dir.str() + "/edit.txt") << "alpha beta alpha";

  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult edit = registry.execute("edit_file", R"({"path":"edit.txt","old":"alpha","new":"gamma"})");
  REQUIRE(edit.ok);

  const bjorne_agent::tools::ToolResult read = registry.execute("read_file", R"({"path":"edit.txt"})");
  REQUIRE(read.ok);
  REQUIRE(read.output == "gamma beta alpha");
}

TEST_CASE("edit_file reports missing text") {
  TempDir dir;
  std::ofstream(dir.str() + "/edit.txt") << "alpha beta";

  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult edit = registry.execute("edit_file", R"({"path":"edit.txt","old":"missing","new":"gamma"})");
  REQUIRE_FALSE(edit.ok);
  REQUIRE(edit.output == "text not found in edit.txt");
}

TEST_CASE("run_shell captures command output in the workspace") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult result = registry.execute("run_shell", R"({"command":"pwd && printf hello"})");
  REQUIRE(result.ok);
  REQUIRE(result.output.find(dir.str()) != std::string::npos);
  REQUIRE(result.output.find("hello") != std::string::npos);
}

TEST_CASE("run_shell reports a non-zero exit code") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult result = registry.execute("run_shell", R"({"command":"exit 3"})");
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.output.find("exit code 3") != std::string::npos);
}

TEST_CASE("unknown tool reports an error") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult result = registry.execute("missing_tool", "{}");
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.output == "unknown tool: missing_tool");
}

TEST_CASE("read_file rejects parent traversal") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult result = registry.execute("read_file", R"({"path":"../secret.txt"})");
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.output.find("parent") != std::string::npos);
}

TEST_CASE("malformed arguments report an error") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult result = registry.execute("read_file", "not json");
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.output == "read_file requires a JSON object");
}

TEST_CASE("run_shell quotes a workspace path containing a single quote") {
  TempDir dir;
  const std::string quoted = dir.str() + "/quote'dir";
  std::filesystem::create_directories(quoted);
  bjorne_agent::tools::ToolRegistry registry(quoted);

  const bjorne_agent::tools::ToolResult result = registry.execute("run_shell", R"({"command":"pwd"})");
  REQUIRE(result.ok);
  REQUIRE(result.output.find("quote'dir") != std::string::npos);
}

TEST_CASE("run_shell truncates oversized output") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());
  const bjorne_agent::tools::ToolResult result = registry.execute("run_shell", R"({"command":"yes x | head -c 300000"})");
  REQUIRE(result.ok);
  REQUIRE(result.output.size() > 256 * 1024);
  REQUIRE(result.output.find("[output truncated]") != std::string::npos);
}

TEST_CASE("tools report missing arguments") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());

  REQUIRE(registry.execute("read_file", R"({"other":1})").output == "read_file requires path");
  REQUIRE(registry.execute("read_file", R"({"path":""})").output == "path is required");

  REQUIRE(registry.execute("write_file", R"({"content":"x"})").output == "write_file requires path");
  REQUIRE(registry.execute("write_file", R"({"path":"x.txt"})").output == "write_file requires content");
  REQUIRE(registry.execute("write_file", R"({"path":"","content":"x"})").output == "path is required");

  REQUIRE(registry.execute("edit_file", R"({"old":"a","new":"b"})").output == "edit_file requires path");
  REQUIRE(registry.execute("edit_file", R"({"path":"x.txt","new":"b"})").output == "edit_file requires old");
  REQUIRE(registry.execute("edit_file", R"({"path":"x.txt","old":"a"})").output == "edit_file requires new");
  REQUIRE(registry.execute("edit_file", R"({"path":"","old":"a","new":"b"})").output == "path is required");

  REQUIRE(registry.execute("run_shell", R"({"other":1})").output == "run_shell requires command");
}

TEST_CASE("tools report malformed argument documents") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());

  REQUIRE(registry.execute("write_file", "nope").output == "write_file requires a JSON object");
  REQUIRE(registry.execute("edit_file", "nope").output == "edit_file requires a JSON object");
  REQUIRE(registry.execute("run_shell", "nope").output == "run_shell requires a JSON object");
}

TEST_CASE("tools report filesystem errors") {
  TempDir dir;
  bjorne_agent::tools::ToolRegistry registry(dir.str());

  REQUIRE(registry.execute("read_file", R"({"path":"missing.txt"})").output == "cannot open file: missing.txt");
  REQUIRE(registry.execute("edit_file", R"({"path":"missing.txt","old":"a","new":"b"})").output ==
          "cannot open file: missing.txt");

  std::filesystem::create_directories(dir.str() + "/folder");
  REQUIRE(registry.execute("write_file", R"({"path":"folder","content":"x"})").output == "cannot write file: folder");

  std::ofstream(dir.str() + "/readonly.txt") << "alpha";
  std::filesystem::permissions(dir.str() + "/readonly.txt", std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace);
  const bjorne_agent::tools::ToolResult edit =
      registry.execute("edit_file", R"({"path":"readonly.txt","old":"alpha","new":"beta"})");
  REQUIRE_FALSE(edit.ok);
  REQUIRE(edit.output == "cannot write file: readonly.txt");
}

TEST_CASE("tool definitions are valid JSON with the four tools") {
  const std::string definitions = bjorne_agent::tools::ToolRegistry::definitionsJson();
  const bjorne_agent::json::Value root = bjorne_agent::json::parse(definitions);
  REQUIRE(root.isArray());
  REQUIRE(root.array.size() == 4);

  std::vector<std::string> names;
  for (const bjorne_agent::json::Value& entry : root.array) {
    const bjorne_agent::json::Value* function = entry.find("function");
    REQUIRE(function != nullptr);
    const bjorne_agent::json::Value* name = function->find("name");
    REQUIRE(name != nullptr);
    REQUIRE(name->isString());
    names.push_back(name->string);
  }
  REQUIRE(names == std::vector<std::string>{"read_file", "write_file", "edit_file", "run_shell"});
}
