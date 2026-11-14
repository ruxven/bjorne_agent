#include "agent/AgentLoop.h"

#include "log/Log.h"

#include <string>
#include <utility>
#include <vector>

namespace bjorne_agent {
namespace agent {

AgentLoop::AgentLoop(AgentLoopConfig config, std::shared_ptr<model::ModelBackend> model,
                     std::shared_ptr<context::ContextManager> context, std::shared_ptr<tools::ToolRegistry> tools,
                     std::shared_ptr<LoopObserver> observer)
    : config_(std::move(config)),
      model_(std::move(model)),
      context_(std::move(context)),
      tools_(std::move(tools)),
      observer_(std::move(observer)) {}

std::vector<model::ModelMessage> AgentLoop::buildContext(const std::string& task) const {
  std::vector<model::ModelMessage> messages = config_.history;
  if (messages.empty() || messages.front().role != "system") {
    if (!config_.system_prompt.empty()) {
      model::ModelMessage system;
      system.role = "system";
      system.content = config_.system_prompt;
      messages.insert(messages.begin(), std::move(system));
    }
  }
  if (!task.empty()) {
    model::ModelMessage user;
    user.role = "user";
    user.content = task;
    messages.push_back(std::move(user));
  }
  return messages;
}

void AgentLoop::appendToolTurn(const model::ModelResponse& response, std::vector<model::ModelMessage>& messages) const {
  model::ModelMessage assistant;
  assistant.role = "assistant";
  assistant.content = response.content;
  assistant.tool_calls = response.tool_calls;
  messages.push_back(assistant);

  for (const model::ToolCall& call : response.tool_calls) {
    const tools::ToolResult outcome = tools_->execute(call.name, call.arguments);
    model::ModelMessage tool;
    tool.role = "tool";
    tool.tool_call_id = call.id;
    tool.content = outcome.ok ? outcome.output : ("Error: " + outcome.output);
    messages.push_back(tool);
  }
}

bool AgentLoop::interrupt(std::vector<model::ModelMessage>& messages, std::string& error) {
  error = "Interrupted by a shutdown signal";
  log::progress("Interrupted, saving the session");
  if (observer_) {
    observer_->onInterrupt(messages);
  }
  return false;
}

void AgentLoop::notifyFailure(const std::vector<model::ModelMessage>& messages, const std::string& error) {
  if (observer_) {
    observer_->onFailure(messages, error);
  }
}

bool AgentLoop::run(const std::string& task, std::string& result, std::string& error) {
  std::vector<model::ModelMessage> messages = buildContext(task);
  if (messages.empty()) {
    error = "No task and no session history to continue";
    log::fail(error);
    notifyFailure(messages, error);
    return false;
  }

  for (long long iteration = 0; iteration < config_.max_iterations; ++iteration) {
    if (config_.stop_requested && config_.stop_requested()) {
      return interrupt(messages, error);
    }

    std::vector<model::ModelMessage> trimmed;
    if (!context_->trim(messages, trimmed, error)) {
      log::fail(error);
      notifyFailure(messages, error);
      return false;
    }
    log::progress("Iteration " + std::to_string(iteration + 1) + " of " + std::to_string(config_.max_iterations) +
                  ", sending " + std::to_string(trimmed.size()) + " messages");
    model::ModelResponse response;
    if (!model_->respond(trimmed, response, error)) {
      log::fail(error);
      notifyFailure(messages, error);
      return false;
    }
    // A shutdown that arrives while the request is in flight is noticed here,
    // before the response is added, so a partial turn is not recorded.
    if (config_.stop_requested && config_.stop_requested()) {
      return interrupt(messages, error);
    }
    // Show what the model is thinking before anything it asked for runs. The
    // final answer goes to stdout, so its content is not repeated here; a
    // reasoning trace is always shown, including on the final turn.
    if (!response.reasoning.empty()) {
      log::block("thinking", response.reasoning);
    }
    if (!response.tool_calls.empty() && !response.content.empty()) {
      log::block("assistant", response.content);
    }
    if (response.tool_calls.empty()) {
      log::progress("Model returned a final response");
      model::ModelMessage assistant;
      assistant.role = "assistant";
      assistant.content = response.content;
      messages.push_back(std::move(assistant));
      result = response.content;
      if (observer_) {
        observer_->onComplete(messages, result);
      }
      return true;
    }
    log::progress("Model requested " + std::to_string(response.tool_calls.size()) + " tool call(s)");
    appendToolTurn(response, messages);
    if (observer_) {
      observer_->onTurn(messages, iteration + 1);
    }
  }
  error = "Iteration cap reached without a final response";
  log::fail(error);
  notifyFailure(messages, error);
  return false;
}

}  // namespace agent
}  // namespace bjorne_agent
