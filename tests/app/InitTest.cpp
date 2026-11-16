#include "app/Init.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "config/Config.h"
#include "config/ConfigPaths.h"
#include "config/Defaults.h"
#include "platform/Platform.h"

#include <catch_amalgamated.hpp>

namespace {

using bjorne_agent::app::Init;

// A directory that is removed when the test ends.
class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("bjorne_agent_init_test_" + std::to_string(bjorne_agent::platform::processId()) + "_" +
             std::to_string(++counter));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string str() const { return path_.string(); }
  std::string file(const std::string& name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

// Points the user level config directory at a temporary directory, so a test
// never writes into the home directory of the user running it.
class UserConfigDirectory {
 public:
  explicit UserConfigDirectory(const std::string& directory) {
    bjorne_agent::platform::setEnvironmentVariable("XDG_CONFIG_HOME", directory);
  }
  ~UserConfigDirectory() { bjorne_agent::platform::unsetEnvironmentVariable("XDG_CONFIG_HOME"); }
  UserConfigDirectory(const UserConfigDirectory&) = delete;
  UserConfigDirectory& operator=(const UserConfigDirectory&) = delete;
};

// Moves the process into a directory for the length of the test, so a test of
// the project local path writes there and nowhere else.
class CurrentDirectory {
 public:
  explicit CurrentDirectory(const std::string& directory) : previous_(std::filesystem::current_path()) {
    std::error_code code;
    std::filesystem::current_path(directory, code);
  }
  ~CurrentDirectory() {
    std::error_code code;
    std::filesystem::current_path(previous_, code);
  }
  CurrentDirectory(const CurrentDirectory&) = delete;
  CurrentDirectory& operator=(const CurrentDirectory&) = delete;

 private:
  std::filesystem::path previous_;
};

std::string readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

void writeFile(const std::string& path, const std::string& content) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << content;
}

// The answers of a full run, after the location was chosen: the endpoint, the
// model, the key variable, the workspace, the certificate question and the
// hostname.
const char* kFullAnswers = "https://api.example.com/v1\n"
                           "example-model\n"
                           "EXAMPLE_KEY\n"
                           "/work\n"
                           "n\n"
                           "\n";

TEST_CASE("The init command writes a config, the prompts and the environment file") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());

  std::istringstream input(std::string("2\n") + kFullAnswers);
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);

  // The wizard prints the location it writes with the forward slash it joins
  // with, so the expected text uses that spelling on every platform.
  const std::string configPath = directory.str() + "/bjorne_agent/bjorne_agent.json";
  REQUIRE(std::filesystem::exists(configPath));

  bjorne_agent::config::Config config;
  std::string error;
  REQUIRE(bjorne_agent::config::load(configPath, config, error));
  REQUIRE(error.empty());
  REQUIRE(config.agent.workspace == "/work");
  REQUIRE(config.agent.system_prompt == bjorne_agent::config::kDefaultSystemPrompt);
  REQUIRE(config.agent.system_prompt_path == "prompts/system.md");
  REQUIRE(config.models.size() == 1);
  REQUIRE(config.models[0].endpoint == "https://api.example.com/v1");
  REQUIRE(config.models[0].model == "example-model");
  REQUIRE(config.models[0].api_key_env == "EXAMPLE_KEY");
  REQUIRE(config.models[0].compression_prompt_path == "prompts/compression.md");
  REQUIRE(config.models[0].ignore_self_signed == false);

  // The prompt files sit next to the config, and the config points at them
  // with a relative path.
  const std::string promptDirectory = directory.str() + "/bjorne_agent/prompts";
  REQUIRE(readFile(promptDirectory + "/compression.md") == bjorne_agent::config::defaultCompressionPrompt());
  REQUIRE(readFile(promptDirectory + "/system.md") ==
          std::string(bjorne_agent::config::kDefaultSystemPrompt) + "\n");
  REQUIRE(readFile(directory.file("bjorne_agent/.env")) == "EXAMPLE_KEY=\n");

  // The output names every file it wrote.
  REQUIRE(output.str().find(configPath) != std::string::npos);
  REQUIRE(output.str().find(promptDirectory + "/compression.md") != std::string::npos);
}

