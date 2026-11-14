#pragma once

#include <string>

#include "jsonrpc/Endpoint.h"

namespace bjorne_agent {
namespace acp {

// The ACP transport: newline delimited JSON-RPC over standard input and
// standard output. Standard error is left alone, so the log lines the agent
// writes never reach the editor as protocol messages.
//
// The reader keeps whatever it has already read, because a message may arrive
// in pieces. The write side is synchronous, so a message reaches the editor
// before the call returns.
//
// The descriptors are injectable so a test can drive the transport over a
// pipe. The default is standard input and standard output.
class StdioTransport final : public jsonrpc::Transport {
 public:
  StdioTransport();
  StdioTransport(int input_descriptor, int output_descriptor);

  bool sendLine(const std::string& line, std::string& error) override;

  jsonrpc::ReadResult readLine(int timeout_ms, std::string& line, std::string& error) override;

 private:
  // Waits up to timeout_ms for input to arrive and appends it to the buffer.
  // Returns false when the wait timed out.
  bool waitForInput(int timeout_ms);

  // Takes the bytes that are ready from the input. Returns false with a
  // reason when the stream failed.
  bool readChunk(std::string& error);

  int input_ = -1;
  int output_ = -1;
  // True when the output is standard output, which needs a flush after a
  // write because the C library may have buffered it.
  bool standard_output_ = true;
  std::string buffer_;
};

}  // namespace acp
}  // namespace bjorne_agent
