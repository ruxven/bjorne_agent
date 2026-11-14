#include "interaction/Interaction.h"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "log/Log.h"
#include "log/LogCapture.h"

namespace {

using bjorne_agent::interaction::Channel;
using bjorne_agent::interaction::Request;
using bjorne_agent_test::LevelGuard;

class ScriptedChannel final : public Channel {
 public:
  bool available() const override { return true; }

  bool ask(const Request& request, std::string& answer, std::string& error) override {
    requests.push_back(request);
    if (fail) {
      error = failure;
      return false;
    }
    answer = next_answer;
    return true;
  }

  std::vector<Request> requests;
  std::string next_answer;
  bool fail = false;
  std::string failure;
};

}  // namespace

TEST_CASE("an unavailable channel explains itself") {
  bjorne_agent::interaction::UnavailableChannel channel("agent.interactive is false");
  REQUIRE_FALSE(channel.available());

  Request request;
  request.question = "Proceed?";
  std::string answer;
  std::string error;
  REQUIRE_FALSE(channel.ask(request, answer, error));
  REQUIRE(error.find("agent.interactive is false") != std::string::npos);
}

TEST_CASE("the channel factory refuses a disabled or piped console") {
  REQUIRE_FALSE(bjorne_agent::interaction::makeChannel(false, true)->available());
  REQUIRE_FALSE(bjorne_agent::interaction::makeChannel(true, false)->available());
  REQUIRE(bjorne_agent::interaction::makeChannel(true, true)->available());
}

TEST_CASE("the console channel returns a free form answer") {
  std::istringstream input("  the answer  \n");
  std::ostringstream output;
  bjorne_agent::interaction::ConsoleChannel channel(input, output);

  Request request;
  request.question = "What should I do?";
  std::string answer;
  std::string error;
  REQUIRE(channel.ask(request, answer, error));
  REQUIRE(answer == "the answer");
  REQUIRE(output.str().find("What should I do?") != std::string::npos);
  REQUIRE(output.str().find("> ") != std::string::npos);
}

TEST_CASE("the console channel turns a number into the matching option") {
  std::istringstream input("2\n");
  std::ostringstream output;
  bjorne_agent::interaction::ConsoleChannel channel(input, output);

  Request request;
  request.question = "Which one?";
  request.options = {"first", "second", "third"};
  std::string answer;
  std::string error;
  REQUIRE(channel.ask(request, answer, error));
  REQUIRE(answer == "second");
  REQUIRE(output.str().find("1. first") != std::string::npos);
  REQUIRE(output.str().find("Choose 1-3") != std::string::npos);
}

TEST_CASE("the console channel accepts an option by its text") {
  std::istringstream input("third\n");
  std::ostringstream output;
  bjorne_agent::interaction::ConsoleChannel channel(input, output);

  Request request;
  request.question = "Which one?";
  request.options = {"first", "second", "third"};
  std::string answer;
  std::string error;
  REQUIRE(channel.ask(request, answer, error));
  REQUIRE(answer == "third");
}

TEST_CASE("the console channel rejects an answer outside the options") {
  std::istringstream input("something else\n");
  std::ostringstream output;
  bjorne_agent::interaction::ConsoleChannel channel(input, output);

  Request request;
  request.question = "Which one?";
  request.options = {"first", "second"};
  std::string answer;
  std::string error;
  REQUIRE_FALSE(channel.ask(request, answer, error));
  REQUIRE(error.find("not one of the offered options") != std::string::npos);
}

TEST_CASE("the console channel rejects an out of range choice") {
  std::istringstream input("7\n");
  std::ostringstream output;
  bjorne_agent::interaction::ConsoleChannel channel(input, output);

  Request request;
  request.question = "Which one?";
  request.options = {"first", "second"};
  std::string answer;
  std::string error;
  REQUIRE_FALSE(channel.ask(request, answer, error));
  REQUIRE(error.find("not one of the offered options") != std::string::npos);
}

