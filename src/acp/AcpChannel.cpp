#include "acp/AcpChannel.h"

#include <utility>

#include "json/Json.h"

namespace bjorne_agent {
namespace acp {

namespace {

// How long the editor has to answer a permission request.
constexpr int kPermissionTimeoutMs = 300000;

json::Value objectValue() {
  json::Value value;
  value.type = json::Value::Type::Object;
  return value;
}

json::Value stringValue(const std::string& text) {
  json::Value value;
  value.type = json::Value::Type::String;
  value.string = text;
  return value;
}

json::Value arrayValue() {
  json::Value value;
  value.type = json::Value::Type::Array;
  return value;
}

// The options in the shape the protocol carries them in. The label is looked
// up rather than indexed, so the two lists cannot drift apart.
json::Value permissionOptions(const interaction::Request& request) {
  json::Value options = arrayValue();
  for (const std::string& label : request.options) {
    for (const PermissionChoice& choice : permissionChoices()) {
      if (choice.label != label) {
        continue;
      }
      json::Value option = objectValue();
      option.object["optionId"] = stringValue(choice.id);
      option.object["name"] = stringValue(choice.label);
      option.object["kind"] = stringValue(choice.kind);
      options.array.push_back(std::move(option));
      break;
    }
  }
  return options;
}

}  // namespace

const std::vector<PermissionChoice>& permissionChoices() {
  static const std::vector<PermissionChoice> choices = {
      {"allow_once", "allow_once", "Allow once", true, false},
      {"allow_always", "allow_always", "Allow always", true, true},
      {"reject_once", "reject_once", "Reject once", false, false},
      {"reject_always", "reject_always", "Reject always", false, false},
  };
  return choices;
}

interaction::Request permissionRequest(const std::string& tool_name) {
  interaction::Request request;
  request.question = "Allow " + tool_name + " to run?";
  for (const PermissionChoice& choice : permissionChoices()) {
    request.options.push_back(choice.label);
  }
  return request;
}

PermissionChoice permissionChoiceForLabel(const std::string& label) {
  for (const PermissionChoice& choice : permissionChoices()) {
    if (choice.label == label) {
      return choice;
    }
  }
  return PermissionChoice{};
}

AcpChannel::AcpChannel(std::shared_ptr<jsonrpc::Endpoint> endpoint, std::string session_id, std::string tool_call_id,
                         std::function<bool()> cancelled)
    : endpoint_(std::move(endpoint)),
      session_id_(std::move(session_id)),
      tool_call_id_(std::move(tool_call_id)),
      cancelled_(std::move(cancelled)) {}

bool AcpChannel::ask(const interaction::Request& request, std::string& answer, std::string& error) {
  json::Value toolCall = objectValue();
  toolCall.object["toolCallId"] = stringValue(tool_call_id_);
  toolCall.object["title"] = stringValue(request.question);

  json::Value params = objectValue();
  params.object["sessionId"] = stringValue(session_id_);
  params.object["toolCall"] = std::move(toolCall);
  params.object["options"] = permissionOptions(request);

  json::Value result;
  jsonrpc::RequestOptions options;
  options.timeout_ms = kPermissionTimeoutMs;
  options.abort = [this]() { return cancelled_ && cancelled_(); };
  if (!endpoint_->request("session/request_permission", params, result, error, options)) {
    if (cancelled_ && cancelled_()) {
      error = "The turn was cancelled";
    } else {
      error = "The permission request failed: " + error;
    }
    return false;
  }

  const json::Value* outcome = result.find("outcome");
  if (outcome == nullptr || !outcome->isObject()) {
    error = "The permission response has no outcome";
    return false;
  }
  const json::Value* kind = outcome->find("outcome");
  if (kind == nullptr || !kind->isString()) {
    error = "The permission response has no outcome kind";
    return false;
  }
  if (kind->string == "cancelled") {
    error = "The user cancelled the request";
    return false;
  }
  if (kind->string != "selected") {
    error = "The permission outcome is not understood";
    return false;
  }
  const json::Value* optionId = outcome->find("optionId");
  if (optionId == nullptr || !optionId->isString()) {
    error = "The permission response names no option";
    return false;
  }

  for (const PermissionChoice& choice : permissionChoices()) {
    if (choice.id == optionId->string) {
      answer = choice.label;
      return true;
    }
  }
  error = "The permission outcome names an option that is not on offer";
  return false;
}

}  // namespace acp
}  // namespace bjorne_agent
