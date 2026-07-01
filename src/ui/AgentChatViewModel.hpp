// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AgentChatViewModel.hpp — the Qt-free presentation model behind the Agent
// Chat panel and the editor's GPU / generative-service status notices (task
// 19.6; design.md "Component 6: In-App Agent Orchestrator" and the UI layer).
//
// The Agent Chat panel (design.md "CHAT[Agent Chat Panel]") is a thin Qt/QML
// surface. All of its logic — driving the AgentOrchestrator, exposing the chat
// transcript, computing @-mention affordances, and holding the two editor
// notices this task owns — lives in THIS class, which contains no Qt types so it
// can be unit-tested without a Qt install. The QWidget/QML view (guarded by
// PALMIER_HAVE_QT) merely renders this model and forwards user actions to it.
//
// Requirements this model realizes:
//
//   * 8.1  The chat operates on the current project through the SAME tools the
//          MCP server uses — it does so by delegating every send to the
//          AgentOrchestrator, which drives the shared McpToolExecutor/ToolRegistry.
//   * 8.2  @-mentions of media items resolve to the referenced item; the model
//          exposes the resolution so the view can render @-mention affordances.
//   * 8.3  An @-mention matching NO media item rejects the message WITHOUT
//          submitting it — surfaced as ChatSendStatus::MentionNotFound.
//   * 8.4  An @-mention matching MORE THAN ONE item prompts the user to pick one
//          BEFORE submitting — surfaced as ChatSendStatus::MentionAmbiguous with
//          the candidate list the picker renders.
//   * 8.5  A send without subscription/BYOK is rejected with an auth prompt and
//          the unsent text is PRESERVED (pendingMessage()) for retry.
//   * 8.6/8.7  A successful edit is reflected in the project; a failed edit leaves
//          it unchanged — both inherited from the orchestrator/executor.
//   * 10.4 A NON-BLOCKING notification that GPU acceleration is unavailable, set
//          when the GPU layer fell back to CPU (software) processing. It is a
//          notice only; it never blocks chat, editing, or any other capability.
//   * 13.4 A "Generative_AI_Service unavailable" error state that leaves the
//          open-source editor fully functional (editorRemainsFunctional() stays
//          true) and preserves in-progress editing state.
//
// The model is transport/UI-agnostic and depends only on the service layer
// (AgentOrchestrator, MentionResolver) and core value types, so the isolated
// unit test compiles it against those sources directly — no Qt, FFmpeg, Vulkan,
// or libsecret.

#ifndef PALMIER_UI_AGENTCHATVIEWMODEL_HPP
#define PALMIER_UI_AGENTCHATVIEWMODEL_HPP

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/MediaAssetRef.hpp"
#include "services/AgentOrchestrator.hpp"
#include "services/MentionResolver.hpp"

namespace palmier::ui {

// ---------------------------------------------------------------------------
// GPU acceleration status (Requirement 10.4)
// ---------------------------------------------------------------------------

/// The GPU-acceleration state the panel surfaces as a NON-BLOCKING notice.
///
/// Derived from the GPU layer: when the GpuContext is the software (CPU)
/// fallback — no compatible GPU, or detection timed out — acceleration is
/// unavailable and `notice` carries the user-facing message (Requirement 10.4).
/// The notice is purely informational; it does not gate any capability.
struct GpuAccelerationStatus {
    bool                       accelerated = true;  ///< False on the software fallback.
    std::optional<std::string> notice;              ///< Set iff !accelerated.

    /// GPU acceleration is active — no notice.
    [[nodiscard]] static GpuAccelerationStatus accelerated_() {
        return GpuAccelerationStatus{true, std::nullopt};
    }

