#pragma once

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
};

// Runs the loop from SPEC.LOOP.md: assemble context, call the model, execute
// tool calls, append results, repeat. Stops on a final response or the
// iteration cap. A model error stops the loop. A tool error becomes tool
// result text and the loop continues.
class AgentLoop {
 public:
  AgentLoop(AgentLoopConfig config, std::shared_ptr<model::ModelBackend> model,
            std::shared_ptr<context::ContextManager> context, std::shared_ptr<tools::ToolRegistry> tools);

  bool run(const std::string& task, std::string& result, std::string& error);

 private:
  std::vector<model::ModelMessage> buildContext(const std::string& task) const;
  void appendToolTurn(const model::ModelResponse& response, std::vector<model::ModelMessage>& messages) const;

  AgentLoopConfig config_;
  std::shared_ptr<model::ModelBackend> model_;
  std::shared_ptr<context::ContextManager> context_;
  std::shared_ptr<tools::ToolRegistry> tools_;
};

}  // namespace agent
}  // namespace bjorne_agent