TEST_CASE("the console channel rejects an empty answer") {
  std::istringstream input("\n");
  std::ostringstream output;
  bjorne_agent::interaction::ConsoleChannel channel(input, output);

  Request request;
  request.question = "Well?";
  std::string answer;
  std::string error;
  REQUIRE_FALSE(channel.ask(request, answer, error));
  REQUIRE(error.find("No answer") != std::string::npos);
}

TEST_CASE("the console channel reports a closed input") {
  std::istringstream input("");
  std::ostringstream output;
  bjorne_agent::interaction::ConsoleChannel channel(input, output);

  Request request;
  request.question = "Well?";
  std::string answer;
  std::string error;
  REQUIRE_FALSE(channel.ask(request, answer, error));
  REQUIRE(error.find("closed") != std::string::npos);
}

TEST_CASE("the request_input tool returns the answer and the request") {
  const LevelGuard guard(bjorne_agent::log::Level::Quiet);
  auto channel = std::make_shared<ScriptedChannel>();
  channel->next_answer = "the answer";

  bjorne_agent::tools::ToolRegistry registry("");
  bjorne_agent::interaction::registerRequestInputTool(registry, channel);

  const bjorne_agent::tools::ToolResult result =
      registry.execute("request_input", R"({"question":"Proceed?","options":["yes","no"]})");
  REQUIRE(result.ok);
  REQUIRE(result.output == "the answer");
  REQUIRE(channel->requests.size() == 1);
  REQUIRE(channel->requests[0].question == "Proceed?");
  REQUIRE(channel->requests[0].options == std::vector<std::string>{"yes", "no"});
}

TEST_CASE("the request_input tool rejects malformed arguments") {
  const LevelGuard guard(bjorne_agent::log::Level::Quiet);
  auto channel = std::make_shared<ScriptedChannel>();
  bjorne_agent::tools::ToolRegistry registry("");
  bjorne_agent::interaction::registerRequestInputTool(registry, channel);

  const bjorne_agent::tools::ToolResult noQuestion = registry.execute("request_input", R"({"options":["yes"]})");
  REQUIRE_FALSE(noQuestion.ok);
  REQUIRE(noQuestion.output.find("question") != std::string::npos);

  const bjorne_agent::tools::ToolResult emptyQuestion = registry.execute("request_input", R"({"question":""})");
  REQUIRE_FALSE(emptyQuestion.ok);

  const bjorne_agent::tools::ToolResult wrongOptions =
      registry.execute("request_input", R"({"question":"Proceed?","options":"yes"})");
  REQUIRE_FALSE(wrongOptions.ok);
  REQUIRE(wrongOptions.output.find("array") != std::string::npos);

  const bjorne_agent::tools::ToolResult badOption =
      registry.execute("request_input", R"({"question":"Proceed?","options":[1]})");
  REQUIRE_FALSE(badOption.ok);
  REQUIRE(badOption.output.find("strings") != std::string::npos);

  const bjorne_agent::tools::ToolResult notAnObject = registry.execute("request_input", "[]");
  REQUIRE_FALSE(notAnObject.ok);
  REQUIRE(channel->requests.empty());
}

TEST_CASE("the request_input tool reports a failed channel as a tool error") {
  const LevelGuard guard(bjorne_agent::log::Level::Quiet);
  auto channel = std::make_shared<ScriptedChannel>();
  channel->fail = true;
  channel->failure = "No interactive channel is available: agent.interactive is false";

  bjorne_agent::tools::ToolRegistry registry("");
  bjorne_agent::interaction::registerRequestInputTool(registry, channel);

  const bjorne_agent::tools::ToolResult result = registry.execute("request_input", R"({"question":"Proceed?"})");
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.output.find("No interactive channel") != std::string::npos);
}

TEST_CASE("the request_input tool is in the model schema") {
  bjorne_agent::tools::ToolRegistry registry("");
  bjorne_agent::interaction::registerRequestInputTool(
      registry, bjorne_agent::interaction::makeChannel(false));

  REQUIRE(registry.hasTool("request_input"));
  const std::string definitions = registry.definitionsJson();
  REQUIRE(definitions.find("\"request_input\"") != std::string::npos);
  REQUIRE(definitions.find("\"options\"") != std::string::npos);
}
