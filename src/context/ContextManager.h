#pragma once

#include <functional>
#include <string>
#include <vector>

#include "model/ModelClient.h"

namespace bjorne_agent {
namespace context {

constexpr const char* kDefaultCompressionPrompt = "Summarize the conversation";

struct ContextManagerConfig {
  enum class OverflowStrategy { Truncate, Compress };
  OverflowStrategy overflow_strategy = OverflowStrategy::Truncate;
  long long context_window = 0;
  long long max_tokens = 0;
  std::string compression_prompt = kDefaultCompressionPrompt;
};

// Compresses a conversation by asking the model for a summary. The handler
// owns the model call so tests can inject a fake and the loop can wire the real
// model client without ContextManager knowing about HTTP details.
using CompressionHandler =
    std::function<bool(const std::vector<model::ModelMessage>& messages, const std::string& instruction,
                      std::string& summary, std::string& error)>;

class ContextManager {
 public:
  ContextManager(ContextManagerConfig config, CompressionHandler compression = nullptr);

  const ContextManagerConfig& config() const;
  CompressionHandler compressionHandler() const;

  long long estimateTokens(const std::vector<model::ModelMessage>& messages) const;

  // Applies the configured overflow strategy and reports the context to send.
  // Returns false and fills error when the context cannot fit.
  bool trim(const std::vector<model::ModelMessage>& messages, std::vector<model::ModelMessage>& out, std::string& error) const;

 private:
  ContextManagerConfig config_;
  CompressionHandler compression_;

  long long tokenCountOf(const model::ModelMessage& message) const;
  bool truncate(const std::vector<model::ModelMessage>& messages, long long context_window, std::vector<model::ModelMessage>& out,
                std::string& error) const;
  bool compress(const std::vector<model::ModelMessage>& messages, std::vector<model::ModelMessage>& out, std::string& error) const;
};

}  // namespace context
}  // namespace bjorne_agent
