#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "json/Json.h"
#include "jsonrpc/JsonRpc.h"

namespace bjorne_agent {
namespace jsonrpc {

// The result of asking a transport for one line.
enum class ReadResult { Line, Timeout, Closed, Error };

// A newline delimited stream of JSON-RPC messages. The MCP client adapts a
// child process to this interface, and the ACP server will adapt standard
// input and output. Tests substitute an in-memory pair.
class Transport {
 public:
  virtual ~Transport() = default;

  // Writes one message and terminates it with a newline.
  virtual bool sendLine(const std::string& line, std::string& error) = 0;

  // Reads one complete line. Returns Timeout when no line arrived in time,
  // so the reader can notice shutdown without a blocking read that never ends.
  virtual ReadResult readLine(int timeout_ms, std::string& line, std::string& error) = 0;
};

// Answers a request from the peer. The handler produces the result or sets the
// error. An empty handler makes every request fail with method-not-found.
using RequestHandler =
    std::function<bool(const std::string& method, const json::Value& params, json::Value& result, Error& error)>;

// Receives a notification from the peer. The default ignores them.
using NotificationHandler = std::function<void(const std::string& method, const json::Value& params)>;

// One JSON-RPC endpoint over a transport. A reader thread keeps the pipe
// drained, responses are matched to pending requests by id, and a write mutex
// keeps two threads from interleaving their lines. The same component serves
// the client role towards an MCP server and the server role towards an editor.
class Endpoint {
 public:
  explicit Endpoint(std::shared_ptr<Transport> transport);
  ~Endpoint();

  Endpoint(const Endpoint&) = delete;
  Endpoint& operator=(const Endpoint&) = delete;

  void setRequestHandler(RequestHandler handler);
  void setNotificationHandler(NotificationHandler handler);

  // Starts the reader thread. Calling it twice does nothing.
  void start();

  // Sends a request and waits for the matching response. The default timeout
  // keeps a wedged peer from hanging the agent forever.
  bool request(const std::string& method, const json::Value& params, json::Value& result, std::string& error,
               int timeout_ms = 30000);

  bool notify(const std::string& method, const json::Value& params, std::string& error);

  // True once the peer closed the connection or the transport failed.
  bool closed() const;

  // Human readable reason for the close, for a tool error or a log line.
  std::string closeReason() const;

 private:
  struct Pending {
    bool done = false;
    bool ok = false;
    json::Value result;
    Error error;
  };

  void readLoop();
  void dispatch(const Message& message);
  void failPending(const std::string& reason);
  void markClosed(const std::string& reason);

  std::shared_ptr<Transport> transport_;
  RequestHandler request_handler_;
  NotificationHandler notification_handler_;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::map<std::string, Pending> pending_;
  long long next_id_ = 1;
  bool closed_ = false;
  std::string close_reason_;
  bool stopping_ = false;
  bool started_ = false;
  std::thread reader_;

  std::mutex write_mutex_;
};

}  // namespace jsonrpc
}  // namespace bjorne_agent
