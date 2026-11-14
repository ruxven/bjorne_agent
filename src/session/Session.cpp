#include "session/Session.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "json/Json.h"
#include "platform/Platform.h"

namespace bjorne_agent {
namespace session {

namespace {

constexpr long long kSessionVersion = 1;
constexpr const char* kSessionExtension = ".json";
constexpr const char* kTemporarySuffix = ".tmp";

long long nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

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

json::Value stringNode(const std::string& text) {
  json::Value value;
  value.type = json::Value::Type::String;
  value.string = text;
  return value;
}

json::Value numberNode(long long number) {
  json::Value value;
  value.type = json::Value::Type::Number;
  value.number = static_cast<double>(number);
  return value;
}

json::Value toolCallJson(const model::ToolCall& call) {
  json::Value node = objectNode();
  node.object["id"] = stringNode(call.id);
  node.object["name"] = stringNode(call.name);
  node.object["arguments"] = stringNode(call.arguments);
  return node;
}

json::Value messageJson(const model::ModelMessage& message) {
  json::Value node = objectNode();
  node.object["role"] = stringNode(message.role);
  node.object["content"] = stringNode(message.content);
  if (!message.tool_call_id.empty()) {
    node.object["tool_call_id"] = stringNode(message.tool_call_id);
  }
  if (!message.tool_calls.empty()) {
    json::Value calls = arrayNode();
    for (const model::ToolCall& call : message.tool_calls) {
      calls.array.push_back(toolCallJson(call));
    }
    node.object["tool_calls"] = std::move(calls);
  }
  return node;
}

std::string toJson(const SessionState& state) {
  json::Value root = objectNode();
  root.object["version"] = numberNode(kSessionVersion);
  root.object["id"] = stringNode(state.id);
  root.object["created"] = numberNode(state.created);
  root.object["updated"] = numberNode(state.updated);
  root.object["task"] = stringNode(state.task);
  root.object["workspace"] = stringNode(state.workspace);
  root.object["model"] = stringNode(state.model);
  root.object["status"] = stringNode(state.status);
  root.object["result"] = stringNode(state.result);
  root.object["error"] = stringNode(state.error);
  root.object["turns"] = numberNode(state.turns);

  json::Value messages = arrayNode();
  for (const model::ModelMessage& message : state.messages) {
    messages.array.push_back(messageJson(message));
  }
  root.object["messages"] = std::move(messages);
  return json::write(root);
}

bool getString(const json::Value& object, const std::string& key, std::string& out) {
  const json::Value* value = object.find(key);
  if (value == nullptr || !value->isString()) {
    return false;
  }
  out = value->string;
  return true;
}

bool getNumber(const json::Value& object, const std::string& key, long long& out) {
  const json::Value* value = object.find(key);
  if (value == nullptr || !value->isNumber()) {
    return false;
  }
  out = static_cast<long long>(value->number);
  return true;
}

bool parseToolCall(const json::Value& node, model::ToolCall& out) {
  if (!node.isObject()) {
    return false;
  }
  getString(node, "id", out.id);
  getString(node, "name", out.name);
  getString(node, "arguments", out.arguments);
  return !out.name.empty();
}

bool parseMessage(const json::Value& node, model::ModelMessage& out) {
  if (!node.isObject()) {
    return false;
  }
  if (!getString(node, "role", out.role) || out.role.empty()) {
    return false;
  }
  getString(node, "content", out.content);
  getString(node, "tool_call_id", out.tool_call_id);
  const json::Value* calls = node.find("tool_calls");
  if (calls != nullptr) {
    if (!calls->isArray()) {
      return false;
    }
    for (const json::Value& call : calls->array) {
      model::ToolCall parsed;
      if (!parseToolCall(call, parsed)) {
        return false;
      }
      out.tool_calls.push_back(std::move(parsed));
    }
  }
  return true;
}

bool fromJson(const std::string& text, SessionState& state, std::string& error) {
  json::Value root;
  try {
    root = json::parse(text);
  } catch (const std::exception& exception) {
    error = std::string("Cannot parse session file: ") + exception.what();
    return false;
  }
  if (!root.isObject()) {
    error = "Session file is not a JSON object";
    return false;
  }

  long long version = kSessionVersion;
  getNumber(root, "version", version);
  if (version != kSessionVersion) {
    error = "Unsupported session version: " + std::to_string(version);
    return false;
  }

  getString(root, "id", state.id);
  getNumber(root, "created", state.created);
  getNumber(root, "updated", state.updated);
  getString(root, "task", state.task);
  getString(root, "workspace", state.workspace);
  getString(root, "model", state.model);
  getString(root, "status", state.status);
  getString(root, "result", state.result);
  getString(root, "error", state.error);
  getNumber(root, "turns", state.turns);
  if (state.status.empty()) {
    state.status = "unknown";
  }

  const json::Value* messages = root.find("messages");
  if (messages != nullptr) {
    if (!messages->isArray()) {
      error = "Session messages must be an array";
      return false;
    }
    for (const json::Value& node : messages->array) {
      model::ModelMessage message;
      if (!parseMessage(node, message)) {
        error = "Malformed message in session file";
        return false;
      }
      state.messages.push_back(std::move(message));
    }
  }
  return true;
}

bool writeAtomic(const std::string& path, const std::string& text, std::string& error) {
  const std::string temporary = path + kTemporarySuffix;
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      error = "Cannot write session file: " + temporary;
      return false;
    }
    file << text;
    file.flush();
    if (!file) {
      file.close();
      std::error_code code;
      std::filesystem::remove(temporary, code);
      error = "Cannot write session file: " + temporary;
      return false;
    }
  }

