#pragma once

#include <string>

namespace bjorne_agent {
namespace tools {

struct ToolResult {
  bool ok = false;
  std::string output;
};

// Fixed tool set for version one: read file, write file, edit file, run shell
// command. Tool calls arrive as JSON arguments and results feed back into the
// agent context.
class ToolRegistry {
 public:
  explicit ToolRegistry(std::string workspace);

  ToolResult execute(const std::string& name, const std::string& arguments) const;

  // OpenAI compatible tool schema for the model request.
  static std::string definitionsJson();

 private:
  ToolResult dispatch(const std::string& name, const std::string& arguments) const;

  std::string resolvePath(const std::string& path, std::string& error) const;

  ToolResult readFile(const std::string& arguments) const;
  ToolResult writeFile(const std::string& arguments) const;
  ToolResult editFile(const std::string& arguments) const;
  ToolResult runShell(const std::string& arguments) const;

  std::string workspace_;
};

}  // namespace tools
}  // namespace bjorne_agent
