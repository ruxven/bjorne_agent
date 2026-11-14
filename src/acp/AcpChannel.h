#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "interaction/Interaction.h"
#include "jsonrpc/Endpoint.h"

namespace bjorne_agent {
namespace acp {

// One choice the editor offers when it is asked for permission.
struct PermissionChoice {
  // The id the protocol carries. It comes back with the answer.
  std::string id;
  // The kind the protocol carries, which is what the editor acts on.
  std::string kind;
  // The label the user reads.
  std::string label;
  // True when the choice allows the tool to run.
  bool granted = false;
  // True when the choice also allows the tool for the rest of the session.
  bool remember = false;
};

// The choices on offer. They are defined once, so the question, the protocol
// options and the reading of the answer cannot drift apart.
const std::vector<PermissionChoice>& permissionChoices();

// The question in the constrained choice shape of the interaction channel: a
// question and the answers it accepts.
interaction::Request permissionRequest(const std::string& tool_name);

// The choice a label stands for. A label that is not on offer grants nothing.
PermissionChoice permissionChoiceForLabel(const std::string& label);

// Asks the editor for permission. It is the interaction channel of the ACP
// mode: the same seam a console or another human answer travels through, with
// the editor behind it.
class AcpChannel final : public interaction::Channel {
 public:
  AcpChannel(std::shared_ptr<jsonrpc::Endpoint> endpoint, std::string session_id, std::string tool_call_id,
             std::function<bool()> cancelled);

  bool available() const override { return true; }

  // Asks the editor and returns the label of the chosen option. Returns false
  // with a reason when the editor cancelled, refused, or could not be reached.
  bool ask(const interaction::Request& request, std::string& answer, std::string& error) override;

 private:
  std::shared_ptr<jsonrpc::Endpoint> endpoint_;
  std::string session_id_;
  std::string tool_call_id_;
  std::function<bool()> cancelled_;
};

}  // namespace acp
}  // namespace bjorne_agent