  if (!platform::replaceFile(temporary, path)) {
    std::error_code code;
    std::filesystem::remove(temporary, code);
    error = "Cannot replace session file: " + path;
    return false;
  }
  return true;
}

std::string oneLineTask(const std::string& task) {
  std::string out;
  out.reserve(task.size());
  for (const char character : task) {
    if (character == '\n' || character == '\r' || character == '\t') {
      out.push_back(' ');
    } else {
      out.push_back(character);
    }
  }
  return out;
}

}  // namespace

bool isValidSessionId(const std::string& id) {
  if (id.empty() || id.size() > 128 || id == "." || id == "..") {
    return false;
  }
  for (const char character : id) {
    const unsigned char value = static_cast<unsigned char>(character);
    if (std::isalnum(value) == 0 && character != '-' && character != '_' && character != '.') {
      return false;
    }
  }
  return true;
}

std::string generateSessionId() {
  const long long milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  static std::atomic<unsigned long long> sequence{0};
  const unsigned long long next = ++sequence;
  return std::to_string(milliseconds) + "-" + std::to_string(platform::processId()) + "-" + std::to_string(next);
}

std::string formatTimestamp(long long epochSeconds) {
  const std::time_t time = static_cast<std::time_t>(epochSeconds);
  std::tm value{};
#if defined(_WIN32)
  if (::gmtime_s(&value, &time) != 0) {
    return {};
  }
#else
  if (::gmtime_r(&time, &value) == nullptr) {
    return {};
  }
#endif
  char buffer[32];
  const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &value);
  if (written == 0) {
    return {};
  }
  return std::string(buffer, written);
}

SessionStore::SessionStore(std::string directory) : directory_(std::move(directory)) {}

std::unique_ptr<SessionStore> SessionStore::open(const std::string& directory, std::string& error) {
  if (directory.empty()) {
    error = "Session directory is empty";
    return nullptr;
  }

  std::error_code code;
  const bool exists = std::filesystem::exists(directory, code);
  if (code) {
    error = "Cannot inspect session directory: " + directory + ": " + code.message();
    return nullptr;
  }
  if (exists) {
    if (!std::filesystem::is_directory(directory, code) || code) {
      error = "Session path is not a directory: " + directory;
      return nullptr;
    }
  } else {
    std::filesystem::create_directories(directory, code);
    if (code) {
      error = "Cannot create session directory: " + directory + ": " + code.message();
      return nullptr;
    }
    if (!std::filesystem::is_directory(directory, code) || code) {
      error = "Session path is not a directory: " + directory;
      return nullptr;
    }
  }
  return std::unique_ptr<SessionStore>(new SessionStore(directory));
}

