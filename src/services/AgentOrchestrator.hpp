// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/AgentOrchestrator.hpp — the in-app agent chat orchestrator (task
// 16.1; design.md "Component 6: In-App Agent Orchestrator").
//
// Requirement 8 gives the agent chat its shape:
//
//   * 8.1  The Agent_Chat operates on the current project using the SAME tools
//          exposed by the MCP server.
//   * 8.5  A message sent without an active subscription or BYOK credentials is
//          rejected with a prompt to authenticate, AND the unsent message
//          content is preserved (the user must not lose their text).
//   * 8.6  When the agent performs an edit, the resulting change is reflected in
//          the project state within the time budget of the operation completing.
//   * 8.7  A failed edit operation surfaces an error and leaves the project
//          state unchanged from before the operation.
//
// The mechanism that makes 8.1 (and Property P4 — UI/MCP/agent edit equivalence)
// true is reuse, not re-implementation: the orchestrator does not build its own
// editing path. It drives an `McpToolExecutor` over the shared `ToolRegistry` —
// the very same executor+registry the MCP HTTP server uses (tasks 15.1/15.3).
// That reuse hands us 8.6 and 8.7 essentially for free: the executor already
// runs each tool within a time budget, and on any failure or timeout it rolls
// the project back to its exact pre-invocation state (Requirements 7.4/7.6/7.7),
// which is precisely 8.7's "leave the project state unchanged". Because the
// executor applies commands synchronously through the TimelineEngine, a
// successful edit is observable the instant the call returns, satisfying 8.6.
//
// This component is deliberately transport/UI-agnostic (no Qt): it owns the
// chat session (message history) and the send pipeline, but it reaches the
// outside world only through narrow seams:
//
//   * IAgentAuthGate     — decides whether the current user may send a message
//                          (active subscription OR authorized BYOK). 8.5.
//   * MessagePreprocessor — a per-message transform run before interpretation.
//                          This is the seam @-mention resolution (task 16.2)
//                          plugs into; 16.1 leaves it as an identity default and
//                          does NOT implement mention parsing here.
//   * IntentInterpreter   — translates a (preprocessed) user message into a tool
//                          call {toolName, arguments}. The natural-language →
//                          tool mapping (an LLM in production) lives behind this
//                          seam; the orchestrator only routes the resulting
//                          intent into the shared executor.
//
// Concrete adapters over the real AuthenticationService are provided below for
// the composition root (task 21.1).

#ifndef PALMIER_SERVICES_AGENTORCHESTRATOR_HPP
#define PALMIER_SERVICES_AGENTORCHESTRATOR_HPP

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.hpp"
#include "services/Json.hpp"

namespace palmier::services {

class AuthenticationService;  // services/AuthenticationService.hpp
class McpToolExecutor;        // services/McpToolExecutor.hpp — the shared exec path.

// ---------------------------------------------------------------------------
// Chat session model
// ---------------------------------------------------------------------------

/// Who authored a chat message.
enum class ChatRole {
    User,   ///< A message the user sent (and that was accepted for processing).
    Agent,  ///< The agent's response to a successfully executed intent.
    System, ///< An out-of-band notice: an auth prompt or an operation error.
};

/// Stable lowercase name for a ChatRole ("user"/"agent"/"system").
[[nodiscard]] std::string_view toStringView(ChatRole role) noexcept;

/// One entry in the conversation transcript.
struct ChatMessage {
    ChatRole                   role = ChatRole::User;
    std::string                content;              ///< Human-readable text.
    bool                       isError = false;      ///< True for failure/notice entries.
    std::optional<std::string> toolName;             ///< Tool driven by an Agent turn.
};

/// The ordered transcript of a single agent chat conversation. Pure data; no
/// threading, no I/O — the orchestrator appends to it as turns are processed.
class ChatSession {
public:
    void append(ChatMessage message) { messages_.push_back(std::move(message)); }

    [[nodiscard]] const std::vector<ChatMessage>& messages() const noexcept {
        return messages_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return messages_.size(); }
    [[nodiscard]] bool empty() const noexcept { return messages_.empty(); }

    /// The most recent message, or nullptr when the transcript is empty.
    [[nodiscard]] const ChatMessage* last() const noexcept {
        return messages_.empty() ? nullptr : &messages_.back();
    }

