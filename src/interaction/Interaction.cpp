#include "interaction/Interaction.h"

#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "json/Json.h"
#include "platform/Platform.h"

namespace bjorne_agent {
namespace interaction {

namespace {

std::string trimmed(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// Parses a one based choice. Returns 0 when the text is not a plain number.
std::size_t choiceNumber(const std::string& text) {
  if (text.empty()) {
    return 0;
  }
  std::size_t value = 0;
  for (const char character : text) {
    if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
      return 0;
    }
    value = value * 10 + static_cast<std::size_t>(character - '0');
    if (value > 1000000) {
      return 0;
    }
  }
  return value;
}

bool readOption(const json::Value& arguments, Request& request, std::string& error) {
  const json::Value* options = arguments.find("options");
  if (options == nullptr) {
    return true;
  }
  if (!options->isArray()) {
    error = "request_input options must be an array of strings";
    return false;
  }
  for (const json::Value& option : options->array) {
    if (!option.isString() || option.string.empty()) {
      error = "request_input options must be non-empty strings";
      return false;
    }
    request.options.push_back(option.string);
  }
  return true;
}

}  // namespace

UnavailableChannel::UnavailableChannel(std::string reason) : reason_(std::move(reason)) {}

bool UnavailableChannel::ask(const Request& request, std::string& answer, std::string& error) {
  (void)request;
  (void)answer;
  error = "No interactive channel is available: " + reason_;
  return false;
}

ConsoleChannel::ConsoleChannel(std::istream& input, std::ostream& output) : input_(input), output_(output) {}

bool ConsoleChannel::ask(const Request& request, std::string& answer, std::string& error) {
  output_ << "\n" << request.question << "\n";
  if (request.options.empty()) {
    output_ << "> ";
  } else {
    for (std::size_t index = 0; index < request.options.size(); ++index) {
      output_ << "  " << (index + 1) << ". " << request.options[index] << "\n";
    }
    output_ << "Choose 1-" << request.options.size() << " or type an option: ";
  }
  output_.flush();

  std::string line;
  if (!std::getline(input_, line)) {
    error = "Standard input closed before an answer was given.";
    return false;
  }
  line = trimmed(line);
  if (line.empty()) {
    error = "No answer was given.";
    return false;
  }
  if (request.options.empty()) {
    answer = line;
    return true;
  }

  const std::size_t choice = choiceNumber(line);
  if (choice >= 1 && choice <= request.options.size()) {
    answer = request.options[choice - 1];
    return true;
  }
  for (const std::string& option : request.options) {
    if (line == option) {
      answer = option;
      return true;
    }
  }
  error = "The answer is not one of the offered options.";
  return false;
}

std::shared_ptr<Channel> makeChannel(bool interactive) {
  return makeChannel(interactive, platform::standardInputIsTerminal());
}

std::shared_ptr<Channel> makeChannel(bool interactive, bool standard_input_is_terminal) {
  if (!interactive) {
    return std::make_shared<UnavailableChannel>("agent.interactive is false");
  }
  if (!standard_input_is_terminal) {
    return std::make_shared<UnavailableChannel>("standard input is not a terminal");
  }
  return std::make_shared<ConsoleChannel>(std::cin, std::cerr);
}

void registerRequestInputTool(tools::ToolRegistry& registry, std::shared_ptr<Channel> channel) {
  tools::ToolDefinition definition;
  definition.name = "request_input";
  definition.description =
      "Ask the user a question. Use this when a decision needs a human. With options, the answer "
      "must be one of them.";
  definition.parameters = json::parse(R"({
    "type": "object",
    "properties": {
      "question": {"type": "string", "description": "Question to ask the user."},
      "options": {
        "type": "array",
        "items": {"type": "string"},
        "description": "Constrained choices. Omit for a free form answer."
      }
    },
    "required": ["question"]
  })");

  registry.registerTool(definition, [channel](const std::string& arguments) {
    json::Value args;
    if (!tools::parseToolArguments(arguments, args)) {
      return tools::ToolResult{false, "request_input requires a JSON object"};
    }
    Request request;
    if (!tools::toolStringArgument(args, "question", request.question) || request.question.empty()) {
      return tools::ToolResult{false, "request_input requires question"};
    }
    std::string error;
    if (!readOption(args, request, error)) {
      return tools::ToolResult{false, error};
    }
    if (!channel) {
      return tools::ToolResult{false, "No interactive channel is available"};
    }
    std::string answer;
    if (!channel->ask(request, answer, error)) {
      return tools::ToolResult{false, error};
    }
    return tools::ToolResult{true, answer};
  });
}

}  // namespace interaction
}  // namespace bjorne_agent
