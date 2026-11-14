#include "platform/Process.h"

#include <string>

#if defined(_WIN32)

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

namespace bjorne_agent {
namespace platform {

namespace {

// Windows quoting: an argument is wrapped in quotes when it is empty or holds
// white space or a quote, and a backslash before a quote is doubled.
std::string quoteArgument(const std::string& argument) {
  if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos) {
    return argument;
  }
  std::string quoted = "\"";
  for (std::size_t index = 0; index < argument.size(); ++index) {
    std::size_t backslashes = 0;
    while (index < argument.size() && argument[index] == '\\') {
      ++backslashes;
      ++index;
    }
    if (index == argument.size()) {
      quoted.append(backslashes * 2, '\\');
      break;
    }
    if (argument[index] == '"') {
      quoted.append(backslashes * 2 + 1, '\\');
      quoted.push_back('"');
    } else {
      quoted.append(backslashes, '\\');
      quoted.push_back(argument[index]);
    }
  }
  quoted.push_back('"');
  return quoted;
}

std::string environmentBlock(const std::map<std::string, std::string>& extra) {
  std::map<std::string, std::string> entries;
  if (char* block = ::GetEnvironmentStringsA()) {
    for (const char* entry = block; *entry != '\0'; entry += std::strlen(entry) + 1) {
      const std::string text(entry);
      if (!text.empty() && text.front() == '=') {
        // A hidden drive entry such as =C:=C:\ has no separate name; keep it
        // verbatim so the child process sees the same drives.
        entries[text] = text;
        continue;
      }
      const std::size_t separator = text.find('=');
      if (separator != std::string::npos) {
        entries[text.substr(0, separator)] = text;
      }
    }
    ::FreeEnvironmentStringsA(block);
  }
  for (const auto& [key, value] : extra) {
    entries[key] = key + "=" + value;
  }

  std::string block;
  for (const auto& [key, entry] : entries) {
    (void)key;
    block.append(entry);
    block.push_back('\0');
  }
  block.push_back('\0');
  return block;
}

}  // namespace

ChildProcess::~ChildProcess() { stop(); }

bool ChildProcess::start(const std::string& command, const std::vector<std::string>& arguments,
                         const std::map<std::string, std::string>& environment, std::string& error) {
  if (started_) {
    error = "The process has already been started";
    return false;
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE stdin_read = nullptr;
  HANDLE stdin_write = nullptr;
  HANDLE stdout_read = nullptr;
  HANDLE stdout_write = nullptr;
  if (::CreatePipe(&stdin_read, &stdin_write, &attributes, 0) == 0 ||
      ::CreatePipe(&stdout_read, &stdout_write, &attributes, 0) == 0) {
    error = "Cannot create the server pipes";
    if (stdin_read != nullptr) ::CloseHandle(stdin_read);
    if (stdin_write != nullptr) ::CloseHandle(stdin_write);
    if (stdout_read != nullptr) ::CloseHandle(stdout_read);
    if (stdout_write != nullptr) ::CloseHandle(stdout_write);
    return false;
  }
  ::SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

  std::string command_line = quoteArgument(command);
  for (const std::string& argument : arguments) {
    command_line.push_back(' ');
    command_line.append(quoteArgument(argument));
  }
  const std::string block = environmentBlock(environment);

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = stdin_read;
  startup.hStdOutput = stdout_write;
  startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION process{};
  const BOOL created = ::CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        block.empty() ? nullptr : const_cast<char*>(block.data()), nullptr,
                                        &startup, &process);
  ::CloseHandle(stdin_read);
  ::CloseHandle(stdout_write);
  if (created == 0) {
    error = "Cannot start MCP server: " + command;
    ::CloseHandle(stdin_write);
    ::CloseHandle(stdout_read);
    return false;
  }

  ::CloseHandle(process.hThread);
  process_ = process.hProcess;
  stdin_write_ = stdin_write;
  stdout_read_ = stdout_read;
  started_ = true;
  exited_ = false;
  return true;
}

bool ChildProcess::running() {
  if (!started_ || exited_) {
    return false;
  }
  if (::WaitForSingleObject(static_cast<HANDLE>(process_), 0) != WAIT_OBJECT_0) {
    return true;
  }
  ::GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code_);
  exited_ = true;
  return false;
}

