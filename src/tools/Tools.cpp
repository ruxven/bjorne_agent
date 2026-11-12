#include "tools/Tools.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "json/Json.h"
#include "log/Log.h"

namespace bjorne_agent {
namespace tools {

namespace {

constexpr size_t kMaxShellOutput = 256 * 1024;

bool parseArguments(const std::string& arguments, json::Value& out) {
  try {
    out = json::parse(arguments.empty() ? "{}" : arguments);
  } catch (const std::exception&) {
    return false;
  }
  return out.isObject();
}

bool argString(const json::Value& object, const std::string& key, std::string& out) {
  const json::Value* value = object.find(key);
  if (value == nullptr || !value->isString()) {
    return false;
  }
  out = value->string;
  return true;
}

// Windows accepts either separator; POSIX has only the forward slash, and a
// backslash is an ordinary character in a POSIX file name.
bool isPathSeparator(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

size_t nextPathSeparator(const std::string& path, size_t start) {
  for (size_t i = start; i < path.size(); ++i) {
    if (isPathSeparator(path[i])) {
      return i;
    }
  }
  return std::string::npos;
}

// True for a path that starts at the root: /dir on POSIX, C:\dir or \\server
// on Windows.
bool isAbsolutePath(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (isPathSeparator(path.front())) {
    return true;
  }
#if defined(_WIN32)
  if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':' &&
      isPathSeparator(path[2])) {
    return true;
  }
#endif
  return false;
}

bool hasParentTraversal(const std::string& path) {
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = nextPathSeparator(path, start);
    const size_t stop = end == std::string::npos ? path.size() : end;
    if (path.substr(start, stop - start) == "..") {
      return true;
    }
    start = stop + 1;
  }
  return false;
}

std::string shellQuote(const std::string& text) {
#if defined(_WIN32)
  // cmd.exe does not understand single quotes, so the workspace is wrapped in
  // double quotes. An embedded quote is doubled.
  std::string quoted = "\"";
  for (const char c : text) {
    if (c == '"') {
      quoted += "\"\"";
    } else {
      quoted += c;
    }
  }
  quoted += '"';
  return quoted;
#else
  std::string quoted = "'";
  for (const char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
#endif
}

// Runs the command through the platform shell and returns its exit code, or -1
// when the status could not be determined. POSIX pclose reports a wait status
// that has to be decoded; the Windows _pclose already reports the exit code.
int shellExitCode(FILE* pipe) {
#if defined(_WIN32)
  const int status = ::_pclose(pipe);
  return status == -1 ? -1 : status;
#else
  const int status = ::pclose(pipe);
  if (status == -1) {
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

FILE* openShellPipe(const std::string& command) {
#if defined(_WIN32)
  return ::_popen(command.c_str(), "r");
#else
  return ::popen(command.c_str(), "r");
#endif
}

json::Value stringNode(const std::string& text) {
  json::Value value;
  value.type = json::Value::Type::String;
  value.string = text;
  return value;
}

json::Value objectNode() {
  json::Value value;
  value.type = json::Value::Type::Object;
  return value;
}

json::Value arrayNode() {
  json::Value value;
  value.type = json::Value::Type::Array;
  return value;
}

// Builds the JSON schema for a tool whose arguments are all strings.
json::Value parameterSchema(const std::vector<std::pair<std::string, std::string>>& properties,
                            const std::vector<std::string>& required) {
  json::Value propertyNodes = objectNode();
  for (const auto& property : properties) {
    json::Value spec = objectNode();
    spec.object["type"] = stringNode("string");
    spec.object["description"] = stringNode(property.second);
    propertyNodes.object[property.first] = std::move(spec);
  }
  json::Value requiredNodes = arrayNode();
  for (const std::string& name : required) {
    requiredNodes.array.push_back(stringNode(name));
  }
  json::Value parameters = objectNode();
  parameters.object["type"] = stringNode("object");
  parameters.object["properties"] = std::move(propertyNodes);
  parameters.object["required"] = std::move(requiredNodes);
  return parameters;
}

ToolDefinition readFileDefinition() {
  return {"read_file", "Read a file from the workspace.", parameterSchema({{"path", "File path."}}, {"path"})};
}

ToolDefinition writeFileDefinition() {
  return {"write_file",
          "Write a file in the workspace.",
          parameterSchema({{"path", "File path."}, {"content", "File content."}}, {"path", "content"})};
}

ToolDefinition editFileDefinition() {
  return {"edit_file",
          "Replace the first occurrence of text in a file.",
          parameterSchema({{"path", "File path."}, {"old", "Text to replace."}, {"new", "Replacement text."}},
                          {"path", "old", "new"})};
}

ToolDefinition runShellDefinition() {
  return {"run_shell", "Run a shell command in the workspace.", parameterSchema({{"command", "Command to run."}}, {"command"})};
}

}  // namespace

ToolRegistry::ToolRegistry(std::string workspace) : workspace_(std::move(workspace)) { registerBuiltins(); }

void ToolRegistry::registerBuiltins() {
  registerTool(readFileDefinition(), [this](const std::string& arguments) { return readFile(arguments); });
  registerTool(writeFileDefinition(), [this](const std::string& arguments) { return writeFile(arguments); });
  registerTool(editFileDefinition(), [this](const std::string& arguments) { return editFile(arguments); });
  registerTool(runShellDefinition(), [this](const std::string& arguments) { return runShell(arguments); });
}

void ToolRegistry::registerTool(ToolDefinition definition, ToolHandler handler) {
  if (definition.name.empty() || !handler) {
    return;
  }
  Entry entry;
  entry.definition = std::move(definition);
  entry.handler = std::move(handler);
  const std::string name = entry.definition.name;
  tools_[name] = std::move(entry);
}

bool ToolRegistry::removeTool(const std::string& name) { return tools_.erase(name) > 0; }

bool ToolRegistry::hasTool(const std::string& name) const { return tools_.contains(name); }

std::vector<std::string> ToolRegistry::toolNames() const {
  std::vector<std::string> names;
  names.reserve(tools_.size());
  for (const auto& entry : tools_) {
    names.push_back(entry.first);
  }
  return names;
}

ToolResult ToolRegistry::execute(const std::string& name, const std::string& arguments) const {
  log::progress("Running tool: " + name);
  // The arguments say what the tool is about to do: the command for run_shell,
  // the path and content for a write. Showing them before the call runs is what
  // lets a user follow along instead of seeing only the tool name.
  log::block("arguments", arguments, 1000);
  const auto it = tools_.find(name);
  const ToolResult result =
      it == tools_.end() ? ToolResult{false, "unknown tool: " + name} : it->second.handler(arguments);
  if (result.ok) {
    log::detail("Tool " + name + " returned " + std::to_string(result.output.size()) + " bytes");
    log::block("result", result.output, 2000);
  } else {
    log::fail("Tool " + name + " failed: " + result.output);
  }
  return result;
}

std::string ToolRegistry::resolvePath(const std::string& path, std::string& error) const {
  if (path.empty()) {
    error = "path is required";
    return {};
  }
  if (hasParentTraversal(path)) {
    error = "path must not traverse parent directories";
    return {};
  }
  if (isAbsolutePath(path) || workspace_.empty()) {
    return path;
  }
  std::string resolved = workspace_;
  if (!isPathSeparator(resolved.back())) {
    resolved += '/';
  }
  resolved += path;
  return resolved;
}

ToolResult ToolRegistry::readFile(const std::string& arguments) const {
  json::Value args;
  if (!parseArguments(arguments, args)) {
    return {false, "read_file requires a JSON object"};
  }
  std::string path;
  if (!argString(args, "path", path)) {
    return {false, "read_file requires path"};
  }
  std::string error;
  const std::string resolved = resolvePath(path, error);
  if (resolved.empty()) {
    return {false, error};
  }
  std::ifstream file(resolved);
  if (!file.is_open()) {
    return {false, "cannot open file: " + path};
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return {true, buffer.str()};
}

ToolResult ToolRegistry::writeFile(const std::string& arguments) const {
  json::Value args;
  if (!parseArguments(arguments, args)) {
    return {false, "write_file requires a JSON object"};
  }
  std::string path;
  std::string content;
  if (!argString(args, "path", path)) {
    return {false, "write_file requires path"};
  }
  if (!argString(args, "content", content)) {
    return {false, "write_file requires content"};
  }
  std::string error;
  const std::string resolved = resolvePath(path, error);
  if (resolved.empty()) {
    return {false, error};
  }
  const std::filesystem::path target(resolved);
  if (target.has_parent_path()) {
    std::error_code code;
    std::filesystem::create_directories(target.parent_path(), code);
  }
  std::ofstream file(resolved);
  if (!file.is_open()) {
    return {false, "cannot write file: " + path};
  }
  file << content;
  if (!file) {
    return {false, "write failed: " + path};
  }
  return {true, "wrote " + std::to_string(content.size()) + " bytes to " + path};
}

ToolResult ToolRegistry::editFile(const std::string& arguments) const {
  json::Value args;
  if (!parseArguments(arguments, args)) {
    return {false, "edit_file requires a JSON object"};
  }
  std::string path;
  std::string oldText;
  std::string newText;
  if (!argString(args, "path", path)) {
    return {false, "edit_file requires path"};
  }
  if (!argString(args, "old", oldText)) {
    return {false, "edit_file requires old"};
  }
  if (!argString(args, "new", newText)) {
    return {false, "edit_file requires new"};
  }
  std::string error;
  const std::string resolved = resolvePath(path, error);
  if (resolved.empty()) {
    return {false, error};
  }
  std::ifstream input(resolved);
  if (!input.is_open()) {
    return {false, "cannot open file: " + path};
  }
  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string content = buffer.str();
  input.close();
  const size_t position = content.find(oldText);
  if (position == std::string::npos) {
    return {false, "text not found in " + path};
  }
  content.replace(position, oldText.size(), newText);
  std::ofstream output(resolved);
  if (!output.is_open()) {
    return {false, "cannot write file: " + path};
  }
  output << content;
  if (!output) {
    return {false, "write failed: " + path};
  }
  return {true, "edited " + path};
}

ToolResult ToolRegistry::runShell(const std::string& arguments) const {
  json::Value args;
  if (!parseArguments(arguments, args)) {
    return {false, "run_shell requires a JSON object"};
  }
  std::string command;
  if (!argString(args, "command", command)) {
    return {false, "run_shell requires command"};
  }
  std::string full = command;
  if (!workspace_.empty()) {
#if defined(_WIN32)
    // cmd.exe keeps a current directory per drive, so a plain cd to a path on
    // another drive leaves the process on the old drive. /d switches both.
    full = "cd /d " + shellQuote(workspace_) + " && " + command;
#else
    full = "cd " + shellQuote(workspace_) + " && " + command;
#endif
  }
  full += " 2>&1";
  FILE* pipe = openShellPipe(full);
  if (pipe == nullptr) {
    return {false, "cannot start shell command"};
  }
  std::string output;
  bool truncated = false;
  char buffer[4096];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    if (output.size() < kMaxShellOutput) {
      const size_t room = kMaxShellOutput - output.size();
      output.append(buffer, std::min(read, room));
      if (read > room) {
        truncated = true;
      }
    } else {
      truncated = true;
    }
  }
  const int code = shellExitCode(pipe);
  if (truncated) {
    output += "\n[output truncated]";
  }
  if (code == -1) {
    return {false, output + "\nshell status unavailable"};
  }
  if (code == 0) {
    return {true, output};
  }
  return {false, output + "\nexit code " + std::to_string(code)};
}

std::string ToolRegistry::definitionsJson() const {
  json::Value definitions = arrayNode();
  for (const auto& [name, entry] : tools_) {
    json::Value function = objectNode();
    function.object["name"] = stringNode(name);
    function.object["description"] = stringNode(entry.definition.description);
    function.object["parameters"] = entry.definition.parameters;

    json::Value definition = objectNode();
    definition.object["type"] = stringNode("function");
    definition.object["function"] = std::move(function);
    definitions.array.push_back(std::move(definition));
  }
  return json::write(definitions);
}

}  // namespace tools
}  // namespace bjorne_agent
