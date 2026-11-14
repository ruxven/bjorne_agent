#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "tools/Tools.h"

namespace bjorne_agent {
namespace interaction {

// What the model asks the user. The options list is empty for a free form
// answer. With options the answer has to match one of them, so a choice is a
// real constraint rather than a suggestion.
struct Request {
  std::string question;
  std::vector<std::string> options;
};

// The seam between the agent and a human. In a container there is usually no
// human and stdin already carried the task, so the default channel is
// unavailable and fails fast instead of waiting for an answer.
class Channel {
 public:
  virtual ~Channel() = default;

  // False when ask can only fail. The tool reports the reason instead of
  // blocking.
  virtual bool available() const = 0;

  // Asks the question and stores the answer. Returns false with a reason when
  // no answer can be collected.
  virtual bool ask(const Request& request, std::string& answer, std::string& error) = 0;
};

// The default channel. It never reads input; it reports why no human can be
// reached.
class UnavailableChannel final : public Channel {
 public:
  explicit UnavailableChannel(std::string reason);

  bool available() const override { return false; }
  bool ask(const Request& request, std::string& answer, std::string& error) override;

 private:
  std::string reason_;
};

// Prompts on a text stream. It is constructed only when standard input is a
// terminal, so the prompt cannot consume the task or wait on a pipe.
class ConsoleChannel final : public Channel {
 public:
  ConsoleChannel(std::istream& input, std::ostream& output);

  bool available() const override { return true; }
  bool ask(const Request& request, std::string& answer, std::string& error) override;

 private:
  std::istream& input_;
  std::ostream& output_;
};

// Builds the channel for the configured mode. With interactive false, or with
// standard input that is not a terminal, the result is unavailable. The second
// form lets a caller supply the terminal detection, which the tests use.
std::shared_ptr<Channel> makeChannel(bool interactive);
std::shared_ptr<Channel> makeChannel(bool interactive, bool standard_input_is_terminal);

// Registers the `request_input` tool. The tool is always registered: an
// unavailable channel produces a tool error with a clear message, which lets
// the model adapt instead of failing the run.
void registerRequestInputTool(tools::ToolRegistry& registry, std::shared_ptr<Channel> channel);

}  // namespace interaction
}  // namespace bjorne_agent
