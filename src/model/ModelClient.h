#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "http/HttpClient.h"

namespace bjorne_agent {
namespace model {

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;
};

struct ModelMessage {
  std::string role;
  std::string content;
  std::vector<ToolCall> tool_calls;
  std::string tool_call_id;
};

struct ModelResponse {
  std::string content;
  std::vector<ToolCall> tool_calls;
  // Separate reasoning trace some models return alongside the answer. The field
  // is last so that existing aggregate initializers keep their meaning.
  std::string reasoning;
};

// Seam for the agent loop. Real deployments use ModelClient. Tests substitute
// a scripted backend to cover the loop without HTTP.
class ModelBackend {
 public:
  virtual ~ModelBackend() = default;
  virtual bool respond(const std::vector<ModelMessage>& messages, ModelResponse& out, std::string& error) = 0;

  // The tool schema for the next request. An ACP session changes its tool set
  // while the client stays connected, so the loop's owner may replace it. The
  // default ignores the call, which keeps a scripted test backend simple.
  virtual void setTools(const std::string& tools_json) { (void)tools_json; }
};

// Everything the client needs to reach one endpoint. Grouping the fields keeps
// the constructor short and leaves room for options added later.
struct ModelClientOptions {
  std::string endpoint;
  std::string model;
  std::string api_key_env;
  std::shared_ptr<http::HttpTransport> transport;
  bool ignore_self_signed = false;
  std::string hostname;
};

class ModelClient final : public ModelBackend {
 public:
  explicit ModelClient(ModelClientOptions options);

  // Convenience overload for the common case: one endpoint, one model, one
  // key environment variable, and an optional transport for tests.
  ModelClient(const std::string& endpoint, const std::string& model, const std::string& api_key_env,
              std::shared_ptr<http::HttpTransport> transport = nullptr);

  void setTools(const std::string& tools_json) override;

  bool respond(const std::vector<ModelMessage>& messages, ModelResponse& out, std::string& error) override;

 private:
  std::string endpoint_;
  std::string model_;
  std::string apiKeyEnv_;
  std::string toolsJson_;
  std::shared_ptr<http::HttpTransport> transport_;
  // Only a build with TLS reads this, so a no TLS build would flag it as an
  // unused field.
  [[maybe_unused]] bool ignoreSelfSigned_ = false;
  std::string hostname_;
};

}  // namespace model
}  // namespace bjorne_agent
