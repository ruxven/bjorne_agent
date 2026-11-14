#include "session/SessionRecorder.h"

#include <string>
#include <utility>
#include <vector>

#include "log/Log.h"

namespace bjorne_agent {
namespace session {

SessionRecorder::SessionRecorder(std::shared_ptr<SessionStore> store, SessionState state)
    : store_(std::move(store)), state_(std::move(state)) {}

void SessionRecorder::onTurn(const std::vector<model::ModelMessage>& messages, long long turn) {
  state_.turns = turn;
  persist(messages, "running", {}, {});
}

void SessionRecorder::onComplete(const std::vector<model::ModelMessage>& messages, const std::string& result) {
  persist(messages, "complete", result, {});
}

void SessionRecorder::onFailure(const std::vector<model::ModelMessage>& messages, const std::string& error) {
  persist(messages, "error", {}, error);
}

void SessionRecorder::onInterrupt(const std::vector<model::ModelMessage>& messages) {
  persist(messages, "interrupted", {}, "Interrupted by a shutdown signal");
}

void SessionRecorder::persist(const std::vector<model::ModelMessage>& messages, const std::string& status,
                              const std::string& result, const std::string& error) {
  if (!store_) {
    return;
  }
  state_.messages = messages;
  state_.status = status;
  state_.result = result;
  state_.error = error;

  std::string writeError;
  if (!store_->save(state_, writeError)) {
    log::fail("Cannot save session: " + writeError);
  }
}

}  // namespace session
}  // namespace bjorne_agent
