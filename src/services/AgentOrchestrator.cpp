// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/AgentOrchestrator.cpp — the in-app agent chat orchestrator (task
// 16.1). See AgentOrchestrator.hpp for the behavioural contract (Requirements
// 8.1, 8.5, 8.6, 8.7).
//
// The send pipeline mirrors the requirement structure, and every editing step
// flows through the SAME McpToolExecutor + ToolRegistry the MCP server drives,
// so the agent, the MCP server, and the UI all mutate the project through one
// atomic/undoable/observable path (Property P4).

#include "services/AgentOrchestrator.hpp"

#include <utility>

#include "core/Error.hpp"
#include "services/AuthenticationService.hpp"
#include "services/McpToolExecutor.hpp"

namespace palmier::services {

std::string_view toStringView(ChatRole role) noexcept {
    switch (role) {
        case ChatRole::User:   return "user";
        case ChatRole::Agent:  return "agent";
        case ChatRole::System: return "system";
    }
    return "system";
}

// ---------------------------------------------------------------------------
// AgentOrchestrator
// ---------------------------------------------------------------------------

AgentOrchestrator::AgentOrchestrator(McpToolExecutor& executor, IAgentAuthGate& gate,
                                     IntentInterpreter interpreter,
                                     MessagePreprocessor preprocessor)
    : executor_(executor),
      gate_(gate),
      interpreter_(std::move(interpreter)),
      // Default preprocessor is identity: 16.1 performs no mention parsing; the
      // seam is left for task 16.2 to install @-mention resolution.
      preprocessor_(preprocessor
                        ? std::move(preprocessor)
                        : MessagePreprocessor{[](std::string m) -> Result<std::string> {
                              return m;
                          }}) {}

Result<AgentTurn> AgentOrchestrator::sendMessage(std::string message) {
    // 8.5 — gate on subscription/BYOK FIRST. An unauthorized send must not touch
    // the project or the interpreter, and must PRESERVE the unsent content so the
    // user does not lose their text. The message is deliberately NOT appended to
    // the transcript as a sent turn: it is held as the pending message instead.
    if (Result<void> authorized = gate_.authorize(); authorized.isError()) {
        pendingMessage_ = std::move(message);
        hasPending_ = true;
        Error error = std::move(authorized).error();
        session_.append(ChatMessage{ChatRole::System, error.message(),
                                    /*isError=*/true, std::nullopt});
        return err<AgentTurn>(std::move(error));
    }

    // The mention-resolution seam (task 16.2). Its identity default cannot fail;
    // when 16.2 installs real resolution, an unmatched mention rejects the
    // message here BEFORE it is submitted for processing (Req 8.3).
    Result<std::string> processed = preprocessor_(std::move(message));
    if (processed.isError()) {
        Error error = std::move(processed).error();
        session_.append(ChatMessage{ChatRole::System, error.message(),
                                    /*isError=*/true, std::nullopt});
        return err<AgentTurn>(std::move(error));
    }
    const std::string processedMessage = std::move(processed).value();

    // Translate the message into a tool call. A failure to interpret means we
    // have no tool to run; nothing is executed and the project is untouched.
    if (!interpreter_) {
        Error error = makeError(ErrorCode::FailedPrecondition,
                                "the agent has no interpreter configured");
        session_.append(ChatMessage{ChatRole::System, error.message(),
                                    /*isError=*/true, std::nullopt});
        return err<AgentTurn>(std::move(error));
    }
    Result<AgentIntent> interpreted = interpreter_(processedMessage);
    if (interpreted.isError()) {
        // Record the user's message (it was accepted and submitted) and the
        // interpretation error as a system notice.
        session_.append(ChatMessage{ChatRole::User, processedMessage,
                                    /*isError=*/false, std::nullopt});
        Error error = std::move(interpreted).error();
        session_.append(ChatMessage{ChatRole::System, error.message(),
                                    /*isError=*/true, std::nullopt});
        return err<AgentTurn>(std::move(error));
    }
    const AgentIntent intent = std::move(interpreted).value();

    // 8.1 / P4 — run the intent through the SHARED executor (same path as the
    // MCP server). 8.6/8.7 come from the executor's guarantees: a successful
    // edit is applied synchronously (reflected the instant this returns), and a
    // failure or timeout has already rolled the project back to its exact
    // pre-invocation state before returning the error.
    Result<Json> executed = executor_.executeTool(intent.toolName, intent.arguments);

    // The user's message was submitted for processing; record it either way.
    session_.append(ChatMessage{ChatRole::User, processedMessage,
                                /*isError=*/false, std::nullopt});

    if (executed.isError()) {
        // 8.7 — surface the failure; the project is unchanged (executor rolled
        // back). The user turn is kept; a System error entry describes the
        // failed operation.
        Error error = std::move(executed).error();
        ChatMessage failure{ChatRole::System,
                            "The operation failed and the project was left unchanged: " +
                                error.message(),
                            /*isError=*/true, intent.toolName};
        session_.append(std::move(failure));
        return err<AgentTurn>(std::move(error));
    }

    // 8.6 — success: the edit is reflected in the project state. Record the
    // agent's response, clear any preserved pending message, and return.
    Json result = std::move(executed).value();
    session_.append(ChatMessage{ChatRole::Agent, "Applied " + intent.toolName + ".",
                                /*isError=*/false, intent.toolName});
    clearPendingMessage();
    return AgentTurn{intent.toolName, std::move(result)};
}

// ---------------------------------------------------------------------------
// AuthServiceAgentGate
// ---------------------------------------------------------------------------

AuthServiceAgentGate::AuthServiceAgentGate(const AuthenticationService& auth,
                                           std::vector<std::string> byokProviders)
    : auth_(auth), byokProviders_(std::move(byokProviders)) {}

Result<void> AuthServiceAgentGate::authorize() const {
    // An active subscription authorizes generative agent use (Req 8.5 / 9.7).
    if (const std::optional<Session>& session = auth_.currentSession();
        session.has_value() && session->entitlement == EntitlementStatus::Active) {
        return ok();
    }

    // Otherwise a validated BYOK credential for any configured provider suffices.
    for (const std::string& provider : byokProviders_) {
        if (auth_.isByokAuthorized(provider)) {
            return ok();
        }
    }

    return err(makeError(
        ErrorCode::Unauthenticated,
        "An active subscription or BYOK credentials are required to use the agent "
        "chat. Please log in or add your API key to continue."));
}

}  // namespace palmier::services
