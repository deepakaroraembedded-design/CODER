// cf-orch: event-stream routing for the run process (design section 6.6.1).
//
// Subscribes to each opencode server's event stream and routes the events the
// framework must answer: permission.asked goes to the role's PermissionBroker
// (which replies through its own callback), question.asked goes to the
// question policy (RAG answer or reject-and-record). Everything else is
// ignored here; turn completion is observed by the runner.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "cf/agents/opencode_client.hpp"
#include "cf/agents/permission_broker.hpp"

namespace cf::orch {

struct QuestionOption {
  std::string label;
  std::string description;
};

struct Question {
  std::string text;
  std::vector<QuestionOption> options;
  bool multiple = false;
};

struct QuestionAsked {
  std::string session_id;
  std::string request_id;
  std::vector<Question> questions;
};

struct QuestionDecision {
  bool answer = false;  // false: reject and record the ambiguity
  std::vector<std::vector<std::string>> answers;  // one label list per question
  std::string reason;   // recorded when rejected
};

struct EventRouterHooks {
  // Decides and replies to opencode for questions; the permission path
  // replies through the broker's own callback.
  std::function<QuestionDecision(const QuestionAsked&)> reply_question;
};

class OpencodeEventRouter {
 public:
  OpencodeEventRouter(std::string role, agents::PermissionBroker& broker,
                      EventRouterHooks hooks);

  // Returns true when the event was handled (a permission or a question).
  bool handle(const agents::OpencodeEvent& event);

  std::size_t permissions_handled() const { return permissions_handled_; }
  std::size_t questions_handled() const { return questions_handled_; }

 private:
  std::string role_;
  agents::PermissionBroker& broker_;
  EventRouterHooks hooks_;
  std::size_t permissions_handled_ = 0;
  std::size_t questions_handled_ = 0;
};

}  // namespace cf::orch
