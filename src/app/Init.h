#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace bjorne_agent {
namespace app {

// The values the init command collects. The wizard fills them in, and the
// writer turns them into the files.
struct InitAnswers {
  // Where the configuration file goes.
  std::string config_path;
  // The model endpoint, the model id and the environment variable that holds
  // the API key. The key itself stays in the environment.
  std::string endpoint;
  std::string model;
  std::string api_key_env;
  // The workspace the tools work in.
  std::string workspace;
  // The TLS settings: whether peer certificates are verified at all, and the
  // name sent in the SNI extension.
  bool ignore_self_signed = false;
  std::string hostname;
};

// Creates the initial configuration. The command replaces the setup script:
// the binary carries the default prompts, so the package holds the executable
// and nothing else, and the wizard writes the editable copies next to the
// configuration.
//
// The questions are asked on a stream, so a test drives the wizard without a
// terminal. The terminal check is a constructor argument for the same reason:
// the tests answer on a string stream.
class Init {
 public:
  Init(std::istream& input, std::ostream& output, bool input_is_terminal);

  // Asks the questions and writes the files. Returns 0 when the files were
  // written, or when the user kept the configuration that was already there,
  // and 1 after an error was reported.
  int run();

 private:
  // Asks a question and stores the answer. An empty line takes the fallback.
  bool ask(const std::string& question, const std::string& fallback, std::string& out);
  // Asks a question with a numbered list of answers and stores the index of
  // the chosen one.
  bool askChoice(const std::string& question, const std::vector<std::string>& options, std::size_t& out);
  // Asks a yes or no question. Anything that does not start with y is no.
  bool confirm(const std::string& question, bool& out);
  // What asking the questions led to.
  enum class CollectOutcome {
    // Every answer was given and the files can be written.
    Ready,
    // The user kept the configuration that was already there.
    KeptExisting,
    // A question could not be answered.
    Failed
  };
  // Asks every question and stores the answers.
  CollectOutcome collect(InitAnswers& answers);
  // Asks where the configuration goes and whether an existing file is replaced.
  CollectOutcome collectLocation(InitAnswers& answers);
  // Asks for the endpoint, the model, the workspace and the certificate settings.
  CollectOutcome collectSettings(InitAnswers& answers);
  // Writes the configuration, the prompt files and the environment file.
  bool write(const InitAnswers& answers);
  // Writes one file, through a temporary file, so a half written file never
  // replaces a good one.
  bool writeFile(const std::string& path, const std::string& content);

  std::istream& input_;
  std::ostream& output_;
  bool input_is_terminal_;
};

}  // namespace app
}  // namespace bjorne_agent
