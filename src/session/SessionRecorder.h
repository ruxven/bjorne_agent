#pragma once

#include <memory>
#include <string>
#include <vector>

#include "agent/AgentLoop.h"
#include "session/Session.h"

namespace bjorne_agent {
namespace session {

// Writes the conversation to the store after every loop event, so a killed
// process loses at most the turn that was still running. A failed write is
// logged and the run continues; the next event tries again.
class SessionRecorder final : public agent::LoopObserver {
 public:
  SessionRecorder(std::shared_ptr<SessionStore> store, SessionState state);

  void onTurn(const std::vector<model::ModelMessage>& messages, long long turn) override;
  void onComplete(const std::vector<model::ModelMessage>& messages, const std::string& result) override;
  void onFailure(const std::vector<model::ModelMessage>& messages, const std::string& error) override;
  void onInterrupt(const std::vector<model::ModelMessage>& messages) override;

  const SessionState& state() const { return state_; }

 private:
  void persist(const std::vector<model::ModelMessage>& messages, const std::string& status, const std::string& result,
               const std::string& error);

  std::shared_ptr<SessionStore> store_;
  SessionState state_;
};

}  // namespace session
}  // namespace bjorne_agent
