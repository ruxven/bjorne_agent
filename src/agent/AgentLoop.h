#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "context/ContextManager.h"
#include "model/ModelClient.h"
#include "tools/Tools.h"

namespace bjorne_agent {
namespace agent {

struct AgentLoopConfig {
  std::string system_prompt;
  std::string workspace;
  long long max_iterations = 25;
  // Conversation to continue from when resuming. The stored system message, if
  // there is one, is left in place; otherwise the configured prompt is added.
  std::vector<model::ModelMessage> history;
  // Polled before and after each model call. A shutdown handler only sets a
  // flag, and this is where the loop notices it and saves the session.
  std::function<bool()> stop_requested;
};

// Seam between the loop and session persistence. The default methods do
// nothing, so a one-shot run without a session store ignores the events.
class LoopObserver {
 public:
  virtual ~LoopObserver() = default;

  // A model turn finished and its tool results are in the conversation.
  virtual void onTurn(const std::vector<model::ModelMessage>& messages, long long turn) { (void)messages; (void)turn; }

  // The model produced a final answer, which is now the last message.
  virtual void onComplete(const std::vector<model::ModelMessage>& messages, const std::string& result) {
    (void)messages;
    (void)result;
  }

  // The loop stopped without a final answer.
  virtual void onFailure(const std::vector<model::ModelMessage>& messages, const std::string& error) {
    (void)messages;
    (void)error;
  }

  // A shutdown request stopped the loop between steps.
  virtual void onInterrupt(const std::vector<model::ModelMessage>& messages) { (void)messages; }
};

// Runs the loop from SPEC.LOOP.md: assemble context, call the model, execute
// tool calls, append results, repeat. Stops on a final response or the
// iteration cap. A model error stops the loop. A tool error becomes tool
// result text and the loop continues.
class AgentLoop {
 public:
  AgentLoop(AgentLoopConfig config, std::shared_ptr<model::ModelBackend> model,
            std::shared_ptr<context::ContextManager> context, std::shared_ptr<tools::ToolRegistry> tools,
            std::shared_ptr<LoopObserver> observer = nullptr);

  bool run(const std::string& task, std::string& result, std::string& error);

 private:
  std::vector<model::ModelMessage> buildContext(const std::string& task) const;
  void appendToolTurn(const model::ModelResponse& response, std::vector<model::ModelMessage>& messages) const;
  bool interrupt(std::vector<model::ModelMessage>& messages, std::string& error);
  void notifyFailure(const std::vector<model::ModelMessage>& messages, const std::string& error);

  AgentLoopConfig config_;
  std::shared_ptr<model::ModelBackend> model_;
  std::shared_ptr<context::ContextManager> context_;
  std::shared_ptr<tools::ToolRegistry> tools_;
  std::shared_ptr<LoopObserver> observer_;
};

}  // namespace agent
}  // namespace bjorne_agent
