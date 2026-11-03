#include "agent/AgentLoop.h"

#include "log/Log.h"

#include <string>
#include <utility>
#include <vector>

namespace bjorne_agent {
namespace agent {

AgentLoop::AgentLoop(AgentLoopConfig config, std::shared_ptr<model::ModelBackend> model,
                     std::shared_ptr<context::ContextManager> context, std::shared_ptr<tools::ToolRegistry> tools)
    : config_(std::move(config)), model_(std::move(model)), context_(std::move(context)), tools_(std::move(tools)) {}

std::vector<model::ModelMessage> AgentLoop::buildContext(const std::string& task) const {
  std::vector<model::ModelMessage> messages;
  if (!config_.system_prompt.empty()) {
    model::ModelMessage system;
    system.role = "system";
    system.content = config_.system_prompt;
    messages.push_back(system);
  }
  model::ModelMessage user;
  user.role = "user";
  user.content = task;
  messages.push_back(user);
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

bool AgentLoop::run(const std::string& task, std::string& result, std::string& error) {
  std::vector<model::ModelMessage> messages = buildContext(task);
  for (long long iteration = 0; iteration < config_.max_iterations; ++iteration) {
    std::vector<model::ModelMessage> trimmed;
    if (!context_->trim(messages, trimmed, error)) {
      log::fail(error);
      return false;
    }
    log::progress("Iteration " + std::to_string(iteration + 1) + " of " + std::to_string(config_.max_iterations) +
                  ", sending " + std::to_string(trimmed.size()) + " messages");
    model::ModelResponse response;
    if (!model_->respond(trimmed, response, error)) {
      log::fail(error);
      return false;
    }
    if (response.tool_calls.empty()) {
      log::progress("Model returned a final response");
      result = response.content;
      return true;
    }
    log::progress("Model requested " + std::to_string(response.tool_calls.size()) + " tool call(s)");
    appendToolTurn(response, messages);
  }
  error = "Iteration cap reached without a final response";
  log::fail(error);
  return false;
}

}  // namespace agent
}  // namespace bjorne_agent