    /// GPU acceleration is unavailable — carry a non-blocking notice.
    [[nodiscard]] static GpuAccelerationStatus unavailable(std::string message) {
        return GpuAccelerationStatus{false, std::move(message)};
    }
};

/// Derive a GpuAccelerationStatus from any GpuContext-like object exposing
/// `isSoftwareFallback()` and `unavailableNotice()` (Requirement 10.4).
///
/// Templated so this header carries NO dependency on the GPU library: the
/// composition root (task 21.1) instantiates it with the real gpu::GpuContext,
/// while tests use a lightweight stand-in. When the context is the software
/// fallback, the context's own notice is used (or a sensible default when it
/// carries none).
template <typename GpuContextLike>
[[nodiscard]] GpuAccelerationStatus makeGpuAccelerationStatus(const GpuContextLike& context) {
    if (context.isSoftwareFallback()) {
        const std::optional<std::string>& notice = context.unavailableNotice();
        return GpuAccelerationStatus::unavailable(
            notice.has_value()
                ? *notice
                : std::string("GPU acceleration is unavailable; media operations "
                              "will use the CPU processing path."));
    }
    return GpuAccelerationStatus::accelerated_();
}

// ---------------------------------------------------------------------------
// Send outcome
// ---------------------------------------------------------------------------

/// The category of outcome of submitting a chat message. Mirrors the Requirement
/// 8 branches so the view can render the right affordance (a retry prompt, an
/// @-mention picker, an error notice, or the applied result).
enum class ChatSendStatus {
    Applied,                 ///< The edit was applied and reflected (8.6).
    AuthenticationRequired,  ///< No subscription/BYOK; text preserved (8.5).
    MentionNotFound,         ///< An @-mention matched no media item (8.3).
    MentionAmbiguous,        ///< An @-mention matched several items (8.4).
    Failed,                  ///< The message could not be interpreted or the edit failed (8.7).
};

/// The structured result of ChatViewModel::sendMessage — everything the view
/// needs to react, without inspecting the transcript.
struct ChatSendResult {
    ChatSendStatus                        status = ChatSendStatus::Failed;
    std::string                           notice;            ///< Human-readable message/notice.
    std::optional<std::string>            toolName;          ///< The tool run on Applied.
    std::string                           problemMention;    ///< The offending mention (8.3/8.4).
    std::vector<services::MentionCandidate> candidates;      ///< Candidates to pick from (8.4).

    [[nodiscard]] bool applied() const noexcept { return status == ChatSendStatus::Applied; }
};

/// A pre-send preview of a message's @-mentions, for as-you-type affordances
/// (Requirement 8.2). It does NOT submit the message; it only reports how the
/// mentions would resolve so the view can offer completions / a disambiguation
/// picker before the user sends.
struct MentionAffordance {
    services::MentionStatus                 status = services::MentionStatus::Resolved;
    std::vector<services::ResolvedMention>  resolved;        ///< Resolved mentions (8.2).
    std::string                             problemMention;  ///< Offending mention (8.3/8.4).
    std::vector<services::MentionCandidate> candidates;      ///< Candidates for an ambiguous mention (8.4).

    [[nodiscard]] bool isResolved() const noexcept {
        return status == services::MentionStatus::Resolved;
    }
};

// ---------------------------------------------------------------------------
// AgentChatViewModel
// ---------------------------------------------------------------------------

/// The Qt-free model behind the Agent Chat panel and the editor status notices.
///
/// Owns no state the orchestrator already owns: the transcript and preserved
/// pending message are read straight through the referenced AgentOrchestrator
/// (which must outlive the model). The model adds the UI-facing concerns this
/// task introduces — @-mention affordances, the GPU-unavailable notice, and the
/// generative-service-unavailable error — none of which gate the editor.
class AgentChatViewModel {
public:
    /// A provider of the CURRENT project's media library, used to compute
    /// @-mention affordances and to enrich an ambiguous-mention send with its
    /// candidate list. Empty by default (no media → mentions never match).
    using MentionSource = std::function<std::vector<MediaAssetRef>()>;

    /// Construct over the agent `orchestrator` that drives every send (8.1). The
    /// orchestrator must outlive the model.
    explicit AgentChatViewModel(services::AgentOrchestrator& orchestrator);

    // --- Chat transcript ---------------------------------------------------

    /// The conversation transcript so far (user turns, agent responses, and
    /// system notices), in order.
    [[nodiscard]] const std::vector<services::ChatMessage>& transcript() const noexcept;

    /// The current input draft the user is composing (bound to the input box).
    [[nodiscard]] const std::string& draft() const noexcept { return draft_; }
    void setDraft(std::string text) { draft_ = std::move(text); }

    // --- Sending -----------------------------------------------------------

