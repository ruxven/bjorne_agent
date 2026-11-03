#include "context/ContextManager.h"

#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

namespace {

using MessageList = std::vector<bjorne_agent::model::ModelMessage>;

void add(MessageList& out, const std::string& role, const std::string& content) {
  bjorne_agent::model::ModelMessage message;
  message.role = role;
  message.content = content;
  out.push_back(message);
}

bjorne_agent::context::ContextManagerConfig makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy strategy, long long context_window) {
  bjorne_agent::context::ContextManagerConfig config;
  config.overflow_strategy = strategy;
  config.context_window = context_window;
  return config;
}

bjorne_agent::context::ContextManager makeManager(bjorne_agent::context::ContextManagerConfig config,
                                            bjorne_agent::context::CompressionHandler compression = bjorne_agent::context::CompressionHandler()) {
  return bjorne_agent::context::ContextManager(std::move(config), std::move(compression));
}

}  // namespace

TEST_CASE("Context manager passes context through when it fits") {
  bjorne_agent::context::ContextManager manager = makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Truncate, 100));

  MessageList messages;
  add(messages, "system", "sys");
  add(messages, "user", "hello");
  std::vector<bjorne_agent::model::ModelMessage> out;
  std::string error;
  REQUIRE(manager.trim(messages, out, error));
  REQUIRE(error.empty());
  REQUIRE(out.size() == messages.size());
  REQUIRE(out[0].role == "system");
  REQUIRE(out[0].content == "sys");
}

TEST_CASE("Context manager passes context through when context window is unset") {
  bjorne_agent::context::ContextManager manager =
      makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Truncate, 0));

  MessageList messages;
  add(messages, "user", "hello world");
  std::vector<bjorne_agent::model::ModelMessage> out;
  std::string error;
  REQUIRE(manager.trim(messages, out, error));
  REQUIRE(out.size() == 1);
}

TEST_CASE("Context manager truncates oldest messages when over context window") {
  // system -> 1 token, user "hello world" -> 3 tokens, assistant "hi" -> 1 token, user "bye" -> 1 token. Total 6.
  // Window 4 forces one drop of the oldest conversation message.
  bjorne_agent::context::ContextManager manager = makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Truncate, 4));

  MessageList messages;
  add(messages, "system", "sys");
  add(messages, "user", "hello world");
  add(messages, "assistant", "hi");
  add(messages, "user", "bye");
  std::vector<bjorne_agent::model::ModelMessage> out;
  std::string error;
  REQUIRE(manager.trim(messages, out, error));
  REQUIRE(error.empty());
  REQUIRE(out.size() == 3);
  REQUIRE(out[0].role == "system");
  REQUIRE(out[0].content == "sys");
  REQUIRE(out[1].role == "assistant");
  REQUIRE(out[1].content == "hi");
  REQUIRE(out[2].role == "user");
  REQUIRE(out[2].content == "bye");
}

TEST_CASE("Context manager keeps system message during truncation") {
  // system -> 1 token, user "hello world" -> 3 tokens, user "bye" -> 1 token. Total 5.
  // Window 3 forces a drop, never the system prompt.
  bjorne_agent::context::ContextManager manager = makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Truncate, 3));

  MessageList messages;
  add(messages, "system", "sys");
  add(messages, "user", "hello world");
  add(messages, "user", "bye");
  std::vector<bjorne_agent::model::ModelMessage> out;
  std::string error;
  REQUIRE(manager.trim(messages, out, error));
  REQUIRE(error.empty());
  REQUIRE(out.size() == 2);
  REQUIRE(out[0].role == "system");
  REQUIRE(out[0].content == "sys");
  REQUIRE(out[1].role == "user");
  REQUIRE(out[1].content == "bye");
}

TEST_CASE("Context manager reports error when context cannot fit after truncation") {
  // System prompt alone is 12 tokens, window is 5, no other messages.
  bjorne_agent::context::ContextManager manager = makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Truncate, 5));

  MessageList messages;
  add(messages, "system", "you are a helpful assistant with a long prompt");
  std::vector<bjorne_agent::model::ModelMessage> out;
  std::string error;
  REQUIRE_FALSE(manager.trim(messages, out, error));
  REQUIRE(!error.empty());
}

