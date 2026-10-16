#include "cf/orch/event_router.hpp"

#include <utility>

namespace cf::orch {

OpencodeEventRouter::OpencodeEventRouter(std::string role,
                                         agents::PermissionBroker& broker,
                                         EventRouterHooks hooks)
    : role_(std::move(role)), broker_(broker), hooks_(std::move(hooks)) {}

bool OpencodeEventRouter::handle(const agents::OpencodeEvent& event) {
  using agents::PermissionRequest;

  if (event.type == "permission.asked") {
    PermissionRequest request;
    request.role = role_;
    request.session_id = event.properties.value("sessionID", "");
    request.request_id = event.properties.value("id", "");
    request.type = event.properties.value("permission", "");
    const auto metadata = event.properties.find("metadata");
    if (metadata != event.properties.end() && metadata->is_object()) {
      request.command_or_path = metadata->value("command", "");
    }
    const auto patterns = event.properties.find("patterns");
    if (patterns != event.properties.end() && patterns->is_array()) {
      for (const auto& pattern : *patterns) {
        if (pattern.is_string()) {
          request.patterns.push_back(pattern.get<std::string>());
        }
      }
    }
    (void)broker_.handle(request);  // replies through the broker's callback
    ++permissions_handled_;
    return true;
  }

  if (event.type == "question.asked") {
    QuestionAsked asked;
    asked.session_id = event.properties.value("sessionID", "");
    asked.request_id = event.properties.value("id", "");
    const auto questions = event.properties.find("questions");
    if (questions != event.properties.end() && questions->is_array()) {
      for (const auto& question : *questions) {
        Question parsed;
        parsed.text = question.value("question", "");
        parsed.multiple = question.value("multiple", false);
        const auto options = question.find("options");
        if (options != question.end() && options->is_array()) {
          for (const auto& option : *options) {
            QuestionOption parsed_option;
            parsed_option.label = option.value("label", "");
            parsed_option.description = option.value("description", "");
            parsed.options.push_back(std::move(parsed_option));
          }
        }
        asked.questions.push_back(std::move(parsed));
      }
    }
    if (hooks_.reply_question) {
      (void)hooks_.reply_question(asked);
    }
    ++questions_handled_;
    return true;
  }

  return false;
}

}  // namespace cf::orch