    /// Submit `message` through the agent pipeline (auth → mention resolution →
    /// interpret → execute). Returns a structured outcome; on an auth rejection
    /// the unsent text is preserved (see pendingMessage(), Requirement 8.5) and
    /// on an ambiguous mention the candidate list is returned (Requirement 8.4).
    [[nodiscard]] ChatSendResult sendMessage(std::string message);

    /// Submit the current draft() and, on anything other than an auth rejection,
    /// clear the draft. On an auth rejection the draft is left intact so the user
    /// can retry after authenticating (Requirement 8.5).
    [[nodiscard]] ChatSendResult sendDraft();

    /// The unsent message preserved after an auth-gated rejection (Requirement
    /// 8.5), or empty when nothing is pending. Cleared by the orchestrator on the
    /// next successful send.
    [[nodiscard]] const std::string& pendingMessage() const noexcept;
    [[nodiscard]] bool hasPendingMessage() const noexcept;

    // --- @-mention affordances (Requirement 8.2/8.4) -----------------------

    /// Set the source of the current project's media library used for @-mention
    /// resolution. Installing a source also wires the SAME resolution into the
    /// orchestrator's message preprocessor, so a sent message resolves its
    /// mentions on the identical library the affordances were computed from.
    void setMentionSource(MentionSource source);

    /// Preview how `message`'s @-mentions would resolve, WITHOUT submitting it —
    /// for as-you-type completions and the disambiguation picker (8.2/8.4).
    [[nodiscard]] MentionAffordance previewMentions(const std::string& message) const;

    // --- GPU acceleration notice (Requirement 10.4) ------------------------

    /// Update the GPU-acceleration status (typically once at startup from the
    /// selected GpuContext, via makeGpuAccelerationStatus). NON-BLOCKING.
    void setGpuAccelerationStatus(GpuAccelerationStatus status) {
        gpu_ = std::move(status);
    }

    /// True when GPU acceleration is active.
    [[nodiscard]] bool gpuAccelerationAvailable() const noexcept { return gpu_.accelerated; }

    /// The non-blocking "GPU acceleration unavailable" notice, or nullopt when
    /// acceleration is active (Requirement 10.4).
    [[nodiscard]] const std::optional<std::string>& gpuUnavailableNotice() const noexcept {
        return gpu_.notice;
    }

    // --- Generative service availability (Requirement 13.4) ----------------

    /// Record that the Generative_AI_Service is unreachable / authentication
    /// failed, with a user-facing `message`. This is an error NOTICE only: it
    /// never degrades the open-source editor (editorRemainsFunctional() stays
    /// true) and preserves in-progress editing state (Requirement 13.4).
    void reportGenerativeServiceUnavailable(std::string message) {
        generativeServiceError_ = std::move(message);
    }

    /// Clear the generative-service error once the service is reachable again.
    void clearGenerativeServiceError() noexcept { generativeServiceError_.reset(); }

    /// True while the Generative_AI_Service is considered reachable.
    [[nodiscard]] bool generativeServiceAvailable() const noexcept {
        return !generativeServiceError_.has_value();
    }

    /// The generative-service-unavailable error message, or nullopt when the
    /// service is reachable (Requirement 13.4).
    [[nodiscard]] const std::optional<std::string>& generativeServiceError() const noexcept {
        return generativeServiceError_;
    }

    /// The open-source editor always remains functional regardless of GPU
    /// acceleration or generative-service availability (Requirements 10.4/13.4).
    /// Provided so the view (and tests) can assert the invariant explicitly.
    [[nodiscard]] bool editorRemainsFunctional() const noexcept { return true; }

private:
    // Classify an orchestrator send error into a ChatSendResult, enriching a
    // mention-related failure with structured candidates from the media source.
    [[nodiscard]] ChatSendResult classifyError(const Error& error,
                                               const std::string& submitted) const;

    services::AgentOrchestrator& orchestrator_;
    MentionSource                mentionSource_;
    std::string                  draft_;
    GpuAccelerationStatus        gpu_ = GpuAccelerationStatus::accelerated_();
    std::optional<std::string>   generativeServiceError_;
};

}  // namespace palmier::ui

#endif  // PALMIER_UI_AGENTCHATVIEWMODEL_HPP