TEST_CASE("Context manager compresses via handler when strategy is compress") {
  bjorne_agent::context::CompressionHandler handler =
      [](const MessageList&, const std::string& instruction, std::string& summary, std::string&) -> bool {
        summary = "compressed summary of: " + instruction;
        return true;
      };
  bjorne_agent::context::ContextManager manager = makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Compress, 2), handler);

  MessageList messages;
  add(messages, "system", "sys");
  add(messages, "user", "hello world");
  std::vector<bjorne_agent::model::ModelMessage> out;
  std::string error;
  REQUIRE(manager.trim(messages, out, error));
  REQUIRE(error.empty());
  REQUIRE(out.size() == 2);
  REQUIRE(out[0].role == "system");
  REQUIRE(out[0].content == "sys");
  REQUIRE(out[1].role == "user");
  REQUIRE(out[1].content == "compressed summary of: Summarize the conversation");
}

TEST_CASE("Context manager reports error when compression handler fails") {
  bjorne_agent::context::CompressionHandler handler =
      [](const MessageList&, const std::string&, std::string&, std::string& error) -> bool {
        error = "mock compression failed";
        return false;
      };
  bjorne_agent::context::ContextManager manager =
      makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Compress, 2), handler);

  MessageList messages;
  add(messages, "user", "hello world");
  std::vector<bjorne_agent::model::ModelMessage> out;
  std::string error;
  REQUIRE_FALSE(manager.trim(messages, out, error));
  REQUIRE(error == "mock compression failed");
}

TEST_CASE("Context manager reports error when compression strategy has no handler") {
  bjorne_agent::context::ContextManager manager = makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Compress, 2));

  MessageList messages;
  add(messages, "user", "hello world");
  std::vector<bjorne_agent::model::ModelMessage> out;
  std::string error;
  REQUIRE_FALSE(manager.trim(messages, out, error));
  REQUIRE(!error.empty());
}

TEST_CASE("Context manager estimates tokens from message content") {
  MessageList messages;
  add(messages, "user", "hello world");
  add(messages, "assistant", "hi");
  bjorne_agent::context::ContextManager manager = makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Truncate, 100));
  REQUIRE(manager.estimateTokens(messages) == static_cast<long long>(3) + static_cast<long long>(1));
}

TEST_CASE("Context manager rejects an empty compression summary") {
  bjorne_agent::context::CompressionHandler handler =
      [](const MessageList&, const std::string&, std::string& summary, std::string&) {
        summary.clear();
        return true;
      };
  bjorne_agent::context::ContextManager manager =
      makeManager(makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Compress, 2), handler);

  MessageList messages;
  add(messages, "user", "hello world");
  std::vector<bjorne_agent::model::ModelMessage> out;
  std::string error;
  REQUIRE_FALSE(manager.trim(messages, out, error));
  REQUIRE(error == "Compression returned an empty summary");
}

TEST_CASE("Context manager exposes its configuration and handler") {
  bjorne_agent::context::ContextManagerConfig config =
      makeConfig(bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Compress, 512);
  config.compression_prompt = "summarize please";

  bool called = false;
  bjorne_agent::context::CompressionHandler handler =
      [&called](const MessageList&, const std::string&, std::string& summary, std::string&) {
        called = true;
        summary = "summary";
        return true;
      };
  bjorne_agent::context::ContextManager manager = makeManager(config, handler);

  REQUIRE(manager.config().overflow_strategy == bjorne_agent::context::ContextManagerConfig::OverflowStrategy::Compress);
  REQUIRE(manager.config().context_window == 512);
  REQUIRE(manager.config().compression_prompt == "summarize please");

  std::string summary;
  std::string error;
  REQUIRE(manager.compressionHandler()(MessageList{}, "instruction", summary, error));
  REQUIRE(called);
  REQUIRE(summary == "summary");
}
