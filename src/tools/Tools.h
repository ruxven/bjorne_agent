#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "json/Json.h"

namespace bjorne_agent {
namespace tools {

struct ToolResult {
  bool ok = false;
  std::string output;
};

// The JSON schema a tool presents to the model. The parameters value is a JSON
// schema object, so a tool can describe any argument shape without a code
// change in the schema builder.
struct ToolDefinition {
  std::string name;
  std::string description;
  json::Value parameters;
};

using ToolHandler = std::function<ToolResult(const std::string& arguments)>;

// Tool calls arrive as JSON arguments and results feed back into the agent
// context. The registry starts with the built-in file and shell tools. A
// caller may register more tools, replace a built-in by name, or remove one;
// the definitions handed to the model are assembled from whatever is
// registered.
class ToolRegistry {
 public:
  explicit ToolRegistry(std::string workspace);
  ToolRegistry(const ToolRegistry&) = delete;
  ToolRegistry& operator=(const ToolRegistry&) = delete;

  // Adds a tool, or replaces the tool of the same name. The built-ins are
  // registered first, so a later registration wins over the built-in.
  void registerTool(ToolDefinition definition, ToolHandler handler);

  // Removes a tool by name. Returns false when no tool has that name.
  bool removeTool(const std::string& name);

  bool hasTool(const std::string& name) const;

  // Registered tool names, sorted by name.
  std::vector<std::string> toolNames() const;

  ToolResult execute(const std::string& name, const std::string& arguments) const;

  // OpenAI compatible tool schema for the model request.
  std::string definitionsJson() const;

 private:
  struct Entry {
    ToolDefinition definition;
    ToolHandler handler;
  };

  void registerBuiltins();

  std::string resolvePath(const std::string& path, std::string& error) const;

  ToolResult readFile(const std::string& arguments) const;
  ToolResult writeFile(const std::string& arguments) const;
  ToolResult editFile(const std::string& arguments) const;
  ToolResult runShell(const std::string& arguments) const;

  std::map<std::string, Entry> tools_;
  std::string workspace_;
};

}  // namespace tools
}  // namespace bjorne_agent
