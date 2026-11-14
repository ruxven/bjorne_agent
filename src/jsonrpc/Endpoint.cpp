#include "jsonrpc/Endpoint.h"

#include <chrono>
#include <string>
#include <utility>

#include "json/Json.h"
#include "jsonrpc/JsonRpc.h"

namespace bjorne_agent {
namespace jsonrpc {

namespace {

constexpr int kPollMilliseconds = 200;
constexpr std::size_t kMaxLineLength = 16 * 1024 * 1024;

bool writeLine(const std::shared_ptr<Transport>& transport, std::mutex& write_mutex, const std::string& line,
               std::string& error) {
  std::lock_guard<std::mutex> guard(write_mutex);
  return transport->sendLine(line, error);
}

}  // namespace

Endpoint::Endpoint(std::shared_ptr<Transport> transport) : transport_(std::move(transport)) {}

Endpoint::~Endpoint() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
  if (reader_.joinable()) {
    reader_.join();
  }
}

void Endpoint::setRequestHandler(RequestHandler handler) { request_handler_ = std::move(handler); }

void Endpoint::setNotificationHandler(NotificationHandler handler) {
  notification_handler_ = std::move(handler);
}

void Endpoint::start() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (started_) {
    return;
  }
  started_ = true;
  reader_ = std::thread(&Endpoint::readLoop, this);
}

bool Endpoint::request(const std::string& method, const json::Value& params, json::Value& result,
                       std::string& error, int timeout_ms) {
  std::string key;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) {
      error = close_reason_;
      return false;
    }
    const json::Value id = numberId(next_id_++);
    key = json::write(id);
    pending_[key] = Pending();

    if (!writeLine(transport_, write_mutex_, encodeRequest(id, method, params), error)) {
      pending_.erase(key);
      return false;
    }
  }

  std::unique_lock<std::mutex> lock(mutex_);
  const bool answered = condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, &key] {
    auto it = pending_.find(key);
    return it == pending_.end() || it->second.done;
  });

  auto entry = pending_.find(key);
  if (entry == pending_.end()) {
    error = closed_ ? close_reason_ : ("The request " + method + " was abandoned");
    return false;
  }
  if (!answered || !entry->second.done) {
    pending_.erase(entry);
    error = "The request " + method + " timed out after " + std::to_string(timeout_ms) + " ms";
    return false;
  }

  const bool ok = entry->second.ok;
  if (ok) {
    result = entry->second.result;
  } else {
    error = entry->second.error.message;
  }
  pending_.erase(entry);
  return ok;
}

bool Endpoint::notify(const std::string& method, const json::Value& params, std::string& error) {
  return writeLine(transport_, write_mutex_, encodeNotification(method, params), error);
}

bool Endpoint::closed() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return closed_;
}

std::string Endpoint::closeReason() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return close_reason_;
}

void Endpoint::readLoop() {
  while (true) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (stopping_) {
        return;
      }
    }

    std::string line;
    std::string error;
    const ReadResult status = transport_->readLine(kPollMilliseconds, line, error);
    if (status == ReadResult::Timeout) {
      continue;
    }
    if (status != ReadResult::Line) {
      markClosed(status == ReadResult::Closed ? "The connection closed." : error);
      return;
    }
    if (line.size() > kMaxLineLength) {
      markClosed("The peer sent a line larger than the limit.");
      return;
    }

    Message message;
    std::string parse_error;
    if (!parse(line, message, parse_error)) {
      // A malformed line does not end the connection: a peer that emits one
      // bad log line should not take a working session down with it.
      continue;
    }
    dispatch(message);
  }
}

void Endpoint::dispatch(const Message& message) {
  if (message.kind == Message::Kind::Response) {
    const std::string key = json::write(message.id);
    {
      std::lock_guard<std::mutex> guard(mutex_);
      auto entry = pending_.find(key);
      if (entry == pending_.end()) {
        return;
      }
      entry->second.done = true;
      entry->second.ok = message.hasResult;
      if (message.hasResult) {
        entry->second.result = message.result;
      } else {
        entry->second.error = message.error;
      }
    }
    condition_.notify_all();
    return;
  }

  if (message.kind == Message::Kind::Notification) {
    if (notification_handler_) {
      notification_handler_(message.method, message.params);
    }
    return;
  }

  if (message.kind != Message::Kind::Request) {
    return;
  }

  json::Value result;
  Error failure;
  if (!request_handler_ || !request_handler_(message.method, message.params, result, failure)) {
    if (failure.message.empty()) {
      failure.code = kMethodNotFound;
      failure.message = "Method not found: " + message.method;
    }
    std::string error;
    writeLine(transport_, write_mutex_, encodeError(message.id, failure.code, failure.message), error);
    return;
  }

  std::string error;
  writeLine(transport_, write_mutex_, encodeResult(message.id, result), error);
}

void Endpoint::failPending(const std::string& reason) {
  for (auto& [key, entry] : pending_) {
    (void)key;
    if (!entry.done) {
      entry.done = true;
      entry.ok = false;
      entry.error.code = kInternalError;
      entry.error.message = reason;
    }
  }
  condition_.notify_all();
}

void Endpoint::markClosed(const std::string& reason) {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
    close_reason_ = reason.empty() ? "The connection closed." : reason;
    failPending(close_reason_);
  }
  condition_.notify_all();
}

}  // namespace jsonrpc
}  // namespace bjorne_agent