bool ChildProcess::sendLine(const std::string& line, std::string& error) {
  if (!started_ || stdin_write_ == nullptr) {
    error = "The server process is not running";
    return false;
  }
  const std::string payload = line + "\n";
  DWORD written = 0;
  if (::WriteFile(static_cast<HANDLE>(stdin_write_), payload.data(), static_cast<DWORD>(payload.size()), &written,
                  0) == 0 ||
      written != payload.size()) {
    error = "Cannot write to the server process";
    return false;
  }
  return true;
}

ReadStatus ChildProcess::readLine(int timeout_ms, std::string& line, std::string& error) {
  (void)error;
  const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  while (true) {
    if (extractLine(line)) {
      return ReadStatus::Line;
    }

    if (!started_ || stdout_read_ == nullptr) {
      return ReadStatus::Closed;
    }
    DWORD available = 0;
    if (::PeekNamedPipe(static_cast<HANDLE>(stdout_read_), nullptr, 0, nullptr, &available, nullptr) == 0) {
      return ReadStatus::Error;
    }
    if (available > 0) {
      char chunk[4096];
      DWORD read = 0;
      if (::ReadFile(static_cast<HANDLE>(stdout_read_), chunk, sizeof(chunk), &read, nullptr) == 0) {
        return ReadStatus::Closed;
      }
      buffer_.append(chunk, read);
      if (buffer_.size() > 16 * 1024 * 1024) {
        return ReadStatus::Error;
      }
      continue;
    }
    if (!running()) {
      return ReadStatus::Closed;
    }
    if (::GetTickCount64() >= deadline) {
      return ReadStatus::Timeout;
    }
    ::Sleep(5);
  }
}

void ChildProcess::stop() {
  if (!started_) {
    return;
  }
  if (stdin_write_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stdin_write_));
    stdin_write_ = nullptr;
  }
  if (!exited_ && process_ != nullptr) {
    if (::WaitForSingleObject(static_cast<HANDLE>(process_), 200) != WAIT_OBJECT_0) {
      ::TerminateProcess(static_cast<HANDLE>(process_), 1);
      ::WaitForSingleObject(static_cast<HANDLE>(process_), 1000);
    }
    ::GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code_);
    exited_ = true;
  }
  if (stdout_read_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stdout_read_));
    stdout_read_ = nullptr;
  }
  if (process_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
  started_ = false;
}

int ChildProcess::exitCode() { return exited_ ? static_cast<int>(exit_code_) : -1; }

}  // namespace platform
}  // namespace bjorne_agent

