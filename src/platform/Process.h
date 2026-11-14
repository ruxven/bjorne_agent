#pragma once

#include <map>
#include <string>
#include <vector>

namespace bjorne_agent {
namespace platform {

// The result of reading one newline delimited line from a child process.
enum class ReadStatus { Line, Timeout, Closed, Error };

// A child process with pipes on standard input and standard output. Standard
// error is inherited, so a server's own log lines reach the agent's stderr and
// never fill a pipe nobody drains. The class is the process half of the
// JSON-RPC transport and knows nothing about JSON.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // Starts command with its arguments and the inherited environment plus the
  // entries in environment. Returns false with a reason when the process or a
  // pipe cannot be created. The child is not waited for here.
  bool start(const std::string& command, const std::vector<std::string>& arguments,
             const std::map<std::string, std::string>& environment, std::string& error);

  // True while the process has not exited. Reaps a process that has.
  bool running();

  // Writes one line and its newline. Returns false when the pipe is closed.
  bool sendLine(const std::string& line, std::string& error);

  // Reads one line, which may already be buffered. A partial line is kept for
  // the next call. Returns Timeout when no complete line arrives in time.
  ReadStatus readLine(int timeout_ms, std::string& line, std::string& error);

  // Removes the first complete line from the buffer. A trailing carriage
  // return is dropped, because a peer on either platform may end a line with
  // a newline pair.
  bool extractLine(std::string& line);

  // Closes the pipes and stops the process: SIGTERM or TerminateProcess first,
  // then a forced kill when it does not exit promptly.
  void stop();

  // The exit code once the process has exited, or -1 while it runs.
  int exitCode();

  bool started() const { return started_; }

 private:
#if defined(_WIN32)
  void* process_ = nullptr;
  void* stdin_write_ = nullptr;
  void* stdout_read_ = nullptr;
  unsigned long exit_code_ = 0;
#else
  int pid_ = -1;
  int stdin_fd_ = -1;
  int stdout_fd_ = -1;
  int exit_code_ = -1;

  // Reads whatever the pipe holds and turns it into a line when one is
  // complete.
  ReadStatus readChunk(std::string& line, std::string& error);
#endif
  bool started_ = false;
  bool exited_ = false;
  std::string buffer_;
};

}  // namespace platform
}  // namespace bjorne_agent
