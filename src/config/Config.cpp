#include "config/Config.h"

#include "json/Json.h"

#include <fstream>
#include <sstream>

namespace bjorne_agent {
namespace config {

namespace {

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

void loadAgent(const json::Value& root, AgentConfig& agent) {
  const json::Value* node = root.find("agent");
  if (node == nullptr || !node->isObject()) {
    return;
  }
  getString(*node, "system_prompt", agent.system_prompt);
  getString(*node, "workspace", agent.workspace);
}

void loadModel(const json::Value& entry, ModelConfig& model) {
  getString(entry, "provider", model.provider);
  getString(entry, "endpoint", model.endpoint);
  getString(entry, "model", model.model);
  getNumber(entry, "context_window", model.context_window);
  getNumber(entry, "max_tokens", model.max_tokens);
  const json::Value* temperature = entry.find("temperature");
  if (temperature != nullptr && temperature->isNumber()) {
    model.temperature = temperature->number;
  }
  getString(entry, "api_key_env", model.api_key_env);
  getNumber(entry, "priority", model.priority);
  getString(entry, "compression_prompt_path", model.compression_prompt_path);
  const json::Value* ignore_self_signed = entry.find("ignore_self_signed");
  if (ignore_self_signed != nullptr && ignore_self_signed->isBoolean()) {
    model.ignore_self_signed = ignore_self_signed->boolean;
  }
  const json::Value* hostname = entry.find("hostname");
  if (hostname != nullptr && hostname->isString()) {
    model.hostname = hostname->string;
  }
}

void loadModels(const json::Value& root, std::vector<ModelConfig>& out) {
  const json::Value* models = root.find("models");
  if (models == nullptr || !models->isArray()) {
    return;
  }
  for (const json::Value& entry : models->array) {
    if (!entry.isObject()) {
      continue;
    }
    ModelConfig model;
    loadModel(entry, model);
    out.push_back(model);
  }
}

}  // namespace

bool load(const std::string& path, Config& out, std::string& error) {
  std::ifstream file(path);
  if (!file.is_open()) {
    error = "Cannot open config file: " + path;
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  json::Value root;
  try {
    root = json::parse(buffer.str());
  } catch (const std::exception& e) {
    error = std::string("JSON parse error: ") + e.what();
    return false;
  }
  if (!root.isObject()) {
    error = "Config root must be a JSON object";
    return false;
  }

  const json::Value* version = root.find("version");
  if (version == nullptr || !version->isNumber()) {
    error = "Config version must be a number";
    return false;
  }
  out.version = static_cast<long long>(version->number);
  if (out.version != 1) {
    error = "Unsupported config version: " + std::to_string(out.version);
    return false;
  }

  loadAgent(root, out.agent);
  loadModels(root, out.models);
  return true;
}

}  // namespace config
}  // namespace bjorne_agent
