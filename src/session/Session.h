#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "model/ModelClient.h"

namespace bjorne_agent {
namespace session {

// One stored conversation. The messages are the exact sequence the loop sent
// and received, including the system message and tool results, so a resume can
// continue without guessing.
struct SessionState {
  std::string id;
  long long created = 0;
  long long updated = 0;
  std::string task;
  std::string workspace;
  std::string model;
  // running, complete, error, or interrupted.
  std::string status = "running";
  std::string result;
  std::string error;
  long long turns = 0;
  std::vector<model::ModelMessage> messages;
};

// The row the `sessions` command prints. Listing does not read every message,
// so a large conversation does not slow the list down more than the parse.
struct SessionSummary {
  std::string id;
  long long created = 0;
  long long updated = 0;
  std::string status;
  std::string task;
  std::size_t messages = 0;
};

// A directory of session files, one `<id>.json` per session. Every write goes
// to `<id>.json.tmp` first and is published with a rename, so a process that
// dies mid-write leaves the previous record intact.
class SessionStore {
 public:
  // Creates the directory when it is missing. Returns null with the reason in
  // error when the path cannot be created or is not a directory.
  static std::unique_ptr<SessionStore> open(const std::string& directory, std::string& error);

  SessionStore(const SessionStore&) = delete;
  SessionStore& operator=(const SessionStore&) = delete;

  const std::string& directory() const { return directory_; }

  // Generates an id, stamps the record, and writes it. The caller fills in the
  // task, workspace and model first.
  bool begin(SessionState& state, std::string& error);

  // Writes the record and stamps updated. An unsafe id is refused.
  bool save(SessionState& state, std::string& error);

  bool load(const std::string& id, SessionState& state, std::string& error) const;

  // Sessions ordered by updated time, newest first. A file that cannot be read
  // or parsed is skipped rather than failing the list.
  std::vector<SessionSummary> list(std::string& error) const;

  // The id of the most recently updated session, or an empty string when the
  // store has none.
  std::string latest(std::string& error) const;

 private:
  explicit SessionStore(std::string directory);

  std::string pathFor(const std::string& id) const;

  std::string directory_;
};

// A session id is used as a file name, so only letters, digits, dash,
// underscore and dot are accepted. The generated ids use digits and dashes.
bool isValidSessionId(const std::string& id);

std::string generateSessionId();

// UTC, "YYYY-MM-DD HH:MM:SS". Returns an empty string when the platform cannot
// format the value.
std::string formatTimestamp(long long epochSeconds);

// Adds a synthetic tool result for every tool call that has no result yet. A
// container can die after the model requested a tool and before the result was
// recorded; the API then rejects the assistant message on resume. Returns the
// number of results added.
std::size_t repairDanglingToolCalls(std::vector<model::ModelMessage>& messages);

}  // namespace session
}  // namespace bjorne_agent