    void clear() noexcept { messages_.clear(); }

private:
    std::vector<ChatMessage> messages_;
};

// ---------------------------------------------------------------------------
// Seams
// ---------------------------------------------------------------------------

/// Gate deciding whether the current user may send an agent message (Req 8.5).
/// Returns ok() when authorized (active subscription OR valid BYOK); otherwise
/// an ErrorCode::Unauthenticated Error whose message prompts authentication.
class IAgentAuthGate {
public:
    virtual ~IAgentAuthGate() = default;
    [[nodiscard]] virtual Result<void> authorize() const = 0;
};

/// A parsed user intent: a tool call to run through the shared executor. The
/// tool name is one of the ToolRegistry surface (e.g. "timeline.add_clip"), and
/// `arguments` is the JSON payload the tool handler expects.
struct AgentIntent {
    std::string toolName;
    Json        arguments = Json::object();
};

/// Translates a (preprocessed) user message into an AgentIntent. In production
/// this is the language-model call; in tests it is a scripted mapping. An error
/// means the message could not be turned into a tool call (nothing is executed).
using IntentInterpreter = std::function<Result<AgentIntent>(std::string_view message)>;

/// A per-message transform applied BEFORE interpretation. The @-mention
/// resolution of task 16.2 plugs in here (resolving "@clip" references, or
/// rejecting an unmatched mention so the message is not submitted). 16.1 uses an
/// identity default. Returning an error rejects the message before any tool runs.
using MessagePreprocessor = std::function<Result<std::string>(std::string message)>;

// ---------------------------------------------------------------------------
// AgentOrchestrator
// ---------------------------------------------------------------------------

/// The outcome of a successfully processed agent turn.
struct AgentTurn {
    std::string toolName;  ///< The tool the intent invoked.
    Json        result;    ///< The tool's success payload.
};

/// Maintains a chat session and routes user messages into the shared MCP tool
/// executor, enforcing the agent-chat policy of Requirement 8.
///
/// All referenced collaborators (executor, gate) must outlive the orchestrator.
/// Thread-affinity: instances are not internally synchronized.
class AgentOrchestrator {
public:
    /// Construct over the shared `executor` (the SAME executor/registry the MCP
    /// server drives — Req 8.1 / Property P4), the auth `gate` (Req 8.5), and an
    /// `interpreter` mapping messages to tool calls. The message `preprocessor`
    /// defaults to identity; task 16.2 sets it to @-mention resolution.
    AgentOrchestrator(McpToolExecutor& executor, IAgentAuthGate& gate,
                      IntentInterpreter interpreter,
                      MessagePreprocessor preprocessor = {});

    /// Send a user message through the pipeline: authorize (8.5) -> preprocess
    /// (mention seam) -> interpret -> execute through the shared executor.
    ///
    ///   * Unauthorized (8.5): returns Unauthenticated; the message is NOT added
    ///     to the transcript but is preserved (see pendingMessage()) so the user
    ///     can retry after authenticating. A System notice is recorded.
    ///   * Preprocess/interpret failure: the message is not submitted; a System
    ///     error is recorded and the error returned. The project is untouched.
    ///   * Edit failure (8.7): the executor has already rolled the project back
    ///     to its pre-invocation state; the user turn and a System error entry
    ///     are recorded and the error is returned.
    ///   * Success (8.6): the edit is reflected in the project (synchronously via
    ///     the executor); the user turn and an Agent response are recorded, any
    ///     preserved pending message is cleared, and the AgentTurn is returned.
    [[nodiscard]] Result<AgentTurn> sendMessage(std::string message);

    /// The conversation transcript so far.
    [[nodiscard]] const ChatSession& session() const noexcept { return session_; }

    /// The unsent message content preserved after an auth-gated rejection (Req
    /// 8.5), or empty when there is nothing pending. Cleared on the next
    /// successful send.
    [[nodiscard]] const std::string& pendingMessage() const noexcept {
        return pendingMessage_;
    }
    [[nodiscard]] bool hasPendingMessage() const noexcept { return hasPending_; }

    /// Discard any preserved pending message (e.g. the user cleared the input).
    void clearPendingMessage() noexcept {
        pendingMessage_.clear();
        hasPending_ = false;
    }

    /// Replace the message interpreter (e.g. when the model backend changes).
    void setInterpreter(IntentInterpreter interpreter) {
        interpreter_ = std::move(interpreter);
    }

    /// Install the message preprocessor — the seam task 16.2 uses to wire in
    /// @-mention resolution.
    void setPreprocessor(MessagePreprocessor preprocessor) {
        preprocessor_ = std::move(preprocessor);
    }

private:
    McpToolExecutor&    executor_;
    IAgentAuthGate&     gate_;
    IntentInterpreter   interpreter_;
    MessagePreprocessor preprocessor_;
    ChatSession         session_;
    std::string         pendingMessage_;
    bool                hasPending_ = false;
};

// ---------------------------------------------------------------------------
// Concrete auth gate for the composition root (task 21.1)
// ---------------------------------------------------------------------------

/// IAgentAuthGate backed by the real AuthenticationService (Req 8.5).
///
/// Authorized when the current session holds an Active subscription entitlement,
/// or when a validated BYOK credential is authorized for any of the configured
/// `byokProviders`. Otherwise authorize() returns Unauthenticated with a message
/// prompting the user to authenticate (log in or add BYOK credentials).
class AuthServiceAgentGate : public IAgentAuthGate {
public:
    explicit AuthServiceAgentGate(const AuthenticationService& auth,
                                  std::vector<std::string> byokProviders = {});

    [[nodiscard]] Result<void> authorize() const override;

private:
    const AuthenticationService& auth_;
    std::vector<std::string>     byokProviders_;
};

}  // namespace palmier::services

#endif  // PALMIER_SERVICES_AGENTORCHESTRATOR_HPP