std::string SessionStore::pathFor(const std::string& id) const {
  return (std::filesystem::path(directory_) / (id + kSessionExtension)).string();
}

bool SessionStore::begin(SessionState& state, std::string& error) {
  state.id = generateSessionId();
  state.created = nowSeconds();
  if (state.status.empty()) {
    state.status = "running";
  }
  return save(state, error);
}

bool SessionStore::save(SessionState& state, std::string& error) {
  if (!isValidSessionId(state.id)) {
    error = "Invalid session id: " + state.id;
    return false;
  }
  state.updated = nowSeconds();
  return writeAtomic(pathFor(state.id), toJson(state), error);
}

bool SessionStore::load(const std::string& id, SessionState& state, std::string& error) const {
  if (!isValidSessionId(id)) {
    error = "Invalid session id: " + id;
    return false;
  }

  std::string text;
  if (!readFile(pathFor(id), text)) {
    error = "Cannot read session: " + id;
    return false;
  }

  state = SessionState();
  if (!fromJson(text, state, error)) {
    return false;
  }
  // The file name is authoritative, so a damaged record cannot point the next
  // save at another session.
  state.id = id;
  return true;
}

std::vector<SessionSummary> SessionStore::list(std::string& error) const {
  std::vector<SessionSummary> summaries;
  error.clear();

  std::error_code code;
  std::filesystem::directory_iterator entry(directory_, code);
  const std::filesystem::directory_iterator end;
  while (!code && entry != end) {
    std::error_code entryCode;
    const bool regular = entry->is_regular_file(entryCode);
    if (!entryCode && regular && entry->path().extension() == kSessionExtension) {
      std::string text;
      if (readFile(entry->path().string(), text)) {
        SessionState state;
        std::string parseError;
        if (fromJson(text, state, parseError) && isValidSessionId(entry->path().stem().string())) {
          SessionSummary summary;
          summary.id = entry->path().stem().string();
          summary.created = state.created;
          summary.updated = state.updated;
          summary.status = state.status;
          summary.task = oneLineTask(state.task);
          summary.messages = state.messages.size();
          summaries.push_back(std::move(summary));
        }
      }
    }
    entry.increment(code);
  }

  if (code) {
    error = "Cannot read session directory: " + directory_ + ": " + code.message();
    return {};
  }

  std::sort(summaries.begin(), summaries.end(), [](const SessionSummary& left, const SessionSummary& right) {
    if (left.updated != right.updated) {
      return left.updated > right.updated;
    }
    return left.id < right.id;
  });
  return summaries;
}

std::string SessionStore::latest(std::string& error) const {
  const std::vector<SessionSummary> summaries = list(error);
  if (!error.empty() || summaries.empty()) {
    return {};
  }
  return summaries.front().id;
}

std::size_t repairDanglingToolCalls(std::vector<model::ModelMessage>& messages) {
  std::size_t repaired = 0;
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (messages[index].role != "assistant" || messages[index].tool_calls.empty()) {
      continue;
    }

    // Tool results for an assistant message sit immediately after it. Stop at
    // the first message of another kind, which is also where a missing result
    // has to be inserted to keep the sequence valid.
    std::size_t end = index + 1;
    std::vector<std::string> answered;
    while (end < messages.size() && messages[end].role == "tool") {
      answered.push_back(messages[end].tool_call_id);
      ++end;
    }

    std::vector<model::ModelMessage> missing;
    for (const model::ToolCall& call : messages[index].tool_calls) {
      if (std::find(answered.begin(), answered.end(), call.id) != answered.end()) {
        continue;
      }
      model::ModelMessage tool;
      tool.role = "tool";
      tool.tool_call_id = call.id;
      tool.content = "Error: the tool call did not finish before the session stopped.";
      missing.push_back(std::move(tool));
      ++repaired;
    }

    if (!missing.empty()) {
      messages.insert(messages.begin() + static_cast<std::ptrdiff_t>(end), missing.begin(), missing.end());
    }
    index = end + missing.size() - 1;
  }
  return repaired;
}

}  // namespace session
}  // namespace bjorne_agent
