#include "app/Init.h"

#include "config/ConfigPaths.h"
#include "config/Defaults.h"
#include "json/Json.h"
#include "platform/Platform.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace bjorne_agent {
namespace app {

namespace {

constexpr const char* kProjectDirectory = ".bjorne_agent";
constexpr const char* kConfigFileName = "bjorne_agent.json";
constexpr const char* kPromptDirectory = "prompts";
constexpr const char* kCompressionPromptFile = "compression.md";
constexpr const char* kSystemPromptFile = "system.md";
constexpr const char* kEnvironmentFile = ".env";

// The prompt paths in the configuration are relative, and the run resolves
// them against the directory holding the configuration file.
constexpr const char* kRelativeCompressionPrompt = "prompts/compression.md";
constexpr const char* kRelativeSystemPrompt = "prompts/system.md";

constexpr const char* kProvider = "openai-compatible";
constexpr const char* kDefaultModel = "gpt-4o";
constexpr const char* kDefaultApiKeyEnv = "OPENAI_API_KEY";
constexpr long long kDefaultContextWindow = 128000;
constexpr long long kDefaultMaxTokens = 4096;
constexpr double kDefaultTemperature = 0.7;

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

// The text as a JSON string, with the quotes and the escapes. The serializer
// escapes what has to be escaped, so a path with a quote or a newline in it
// still produces a valid configuration.
std::string quoted(const std::string& text) {
  json::Value value;
  value.type = json::Value::Type::String;
  value.string = text;
  return json::write(value);
}

// A number in the form a person reads. The serializer writes every double with
// seventeen digits, which round-trips but reads badly in a file the user
// edits.
std::string number(double value) {
  char buffer[32];
  const int written = std::snprintf(buffer, sizeof(buffer), "%g", value);
  if (written <= 0) {
    return "0";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

bool exists(const std::string& path) {
  std::error_code code;
  return std::filesystem::exists(path, code) && !code;
}

// The directory that holds a file. An empty path yields an empty directory,
// which means the file sits in the working directory.
std::string directoryOf(const std::string& path) {
  return std::filesystem::path(path).parent_path().string();
}

std::string joinPath(const std::string& base, const std::string& name) {
  if (base.empty()) {
    return name;
  }
  if (base.back() == '/' || base.back() == '\\') {
    return base + name;
  }
  return base + "/" + name;
}

// The configuration file the wizard writes. The two locations are the ones the
// run searches, so a file written here is found without an environment
// variable.
std::string projectConfigPath() {
  return std::string(kProjectDirectory) + "/" + kConfigFileName;
}

std::string userConfigPath() {
  const std::string directory = config::userConfigDirectory();
  if (directory.empty()) {
    return {};
  }
  return joinPath(directory, kConfigFileName);
}

// The configuration file as it is written. The layout follows the example
// configuration, so the two read alike.
std::string configText(const InitAnswers& answers) {
  std::string text;
  text += "{\n";
  text += "  \"version\": 1,\n";
  text += "  \"agent\": {\n";
  text += "    \"system_prompt\": " + quoted(config::kDefaultSystemPrompt) + ",\n";
  text += "    \"system_prompt_path\": " + quoted(kRelativeSystemPrompt) + ",\n";
  text += "    \"workspace\": " + quoted(answers.workspace) + ",\n";
  text += "    \"skills_path\": \"\",\n";
  text += "    \"session_dir\": \"\",\n";
  text += "    \"interactive\": false\n";
  text += "  },\n";
  text += "  \"models\": [\n";
  text += "    {\n";
  text += "      \"provider\": " + quoted(kProvider) + ",\n";
  text += "      \"endpoint\": " + quoted(answers.endpoint) + ",\n";
  text += "      \"model\": " + quoted(answers.model) + ",\n";
  text += "      \"context_window\": " + std::to_string(kDefaultContextWindow) + ",\n";
  text += "      \"max_tokens\": " + std::to_string(kDefaultMaxTokens) + ",\n";
  text += "      \"temperature\": " + number(kDefaultTemperature) + ",\n";
  text += "      \"api_key_env\": " + quoted(answers.api_key_env) + ",\n";
  text += "      \"priority\": 1,\n";
  text += "      \"compression_prompt_path\": " + quoted(kRelativeCompressionPrompt) + ",\n";
  text += "      \"ignore_self_signed\": " + std::string(answers.ignore_self_signed ? "true" : "false") + ",\n";
  text += "      \"hostname\": " + quoted(answers.hostname) + "\n";
  text += "    }\n";
  text += "  ],\n";
  text += "  \"mcp_servers\": {}\n";
  text += "}\n";
  return text;
}

}  // namespace

Init::Init(std::istream& input, std::ostream& output, bool input_is_terminal)
    : input_(input), output_(output), input_is_terminal_(input_is_terminal) {}

int Init::run() {
  if (!input_is_terminal_) {
    output_ << "The init command asks its questions on the terminal, and standard input is not a "
                "terminal.\n";
    return 1;
  }

  InitAnswers answers;
  const CollectOutcome outcome = collect(answers);
  if (outcome == CollectOutcome::Failed) {
    return 1;
  }
  if (outcome == CollectOutcome::KeptExisting) {
    return 0;
  }
  if (!write(answers)) {
    return 1;
  }
  return 0;
}

bool Init::ask(const std::string& question, const std::string& fallback, std::string& out) {
  output_ << question;
  if (!fallback.empty()) {
    output_ << " [" << fallback << "]";
  }
  output_ << ": ";
  output_.flush();

  std::string line;
  if (!std::getline(input_, line)) {
    output_ << "\n";
    output_ << "The input ended before the question was answered.\n";
    return false;
  }
  line = trimmed(line);
  out = line.empty() ? fallback : line;
  return true;
}

bool Init::askChoice(const std::string& question, const std::vector<std::string>& options, std::size_t& out) {
  output_ << question << "\n";
  for (std::size_t index = 0; index < options.size(); ++index) {
    output_ << "  " << (index + 1) << ". " << options[index] << "\n";
  }
  output_ << "Choose 1-" << options.size() << " [1]: ";
  output_.flush();

  std::string line;
  if (!std::getline(input_, line)) {
    output_ << "\n";
    output_ << "The input ended before the question was answered.\n";
    return false;
  }
  line = trimmed(line);
  if (line.empty()) {
    out = 0;
    return true;
  }

  bool digits = !line.empty();
  std::size_t number = 0;
  for (const char character : line) {
    if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
      digits = false;
      break;
    }
    number = number * 10 + static_cast<std::size_t>(character - '0');
    if (number > 1000000) {
      digits = false;
      break;
    }
  }
  if (digits && number >= 1 && number <= options.size()) {
    out = number - 1;
    return true;
  }
  for (std::size_t index = 0; index < options.size(); ++index) {
    if (options[index] == line) {
      out = index;
      return true;
    }
  }

  output_ << "\n\"" << line << "\" is not one of the choices.\n";
  return false;
}

bool Init::confirm(const std::string& question, bool& out) {
  output_ << question << " [y/N]: ";
  output_.flush();

  std::string line;
  if (!std::getline(input_, line)) {
    output_ << "\n";
    output_ << "The input ended before the question was answered.\n";
    return false;
  }
  line = trimmed(line);
  out = !line.empty() && (line[0] == 'y' || line[0] == 'Y');
  return true;
}

Init::CollectOutcome Init::collect(InitAnswers& answers) {
  const CollectOutcome location = collectLocation(answers);
  if (location != CollectOutcome::Ready) {
    return location;
  }
  return collectSettings(answers);
}

// Asks where the configuration goes and whether an existing file is replaced.
Init::CollectOutcome Init::collectLocation(InitAnswers& answers) {
  std::vector<std::string> locations;
  locations.push_back("Project local: " + projectConfigPath());
  const std::string userPath = userConfigPath();
  if (!userPath.empty()) {
    locations.push_back("User level: " + userPath);
  }

  std::size_t chosen = 0;
  if (!askChoice("Where should the configuration go?", locations, chosen)) {
    return CollectOutcome::Failed;
  }
  answers.config_path = chosen == 0 || userPath.empty() ? projectConfigPath() : userPath;

  // An existing configuration is the user's work, so the question comes before
  // the rest and a refusal stops the wizard.
  if (!exists(answers.config_path)) {
    return CollectOutcome::Ready;
  }
  output_ << "The file " << answers.config_path << " already exists.\n";
  bool overwrite = false;
  if (!confirm("Overwrite it?", overwrite)) {
    return CollectOutcome::Failed;
  }
  if (!overwrite) {
    output_ << "Kept " << answers.config_path << ".\n";
    return CollectOutcome::KeptExisting;
  }
  return CollectOutcome::Ready;
}

// Asks for the endpoint, the model, the workspace and the certificate settings.
Init::CollectOutcome Init::collectSettings(InitAnswers& answers) {
  if (!ask("Endpoint, for example https://api.openai.com/v1", "", answers.endpoint)) {
    return CollectOutcome::Failed;
  }
  if (answers.endpoint.empty()) {
    output_ << "The endpoint is required.\n";
    return CollectOutcome::Failed;
  }
  if (!ask("Model id", kDefaultModel, answers.model)) {
    return CollectOutcome::Failed;
  }
  if (!ask("Environment variable holding the API key", kDefaultApiKeyEnv, answers.api_key_env)) {
    return CollectOutcome::Failed;
  }

  std::string workspace = platform::currentWorkingDirectory();
  if (workspace.empty()) {
    workspace = ".";
  }
  if (!ask("Workspace directory", workspace, answers.workspace)) {
    return CollectOutcome::Failed;
  }

  std::string selfSigned = "n";
  if (!ask("Accept self-signed certificates? (y/N)", "n", selfSigned)) {
    return CollectOutcome::Failed;
  }
  answers.ignore_self_signed = !selfSigned.empty() && (selfSigned[0] == 'y' || selfSigned[0] == 'Y');

  if (!ask("Hostname for the certificate check, empty for the endpoint host", "", answers.hostname)) {
    return CollectOutcome::Failed;
  }
  return CollectOutcome::Ready;
}

bool Init::write(const InitAnswers& answers) {
  const std::string directory = directoryOf(answers.config_path);
  std::error_code code;
  if (!directory.empty()) {
    std::filesystem::create_directories(directory, code);
    if (code) {
      output_ << "Cannot create " << directory << ": " << code.message() << "\n";
      return false;
    }
  }

  const std::string promptDirectory = joinPath(directory, kPromptDirectory);
  std::filesystem::create_directories(promptDirectory, code);
  if (code) {
    output_ << "Cannot create " << promptDirectory << ": " << code.message() << "\n";
    return false;
  }

  if (!writeFile(answers.config_path, configText(answers))) {
    output_ << "Cannot write " << answers.config_path << "\n";
    return false;
  }
  output_ << "Wrote " << answers.config_path << "\n";

  // The prompt files are the ones the user customizes, so an existing file is
  // kept rather than replaced.
  const std::string compression = joinPath(promptDirectory, kCompressionPromptFile);
  if (exists(compression)) {
    output_ << "Kept " << compression << "\n";
  } else if (writeFile(compression, config::defaultCompressionPrompt())) {
    output_ << "Wrote " << compression << "\n";
  } else {
    output_ << "Cannot write " << compression << "\n";
    return false;
  }

  const std::string system = joinPath(promptDirectory, kSystemPromptFile);
  if (exists(system)) {
    output_ << "Kept " << system << "\n";
  } else if (writeFile(system, std::string(config::kDefaultSystemPrompt) + "\n")) {
    output_ << "Wrote " << system << "\n";
  } else {
    output_ << "Cannot write " << system << "\n";
    return false;
  }

  const std::string environment = joinPath(directory, kEnvironmentFile);
  if (exists(environment)) {
    output_ << "Kept " << environment << "\n";
  } else if (writeFile(environment, answers.api_key_env + "=\n")) {
    output_ << "Wrote " << environment << "\n";
  } else {
    output_ << "Cannot write " << environment << "\n";
    return false;
  }

  output_ << "\nPut the API key in the environment and run a task:\n";
  output_ << "  export " << answers.api_key_env << "=sk-...\n";
  output_ << "  bjorne_agent \"hello\"\n";
  return true;
}

bool Init::writeFile(const std::string& path, const std::string& content) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      return false;
    }
    file << content;
    if (!file.good()) {
      file.close();
      std::error_code code;
      std::filesystem::remove(temporary, code);
      return false;
    }
  }
  if (!platform::replaceFile(temporary, path)) {
    std::error_code code;
    std::filesystem::remove(temporary, code);
    return false;
  }
  return true;
}

}  // namespace app
}  // namespace bjorne_agent
