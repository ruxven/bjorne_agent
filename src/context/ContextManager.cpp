#include "context/ContextManager.h"

#include <string>
#include <vector>

namespace bjorne_agent {
namespace context {

ContextManager::ContextManager(ContextManagerConfig config, CompressionHandler compression)
    : config_(std::move(config)), compression_(std::move(compression)) {}

const ContextManagerConfig& ContextManager::config() const { return config_; }

CompressionHandler ContextManager::compressionHandler() const { return compression_; }

long long ContextManager::tokenCountOf(const model::ModelMessage& message) const {
  // Approximation: one token per four characters plus one for the message shape.
  return static_cast<long long>(message.content.size()) / 4 + 1;
}

long long ContextManager::estimateTokens(const std::vector<model::ModelMessage>& messages) const {
  long long total = 0;
  for (const model::ModelMessage& message : messages) {
    total += tokenCountOf(message);
  }
  return total;
}

bool ContextManager::trim(const std::vector<model::ModelMessage>& messages, std::vector<model::ModelMessage>& out,
                         std::string& error) const {
  if (config_.context_window <= 0) {
    out = messages;
    return true;
  }
  if (estimateTokens(messages) <= config_.context_window) {
    out = messages;
    return true;
  }
  if (config_.overflow_strategy == ContextManagerConfig::OverflowStrategy::Compress) {
    return compress(messages, out, error);
  }
  return truncate(messages, config_.context_window, out, error);
}

bool ContextManager::truncate(const std::vector<model::ModelMessage>& messages, long long context_window,
                               std::vector<model::ModelMessage>& out, std::string& error) const {
  std::vector<model::ModelMessage> kept = messages;
  size_t firstContent = 0;
  while (firstContent < kept.size() && kept[firstContent].role == "system") {
    ++firstContent;
  }
  std::vector<model::ModelMessage> working;
  if (firstContent < kept.size()) {
    working.assign(kept.begin() + static_cast<std::ptrdiff_t>(firstContent), kept.end());
  }
  long long workingTotal = estimateTokens(working);
  size_t dropIndex = 0;
  while (workingTotal > context_window && dropIndex < working.size()) {
    workingTotal -= tokenCountOf(working[dropIndex]);
    ++dropIndex;
  }
  std::vector<model::ModelMessage> result = working;
  result.erase(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(dropIndex));
  result.insert(result.begin(), kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(firstContent));
  if (estimateTokens(result) > context_window) {
    error = "Context exceeds model context window after truncation";
    return false;
  }
  out = result;
  return true;
}

bool ContextManager::compress(const std::vector<model::ModelMessage>& messages, std::vector<model::ModelMessage>& out,
                              std::string& error) const {
  if (compression_ == nullptr) {
    error = "Compression overflow strategy requires a compression handler";
    return false;
  }
  std::string summary;
  std::string compressError;
  if (!compression_(messages, config_.compression_prompt, summary, compressError)) {
    error = compressError;
    return false;
  }
  if (summary.empty()) {
    error = "Compression returned an empty summary";
    return false;
  }
  std::vector<model::ModelMessage> trimmed;
  for (const model::ModelMessage& message : messages) {
    if (message.role == "system") {
      trimmed.push_back(message);
    }
  }
  model::ModelMessage summaryMessage;
  summaryMessage.role = "user";
  summaryMessage.content = summary;
  trimmed.push_back(summaryMessage);
  out = trimmed;
  return true;
}

}  // namespace context
}  // namespace bjorne_agent
