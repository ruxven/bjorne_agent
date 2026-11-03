#include "tools/Tools.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <sys/wait.h>

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

bool hasParentTraversal(const std::string& path) {
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = path.find('/', start);
    const size_t stop = end == std::string::npos ? path.size() : end;
    if (path.substr(start, stop - start) == "..") {
      return true;
    }
    start = stop + 1;
  }
  return false;
}

std::string shellQuote(const std::string& text) {
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
}

}  // namespace

ToolRegistry::ToolRegistry(std::string workspace) : workspace_(std::move(workspace)) {}

ToolResult ToolRegistry::execute(const std::string& name, const std::string& arguments) const {
  log::progress("Running tool: " + name);
  const ToolResult result = dispatch(name, arguments);
  if (result.ok) {
    log::detail("Tool " + name + " returned " + std::to_string(result.output.size()) + " bytes");
  } else {
    log::fail("Tool " + name + " failed: " + result.output);
  }
  return result;
}

ToolResult ToolRegistry::dispatch(const std::string& name, const std::string& arguments) const {
  if (name == "read_file") {
    return readFile(arguments);
  }
  if (name == "write_file") {
    return writeFile(arguments);
  }
  if (name == "edit_file") {
    return editFile(arguments);
  }
  if (name == "run_shell") {
    return runShell(arguments);
  }
  return {false, "unknown tool: " + name};
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
  if (path.front() == '/' || workspace_.empty()) {
    return path;
  }
  std::string resolved = workspace_;
  if (resolved.back() != '/') {
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
    full = "cd " + shellQuote(workspace_) + " && " + command;
  }
  full += " 2>&1";
  FILE* pipe = popen(full.c_str(), "r");
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
  const int status = pclose(pipe);
  if (truncated) {
    output += "\n[output truncated]";
  }
  if (status == -1) {
    return {false, output + "\nshell status unavailable"};
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return {true, output};
  }
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {false, output + "\nexit code " + std::to_string(code)};
}

std::string ToolRegistry::definitionsJson() {
  return R"json([
    {"type":"function","function":{"name":"read_file","description":"Read a file from the workspace.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path."}},"required":["path"]}}},
    {"type":"function","function":{"name":"write_file","description":"Write a file in the workspace.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path."},"content":{"type":"string","description":"File content."}},"required":["path","content"]}}},
    {"type":"function","function":{"name":"edit_file","description":"Replace the first occurrence of text in a file.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path."},"old":{"type":"string","description":"Text to replace."},"new":{"type":"string","description":"Replacement text."}},"required":["path","old","new"]}}},
    {"type":"function","function":{"name":"run_shell","description":"Run a shell command in the workspace.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"Command to run."}},"required":["command"]}}}
  ])json";
}

}  // namespace tools
}  // namespace bjorne_agent
