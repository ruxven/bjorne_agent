#include "model/ModelClient.h"

#include "http/HttpClient.h"
#include "http/Url.h"
#include "json/Json.h"
#include "log/Log.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace bjorne_agent {
namespace model {

namespace {

std::string escapeJson(const std::string& text) {
  std::string result;
  for (const char c : text) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          const char hex[] = "0123456789abcdef";
          const unsigned int value = static_cast<unsigned char>(c);
          result += "\\u00";
          result += hex[(value >> 4) & 0xf];
          result += hex[value & 0xf];
        } else {
          result += c;
        }
    }
  }
  return result;
}

std::string toolCallJson(const ToolCall& call) {
  std::string json = "{\"id\":\"" + escapeJson(call.id) + "\",\"type\":\"function\",\"function\":{";
  json += "\"name\":\"" + escapeJson(call.name) + "\",\"arguments\":";
  json += call.arguments.empty() ? std::string("\"\"") : ("\"" + escapeJson(call.arguments) + "\"");
  json += "}}";
  return json;
}

std::string messageJson(const ModelMessage& message) {
  std::string json = "{\"role\":\"" + escapeJson(message.role) + "\",\"content\":";
  json += message.content.empty() ? std::string("null") : ("\"" + escapeJson(message.content) + "\"");
  if (!message.tool_call_id.empty()) {
    json += ",\"tool_call_id\":\"" + escapeJson(message.tool_call_id) + "\"";
  }
  if (message.tool_calls.empty()) {
    return json + "}";
  }
  json += ",\"tool_calls\":[";
  for (size_t i = 0; i < message.tool_calls.size(); ++i) {
    if (i > 0) {
      json += ",";
    }
    json += toolCallJson(message.tool_calls[i]);
  }
  return json + "]}";
}

std::string requestBodyJson(const std::string& model, const std::vector<ModelMessage>& messages,
                            const std::string& toolsJson) {
  std::string body = "{\"model\":\"" + escapeJson(model) + "\",\"messages\":[";
  for (size_t i = 0; i < messages.size(); ++i) {
    if (i > 0) {
      body += ",";
    }
    body += messageJson(messages[i]);
  }
  body += "]";
  if (!toolsJson.empty()) {
    body += ",\"tools\":" + toolsJson;
  }
  body += ",\"temperature\":0.7,\"max_tokens\":4096}";
  return body;
}

void parseToolCalls(const json::Value& message, std::vector<ToolCall>& out) {
  const json::Value* toolCalls = message.find("tool_calls");
  if (toolCalls == nullptr || !toolCalls->isArray()) {
    return;
  }
  for (const json::Value& call : toolCalls->array) {
    if (!call.isObject()) {
      continue;
    }
    ToolCall parsed;
    const json::Value* id = call.find("id");
    if (id != nullptr && id->isString()) {
      parsed.id = id->string;
    }
    const json::Value* function = call.find("function");
    if (function != nullptr && function->isObject()) {
      const json::Value* name = function->find("name");
      const json::Value* arguments = function->find("arguments");
      if (name != nullptr && name->isString()) {
        parsed.name = name->string;
      }
      if (arguments != nullptr && arguments->isString()) {
        parsed.arguments = arguments->string;
      }
    }
    if (!parsed.name.empty()) {
      out.push_back(parsed);
    }
  }
}

// Extracts the first choice's message from a chat completions body.
bool parseChatResponse(const std::string& body, ModelResponse& out, std::string& error) {
  json::Value root;
  try {
    root = json::parse(body);
  } catch (const std::exception& e) {
    error = std::string("Model response JSON parse error: ") + e.what();
    return false;
  }
  const json::Value* choices = root.find("choices");
  if (choices == nullptr || !choices->isArray() || choices->array.empty()) {
    error = "Model response missing choices";
    return false;
  }
  const json::Value& choice = choices->array.front();
  const auto messageIt = choice.object.find("message");
  if (messageIt == choice.object.end()) {
    error = "Model response missing message";
    return false;
  }
  const json::Value& message = messageIt->second;
  const json::Value* content = message.find("content");
  if (content != nullptr && content->isString()) {
    out.content = content->string;
  }
  // Reasoning models disagree on the field name: DeepSeek and its clones use
  // reasoning_content, OpenRouter uses reasoning. Both are a plain string.
  const json::Value* reasoning = message.find("reasoning_content");
  if (reasoning == nullptr || !reasoning->isString()) {
    reasoning = message.find("reasoning");
  }
  if (reasoning != nullptr && reasoning->isString()) {
    out.reasoning = reasoning->string;
  }
  parseToolCalls(message, out.tool_calls);
  return true;
}

}  // namespace

ModelClient::ModelClient(ModelClientOptions options)
    : endpoint_(std::move(options.endpoint))
    , model_(std::move(options.model))
    , apiKeyEnv_(std::move(options.api_key_env))
    , transport_(std::move(options.transport))
    , ignoreSelfSigned_(options.ignore_self_signed)
    , hostname_(std::move(options.hostname)) {}

ModelClient::ModelClient(const std::string& endpoint, const std::string& model, const std::string& api_key_env,
                         std::shared_ptr<http::HttpTransport> transport)
    : ModelClient(ModelClientOptions{endpoint, model, api_key_env, std::move(transport), false, ""}) {}

bool ModelClient::respond(const std::vector<ModelMessage>& messages, ModelResponse& out, std::string& error) {
  http::Url url;
  if (!http::parseUrl(endpoint_, url, error)) {
    return false;
  }

  std::string apiKey;
  if (!platform::getEnvironmentVariable(apiKeyEnv_, apiKey)) {
    error = "Missing API key environment variable: " + apiKeyEnv_;
    log::fail(error);
    return false;
  }
  log::detail("API key found in " + apiKeyEnv_);

  std::unique_ptr<http::HttpClient> client;
  if (transport_ != nullptr) {
    client = std::make_unique<http::HttpClient>(transport_);
  } else if (url.tls) {
#if BJORNE_AGENT_HAS_OPENSSL
    http::OpenSslTransport::Options tlsOptions;
    tlsOptions.ignore_self_signed = ignoreSelfSigned_;
    tlsOptions.hostname = hostname_;
    client = http::HttpClient::createOpenSsl(tlsOptions);
#else
    error = "Endpoint " + endpoint_ + " uses https, but this build has no TLS support";
    return false;
#endif
  } else {
    client = http::HttpClient::createPlainTcp();
  }

  http::HttpRequest request;
  request.host = url.host;
  request.port = url.port;
  request.path = url.path + "/chat/completions";
  request.body = requestBodyJson(model_, messages, toolsJson_);
  request.headers = {{ "Authorization", "Bearer " + apiKey }};

  http::HttpResponse response;
  log::progress("Requesting " + url.scheme + "://" + url.host + ":" + url.port + request.path);
  if (!client->request(request, response, error)) {
    log::fail(error);
    return false;
  }
  log::detail("Model endpoint returned status " + std::to_string(response.status) + " with " +
              std::to_string(response.body.size()) + " bytes");
  if (response.status < 200 || response.status >= 300) {
    error = "Model endpoint returned status " + std::to_string(response.status) + ": " + response.body;
    log::fail(error);
    return false;
  }

  ModelResponse result;
  if (!parseChatResponse(response.body, result, error)) {
    log::fail(error);
    return false;
  }
  out = result;
  return true;
}

void ModelClient::setTools(const std::string& tools_json) { toolsJson_ = tools_json; }

}  // namespace model
}  // namespace bjorne_agent