TEST_CASE("The init command writes to the project local path") {
  TempDir directory;
  const CurrentDirectory workingDirectory(directory.str());
  const UserConfigDirectory userDirectory(directory.file("home"));

  std::istringstream input("1\n"
                            "https://api.example.com/v1\n"
                            "\n"
                            "\n"
                            "\n"
                            "n\n"
                            "\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);
  REQUIRE(std::filesystem::exists(directory.file(".bjorne_agent/bjorne_agent.json")));
  REQUIRE(std::filesystem::exists(directory.file(".bjorne_agent/prompts/compression.md")));
}

TEST_CASE("The init command keeps an existing config when the overwrite is declined") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());
  const std::string configPath = directory.file("bjorne_agent/bjorne_agent.json");
  writeFile(configPath, "{\"version\": 1, \"agent\": {}, \"models\": []}\n");

  std::istringstream input("2\nn\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);
  REQUIRE(readFile(configPath) == "{\"version\": 1, \"agent\": {}, \"models\": []}\n");
  REQUIRE(output.str().find("Kept") != std::string::npos);
}

TEST_CASE("The init command overwrites an existing config when the overwrite is confirmed") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());
  const std::string configPath = directory.file("bjorne_agent/bjorne_agent.json");
  writeFile(configPath, "{\"version\": 1, \"agent\": {}, \"models\": []}\n");

  std::istringstream input(std::string("2\ny\n") + kFullAnswers);
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);
  REQUIRE(readFile(configPath) != "{\"version\": 1, \"agent\": {}, \"models\": []}\n");

  bjorne_agent::config::Config config;
  std::string error;
  REQUIRE(bjorne_agent::config::load(configPath, config, error));
  REQUIRE(config.models.size() == 1);
}

TEST_CASE("The init command keeps a prompt file the user already edited") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());
  const std::string promptPath = directory.file("bjorne_agent/prompts/compression.md");
  writeFile(promptPath, "My own instruction.\n");

  std::istringstream input(std::string("2\n") + kFullAnswers);
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);
  REQUIRE(readFile(promptPath) == "My own instruction.\n");
  REQUIRE(output.str().find("Kept") != std::string::npos);
}

TEST_CASE("The init command reports that it needs a terminal") {
  std::istringstream input(kFullAnswers);
  std::ostringstream output;
  Init init(input, output, false);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("not a terminal") != std::string::npos);
}

TEST_CASE("The init command reports an answer that is not one of the choices") {
  std::istringstream input("9\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("is not one of the choices") != std::string::npos);
}

TEST_CASE("The init command accepts the name of a choice") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());

  // The wizard prints the user level location as the temporary directory with
  // the config directory and file appended with forward slashes, so the answer
  // has to be spelled the same way to match it.
  const std::string choice = directory.str() + "/bjorne_agent/bjorne_agent.json";
  std::istringstream input(std::string("User level: ") + choice + "\nhttps://api.example.com/v1\n\n\n\nn\n\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);
  REQUIRE(std::filesystem::exists(directory.file("bjorne_agent/bjorne_agent.json")));
}

TEST_CASE("The init command requires an endpoint") {
  std::istringstream input("2\n\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("The endpoint is required.") != std::string::npos);
}

TEST_CASE("The init command reports that the input ended") {
  std::istringstream input("2\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("The input ended before the question was answered.") != std::string::npos);
}

TEST_CASE("The init command takes the default answers") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());

  std::istringstream input("2\nhttps://api.example.com/v1\n\n\n\n\n\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);

  bjorne_agent::config::Config config;
  std::string error;
  REQUIRE(bjorne_agent::config::load(directory.file("bjorne_agent/bjorne_agent.json"), config, error));
  REQUIRE(config.models[0].model == "gpt-4o");
  REQUIRE(config.models[0].api_key_env == "OPENAI_API_KEY");
  REQUIRE(config.models[0].hostname.empty());
  // The workspace defaults to the working directory.
  REQUIRE_FALSE(config.agent.workspace.empty());
}

TEST_CASE("The init command reports that the input ended before the location was chosen") {
  std::istringstream input("");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("The input ended before the question was answered.") != std::string::npos);
}

TEST_CASE("The init command takes the first location when the answer is empty") {
  TempDir directory;
  const CurrentDirectory workingDirectory(directory.str());
  const UserConfigDirectory userDirectory(directory.file("home"));

  std::istringstream input("\n"
                            "https://api.example.com/v1\n"
                            "\n"
                            "\n"
                            "\n"
                            "n\n"
                            "\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);
  REQUIRE(std::filesystem::exists(directory.file(".bjorne_agent/bjorne_agent.json")));
}

TEST_CASE("The init command reports a number that is too large to be a choice") {
  std::istringstream input("99999999999999999999\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("is not one of the choices") != std::string::npos);
}

TEST_CASE("The init command reports that the input ended before the overwrite was answered") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());
  writeFile(directory.file("bjorne_agent/bjorne_agent.json"), "{}\n");

  std::istringstream input("2\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("The input ended before the question was answered.") != std::string::npos);
}

TEST_CASE("The init command reports that the input ended at any later question") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());

  // The answers stop one question earlier each time, so every question of the
  // wizard is reached with the input already at its end.
  const std::vector<std::string> stops = {
      "2\n",
      "2\nhttps://api.example.com/v1\n",
      "2\nhttps://api.example.com/v1\nmodel\n",
      "2\nhttps://api.example.com/v1\nmodel\nKEY\n",
      "2\nhttps://api.example.com/v1\nmodel\nKEY\n/work\n",
      "2\nhttps://api.example.com/v1\nmodel\nKEY\n/work\nn\n",
  };

  for (const std::string& answers : stops) {
    std::istringstream input(answers);
    std::ostringstream output;
    Init init(input, output, true);

    INFO("answers: " << answers);
    REQUIRE(init.run() == 1);
    REQUIRE(output.str().find("The input ended before the question was answered.") != std::string::npos);
  }
}