#else

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bjorne_agent {
namespace platform {

namespace {

constexpr std::size_t kMaxBuffer = 16 * 1024 * 1024;

void closeFd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

bool setCloseOnExec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

// Copies the inherited environment, then applies the entries the config adds
// or overrides. The child gets the merged block, so a server sees PATH and the
// caller's variables plus its own.
std::vector<std::string> mergedEnvironment(const std::map<std::string, std::string>& extra) {
  std::vector<std::string> entries;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    entries.emplace_back(*entry);
  }
  for (const auto& [key, value] : extra) {
    const std::string prefix = key + "=";
    bool replaced = false;
    for (std::string& entry : entries) {
      if (entry.compare(0, prefix.size(), prefix) == 0) {
        entry = prefix + value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      entries.push_back(prefix + value);
    }
  }
  return entries;
}

void closeSpawnPipes(int stdin_pipe[2], int stdout_pipe[2]) {
  closeFd(stdin_pipe[0]);
  closeFd(stdin_pipe[1]);
  closeFd(stdout_pipe[0]);
  closeFd(stdout_pipe[1]);
}

}  // namespace

ChildProcess::~ChildProcess() { stop(); }

bool ChildProcess::start(const std::string& command, const std::vector<std::string>& arguments,
                         const std::map<std::string, std::string>& environment, std::string& error) {
  if (started_) {
    error = "The process has already been started";
    return false;
  }

  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0) {
    error = std::string("Cannot create pipes: ") + std::strerror(errno);
    closeSpawnPipes(stdin_pipe, stdout_pipe);
    return false;
  }
  for (int fd : {stdin_pipe[0], stdin_pipe[1], stdout_pipe[0], stdout_pipe[1]}) {
    if (!setCloseOnExec(fd)) {
      error = std::string("Cannot configure a pipe: ") + std::strerror(errno);
      closeSpawnPipes(stdin_pipe, stdout_pipe);
      return false;
    }
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);

  std::vector<std::string> argv_storage;
  argv_storage.push_back(command);
  argv_storage.insert(argv_storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (std::string& argument : argv_storage) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  std::vector<std::string> environment_storage = mergedEnvironment(environment);
  std::vector<char*> envp;
  envp.reserve(environment_storage.size() + 1);
  for (std::string& entry : environment_storage) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  pid_t pid = -1;
  const int spawned = ::posix_spawnp(&pid, command.c_str(), &actions, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  closeFd(stdin_pipe[0]);
  closeFd(stdout_pipe[1]);
  if (spawned != 0) {
    error = "Cannot start MCP server: " + command + ": " + std::strerror(spawned);
    closeFd(stdin_pipe[1]);
    closeFd(stdout_pipe[0]);
    return false;
  }

  pid_ = static_cast<int>(pid);
  stdin_fd_ = stdin_pipe[1];
  stdout_fd_ = stdout_pipe[0];
  started_ = true;
  exited_ = false;
  return true;
}

bool ChildProcess::running() {
  if (!started_ || exited_) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (result == 0) {
    return true;
  }
  if (result == static_cast<pid_t>(pid_)) {
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    exited_ = true;
  }
  return false;
}

bool ChildProcess::sendLine(const std::string& line, std::string& error) {
  if (!started_ || stdin_fd_ < 0) {
    error = "The server process is not running";
    return false;
  }
  const std::string payload = line + "\n";
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const ssize_t written = ::write(stdin_fd_, payload.data() + offset, payload.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = std::string("Cannot write to the server process: ") + std::strerror(errno);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

ReadStatus ChildProcess::readLine(int timeout_ms, std::string& line, std::string& error) {
  if (extractLine(line)) {
    return ReadStatus::Line;
  }
  if (!started_ || stdout_fd_ < 0) {
    return ReadStatus::Closed;
  }

  pollfd entry{};
  entry.fd = stdout_fd_;
  entry.events = POLLIN;
  const int ready = ::poll(&entry, 1, timeout_ms);
  if (ready == 0) {
    return ReadStatus::Timeout;
  }
  if (ready < 0) {
    if (errno == EINTR) {
      return ReadStatus::Timeout;
    }
    error = std::string("Cannot read from the server process: ") + std::strerror(errno);
    return ReadStatus::Error;
  }
  return readChunk(line, error);
}

ReadStatus ChildProcess::readChunk(std::string& line, std::string& error) {
  char chunk[4096];
  const ssize_t count = ::read(stdout_fd_, chunk, sizeof(chunk));
  if (count < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      return ReadStatus::Timeout;
    }
    error = std::string("Cannot read from the server process: ") + std::strerror(errno);
    return ReadStatus::Error;
  }
  if (count == 0) {
    return ReadStatus::Closed;
  }
  buffer_.append(chunk, static_cast<std::size_t>(count));
  if (buffer_.size() > kMaxBuffer) {
    error = "The server sent a line larger than the limit.";
    return ReadStatus::Error;
  }
  if (extractLine(line)) {
    return ReadStatus::Line;
  }
  // A partial line arrived. Report a timeout so the caller keeps the buffer
  // and tries again, rather than waiting for a newline that may be far away.
  return ReadStatus::Timeout;
}

void ChildProcess::stop() {
  if (!started_) {
    return;
  }
  closeFd(stdin_fd_);
  if (!running()) {
    closeFd(stdout_fd_);
    started_ = false;
    return;
  }

  ::kill(static_cast<pid_t>(pid_), SIGTERM);
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!running()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!exited_) {
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    exited_ = true;
  }
  closeFd(stdout_fd_);
  started_ = false;
}

int ChildProcess::exitCode() { return exited_ ? exit_code_ : -1; }

}  // namespace platform
}  // namespace bjorne_agent

#endif

namespace bjorne_agent {
namespace platform {

// The framing is the same on both platforms: one message per line, and a peer
// may end the line with a newline pair.
bool ChildProcess::extractLine(std::string& line) {
  const std::size_t newline = buffer_.find('\n');
  if (newline == std::string::npos) {
    return false;
  }
  line = buffer_.substr(0, newline);
  buffer_.erase(0, newline + 1);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return true;
}

}  // namespace platform
}  // namespace bjorne_agent
