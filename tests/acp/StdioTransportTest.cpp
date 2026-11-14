#include "acp/StdioTransport.h"

#include <string>

#include <catch_amalgamated.hpp>

#if defined(_WIN32)

#include <io.h>

#else

#include <unistd.h>

#endif

using bjorne_agent::acp::StdioTransport;
using bjorne_agent::jsonrpc::ReadResult;

namespace {

// A pair of pipes standing in for the editor connection. The test writes into
// the input pipe and reads out of the output pipe.
class PipePair {
 public:
  PipePair() {
#if defined(_WIN32)
    _pipe(input_, sizeof(input_), 0);
    _pipe(output_, sizeof(output_), 0);
#else
    ::pipe(input_);
    ::pipe(output_);
#endif
    // The transport reads from the first end of the input pipe and writes to
    // the second end of the output pipe.
    input_read_ = input_[0];
    input_write_ = input_[1];
    output_read_ = output_[0];
    output_write_ = output_[1];
  }

  ~PipePair() {
    closeInput();
#if defined(_WIN32)
    ::_close(input_read_);
    ::_close(output_read_);
    ::_close(output_write_);
#else
    ::close(input_read_);
    ::close(output_read_);
    ::close(output_write_);
#endif
  }

  void write(const std::string& text) {
#if defined(_WIN32)
    const int written = ::_write(input_write_, text.data(), static_cast<unsigned int>(text.size()));
#else
    const ssize_t written = ::write(input_write_, text.data(), text.size());
#endif
    (void)written;
  }

  // Closes the writing end, so the transport sees the end of the stream.
  void closeInput() {
#if defined(_WIN32)
    ::_close(input_write_);
#else
    ::close(input_write_);
#endif
    input_write_ = -1;
  }

  std::string read() {
    std::string text;
    char chunk[256];
    for (;;) {
#if defined(_WIN32)
      const int got = ::_read(output_read_, chunk, static_cast<unsigned int>(sizeof(chunk)));
#else
      const ssize_t got = ::read(output_read_, chunk, sizeof(chunk));
#endif
      if (got <= 0) {
        break;
      }
      text.append(chunk, static_cast<std::size_t>(got));
      if (text.find('\n') != std::string::npos) {
        break;
      }
    }
    return text;
  }

  int input_read_ = -1;
  int input_write_ = -1;
  int output_read_ = -1;
  int output_write_ = -1;

 private:
#if defined(_WIN32)
  int input_[2] = {-1, -1};
  int output_[2] = {-1, -1};
#else
  int input_[2] = {-1, -1};
  int output_[2] = {-1, -1};
#endif
};

}  // namespace

TEST_CASE("the transport writes a line to its output") {
  PipePair pipes;
  StdioTransport transport(pipes.input_read_, pipes.output_write_);

  std::string error;
  REQUIRE(transport.sendLine(R"({"jsonrpc":"2.0"})", error));
  REQUIRE(error.empty());
  REQUIRE(pipes.read() == "{\"jsonrpc\":\"2.0\"}\n");
}

TEST_CASE("the transport reads a line from its input") {
  PipePair pipes;
  StdioTransport transport(pipes.input_read_, pipes.output_write_);

  pipes.write("first\nsecond\n");
  std::string line;
  std::string error;
  REQUIRE(transport.readLine(1000, line, error) == ReadResult::Line);
  REQUIRE(line == "first");
  REQUIRE(transport.readLine(1000, line, error) == ReadResult::Line);
  REQUIRE(line == "second");
}

TEST_CASE("a message that arrives in pieces is joined") {
  PipePair pipes;
  StdioTransport transport(pipes.input_read_, pipes.output_write_);

  pipes.write("{\"a\":");
  std::string line;
  std::string error;
  REQUIRE(transport.readLine(50, line, error) == ReadResult::Timeout);
  pipes.write("1}\n");
  REQUIRE(transport.readLine(1000, line, error) == ReadResult::Line);
  REQUIRE(line == "{\"a\":1}");
}

TEST_CASE("a line that ends with a newline pair loses the carriage return") {
  PipePair pipes;
  StdioTransport transport(pipes.input_read_, pipes.output_write_);

  pipes.write("line\r\n");
  std::string line;
  std::string error;
  REQUIRE(transport.readLine(1000, line, error) == ReadResult::Line);
  REQUIRE(line == "line");
}

TEST_CASE("a closed input ends the stream") {
  PipePair pipes;
  StdioTransport transport(pipes.input_read_, pipes.output_write_);

  pipes.write("{\"a\":1}\n");
  std::string line;
  std::string error;
  REQUIRE(transport.readLine(1000, line, error) == ReadResult::Line);
  REQUIRE(line == "{\"a\":1}");

  pipes.closeInput();
  REQUIRE(transport.readLine(1000, line, error) == ReadResult::Closed);
}

TEST_CASE("the default transport uses standard input and output") {
  // The transport only records the descriptors here. Nothing is read or
  // written, so the test does not disturb the test runner's own streams.
  StdioTransport transport;
  std::string error;
  REQUIRE(transport.sendLine("", error));
  REQUIRE(error.empty());
}

TEST_CASE("a write to a closed descriptor fails") {
  PipePair pipes;
  pipes.closeInput();
#if defined(_WIN32)
  StdioTransport transport(pipes.input_read_, -1);
#else
  StdioTransport transport(pipes.input_read_, -1);
#endif

  std::string error;
  REQUIRE_FALSE(transport.sendLine(R"({"a":1})", error));
  REQUIRE(error.find("Cannot write") != std::string::npos);
}

TEST_CASE("a read from a closed descriptor fails") {
  PipePair pipes;
  pipes.closeInput();
  StdioTransport transport(pipes.input_read_, pipes.output_write_);

  std::string line;
  std::string error;
  REQUIRE(transport.readLine(1000, line, error) == ReadResult::Closed);
}

TEST_CASE("a read with no time left gives up right away") {
  PipePair pipes;
  StdioTransport transport(pipes.input_read_, pipes.output_write_);

  std::string line;
  std::string error;
  REQUIRE(transport.readLine(0, line, error) == ReadResult::Timeout);
}

TEST_CASE("a last line without a newline is still a line") {
  PipePair pipes;
  StdioTransport transport(pipes.input_read_, pipes.output_write_);

  pipes.write(R"({"a":1})");
  pipes.closeInput();

  std::string line;
  std::string error;
  REQUIRE(transport.readLine(1000, line, error) == ReadResult::Line);
  REQUIRE(line == R"({"a":1})");

  // The stream is over now.
  REQUIRE(transport.readLine(1000, line, error) == ReadResult::Closed);
}