TEST_CASE("The init command keeps an environment file that is already there") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());
  const std::string environment = directory.file("bjorne_agent/.env");
  writeFile(environment, "MY_OWN_KEY=secret\n");

  std::istringstream input(std::string("2\n") + kFullAnswers);
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);
  REQUIRE(readFile(environment) == "MY_OWN_KEY=secret\n");
  REQUIRE(output.str().find("Kept") != std::string::npos);
}

TEST_CASE("The init command keeps a system prompt that is already there") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());
  const std::string prompt = directory.file("bjorne_agent/prompts/system.md");
  writeFile(prompt, "My own system prompt.\n");

  std::istringstream input(std::string("2\n") + kFullAnswers);
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);
  REQUIRE(readFile(prompt) == "My own system prompt.\n");
  REQUIRE(output.str().find("Kept") != std::string::npos);
}

TEST_CASE("The init command reports a config directory it cannot create") {
  TempDir directory;
  const CurrentDirectory workingDirectory(directory.str());
  const UserConfigDirectory userDirectory(directory.file("home"));

  // A file where the config directory should be makes the directory creation
  // fail.
  writeFile(directory.file(".bjorne_agent"), "not a directory\n");

  std::istringstream input("1\n"
                            "https://api.example.com/v1\n"
                            "\n"
                            "\n"
                            "\n"
                            "n\n"
                            "\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("Cannot create") != std::string::npos);
}

TEST_CASE("The init command reports a prompt directory it cannot create") {
  TempDir directory;
  const CurrentDirectory workingDirectory(directory.str());
  const UserConfigDirectory userDirectory(directory.file("home"));

  // A file where the prompt directory should be makes the directory creation
  // fail.
  writeFile(directory.file(".bjorne_agent/prompts"), "not a directory\n");

  std::istringstream input("1\n"
                            "https://api.example.com/v1\n"
                            "\n"
                            "\n"
                            "\n"
                            "n\n"
                            "\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("Cannot create") != std::string::npos);
}

TEST_CASE("The init command reports a config file it cannot write") {
  TempDir directory;
  const CurrentDirectory workingDirectory(directory.str());
  const UserConfigDirectory userDirectory(directory.file("home"));

  // A directory where the config file should be makes the file open fail.
  std::filesystem::create_directories(directory.file(".bjorne_agent/bjorne_agent.json"));

  std::istringstream input("1\n"
                            "y\n"
                            "https://api.example.com/v1\n"
                            "\n"
                            "\n"
                            "\n"
                            "n\n"
                            "\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 1);
  REQUIRE(output.str().find("Cannot write") != std::string::npos);
}

TEST_CASE("The init command trims the spaces around an answer") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());

  std::istringstream input("2\n"
                            "  https://api.example.com/v1  \n"
                            "\n"
                            "\n"
                            "\n"
                            "n\n"
                            "\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);

  bjorne_agent::config::Config config;
  std::string error;
  REQUIRE(bjorne_agent::config::load(directory.file("bjorne_agent/bjorne_agent.json"), config, error));
  REQUIRE(config.models[0].endpoint == "https://api.example.com/v1");
}

TEST_CASE("The init command accepts a self-signed certificate") {
  TempDir directory;
  const UserConfigDirectory userDirectory(directory.str());

  std::istringstream input("2\nhttps://api.example.com/v1\n\n\n\ny\nhost.example\n");
  std::ostringstream output;
  Init init(input, output, true);

  REQUIRE(init.run() == 0);

  bjorne_agent::config::Config config;
  std::string error;
  REQUIRE(bjorne_agent::config::load(directory.file("bjorne_agent/bjorne_agent.json"), config, error));
  REQUIRE(config.models[0].ignore_self_signed == true);
  REQUIRE(config.models[0].hostname == "host.example");
}

}  // namespace
