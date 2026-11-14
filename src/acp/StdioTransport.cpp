#include "acp/StdioTransport.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)

#include <io.h>
#include <windows.h>

#else

#include <cerrno>
#include <poll.h>
#include <unistd.h>

#endif

namespace bjorne_agent {
namespace acp {

namespace {

constexpr std::size_t kChunkSize = 8192;

}  // namespace

StdioTransport::StdioTransport() {
#if defined(_WIN32)
  input_ = ::_fileno(stdin);
  output_ = ::_fileno(stdout);
#else
  input_ = STDIN_FILENO;
  output_ = STDOUT_FILENO;
#endif
  standard_output_ = true;
}

StdioTransport::StdioTransport(int input_descriptor, int output_descriptor)
    : input_(input_descriptor), output_(output_descriptor), standard_output_(false) {}

bool StdioTransport::sendLine(const std::string& line, std::string& error) {
  const std::string payload = line + "\n";
#if defined(_WIN32)
  const int written = ::_write(output_, payload.data(), static_cast<unsigned int>(payload.size()));
#else
  const ssize_t written = ::write(output_, payload.data(), payload.size());
#endif
  if (written < 0 || static_cast<std::size_t>(written) != payload.size()) {
    error = "Cannot write to standard output";
    return false;
  }
  // The editor waits for the message before it sends the next one, so the
  // write has to reach it before the call returns.
  if (standard_output_ && std::fflush(stdout) != 0) {
    error = "Cannot flush standard output";
    return false;
  }
  return true;
}

bool StdioTransport::waitForInput(int timeout_ms) {
#if defined(_WIN32)
  const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(_get_osfhandle(input_)));
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
    return false;
  }
  const DWORD status = ::WaitForSingleObject(handle, static_cast<DWORD>(timeout_ms));
  return status == WAIT_OBJECT_0;
#else
  struct pollfd descriptor {};
  descriptor.fd = input_;
  descriptor.events = POLLIN;
  const int status = ::poll(&descriptor, 1, timeout_ms);
  if (status <= 0) {
    return false;
  }
  // A hangup on the pipe means the editor closed the connection. Treating it
  // as ready lets the read report the end of the stream.
  return (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
#endif
}

bool StdioTransport::readChunk(std::string& error) {
  char chunk[kChunkSize];
#if defined(_WIN32)
  const int read = ::_read(input_, chunk, static_cast<unsigned int>(sizeof(chunk)));
#else
  const ssize_t read = ::read(input_, chunk, sizeof(chunk));
#endif
  if (read < 0) {
#if !defined(_WIN32)
    if (errno == EINTR || errno == EAGAIN) {
      return true;
    }
#endif
    error = "Cannot read from standard input";
    return false;
  }
  if (read == 0) {
    error = "Standard input closed";
    return false;
  }
  buffer_.append(chunk, static_cast<std::size_t>(read));
  return true;
}

jsonrpc::ReadResult StdioTransport::readLine(int timeout_ms, std::string& line, std::string& error) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const std::size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      // A peer on either platform may end a line with a newline pair.
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return jsonrpc::ReadResult::Line;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return jsonrpc::ReadResult::Timeout;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (!waitForInput(static_cast<int>(remaining > 0 ? remaining : 1))) {
      return jsonrpc::ReadResult::Timeout;
    }

    if (!readChunk(error)) {
      // The stream ended. What is left in the buffer was the last line, and a
      // peer that closes without a newline still means it.
      if (!buffer_.empty()) {
        line = buffer_;
        buffer_.clear();
        return jsonrpc::ReadResult::Line;
      }
      return jsonrpc::ReadResult::Closed;
    }
  }
}

}  // namespace acp
}  // namespace bjorne_agent
