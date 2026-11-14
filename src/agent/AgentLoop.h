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
  // Asks before a tool runs. Returns false when the call is denied or the
  // request was cancelled while the question was in flight; the loop then ends
  // the turn without executing the tool.
  std::function<bool(const std::string& tool, const std::string& arguments, const std::string& tool_call_id,
                     std::string& denial)>
      permissionGate;
};

// Seam between the loop and session persistence. The default methods do
// nothing, so a one-shot run without a session store ignores the events.
class LoopObserver {
 public:
  virtual ~LoopObserver() = default;

  // A model turn finished and its tool results are in the conversation.
  virtual void onTurn(const std::vector<model::ModelMessage>& messages, long long turn) { (void)messages; (void)turn; }

  // The model asked for tools and also narrated what it is about to do.
  virtual void onAssistantMessage(const std::string& content) { (void)content; }

  // The model returned a reasoning trace alongside its response.
  virtual void onReasoning(const std::string& reasoning) { (void)reasoning; }

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
// result text and the loop continues. An optional permission gate decides
// whether a requested tool may run; a denied call ends the turn as a refusal.
class AgentLoop {
 public:
  AgentLoop(AgentLoopConfig config, std::shared_ptr<model::ModelBackend> model,
            std::shared_ptr<context::ContextManager> context, std::shared_ptr<tools::ToolRegistry> tools,
            std::shared_ptr<LoopObserver> observer = nullptr);

  bool run(const std::string& task, std::string& result, std::string& error);

  // Why the last run stopped: end_turn, refusal, cancelled, max_tokens, or
  // max_turn_requests. Empty when the run failed before deciding a stop.
  std::string stopReason() const { return stop_reason_; }

 private:
  std::vector<model::ModelMessage> buildContext(const std::string& task) const;
  void appendToolTurn(const model::ModelResponse& response, std::vector<model::ModelMessage>& messages) const;
  bool interrupt(std::vector<model::ModelMessage>& messages, std::string& error);
  void notifyFailure(const std::vector<model::ModelMessage>& messages, const std::string& error);
  // True when the caller asked the loop to stop.
  bool stopRequested() const;
  // Trims the conversation to the budget and asks the model for the next step.
  bool callModel(const std::vector<model::ModelMessage>& messages, long long iteration,
                 model::ModelResponse& response, std::string& error);
  // Shows what the model is thinking before anything it asked for runs.
  void narrate(const model::ModelResponse& response);
  // Asks the gate about every tool the model asked for.
  bool approved(const model::ModelResponse& response, std::string& denial);

  AgentLoopConfig config_;
  std::shared_ptr<model::ModelBackend> model_;
  std::shared_ptr<context::ContextManager> context_;
  std::shared_ptr<tools::ToolRegistry> tools_;
  std::shared_ptr<LoopObserver> observer_;
  std::string stop_reason_;
};

}  // namespace agent
}  // namespace bjorne_agent
