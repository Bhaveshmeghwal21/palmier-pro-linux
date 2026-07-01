// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AgentChatViewModel.cpp — implementation of the Qt-free Agent Chat model
// (task 19.6). See AgentChatViewModel.hpp for the behavioural contract
// (Requirements 8.1-8.7, 10.4, 13.4).

#include "ui/AgentChatViewModel.hpp"

#include <utility>

namespace palmier::ui {

AgentChatViewModel::AgentChatViewModel(services::AgentOrchestrator& orchestrator)
    : orchestrator_(orchestrator) {}

// ---------------------------------------------------------------------------
// Chat transcript / pending message
// ---------------------------------------------------------------------------

const std::vector<services::ChatMessage>& AgentChatViewModel::transcript() const noexcept {
    return orchestrator_.session().messages();
}

const std::string& AgentChatViewModel::pendingMessage() const noexcept {
    return orchestrator_.pendingMessage();
}

bool AgentChatViewModel::hasPendingMessage() const noexcept {
    return orchestrator_.hasPendingMessage();
}

// ---------------------------------------------------------------------------
// @-mention affordances
// ---------------------------------------------------------------------------

void AgentChatViewModel::setMentionSource(MentionSource source) {
    mentionSource_ = std::move(source);

    // Wire the SAME resolution into the orchestrator's preprocessor so a sent
    // message resolves its mentions against the identical library the panel's
    // affordances were computed from (8.2). A fresh snapshot is taken per
    // message so mentions always resolve against the CURRENT project. When no
    // source is installed we leave the orchestrator's preprocessor untouched
    // (its identity default, or whatever the composition root set).
    if (mentionSource_) {
        MentionSource source = mentionSource_;
        orchestrator_.setPreprocessor(
            [source = std::move(source)](std::string message) -> Result<std::string> {
                const services::MentionResolver resolver(source ? source()
                                                                 : std::vector<MediaAssetRef>{});
                return services::toPreprocessorResult(resolver.resolve(message));
            });
    }
}

MentionAffordance AgentChatViewModel::previewMentions(const std::string& message) const {
    MentionAffordance affordance;
    const services::MentionResolver resolver(mentionSource_ ? mentionSource_()
                                                            : std::vector<MediaAssetRef>{});
    const services::MentionResolution resolution = resolver.resolve(message);
    affordance.status = resolution.status;
    affordance.resolved = resolution.resolved;
    affordance.problemMention = resolution.problemMention;
    affordance.candidates = resolution.candidates;
    return affordance;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

ChatSendResult AgentChatViewModel::sendMessage(std::string message) {
    // Delegate the whole policy to the orchestrator (8.1): it authorizes FIRST
    // (8.5), then resolves mentions (8.2-8.4), interprets, and executes through
    // the shared tool path (8.6/8.7). We only translate the outcome for the view.
    Result<services::AgentTurn> outcome = orchestrator_.sendMessage(message);

    if (outcome.isOk()) {
        services::AgentTurn turn = std::move(outcome).value();
        ChatSendResult result;
        result.status = ChatSendStatus::Applied;
        result.toolName = turn.toolName;
        result.notice = "Applied " + turn.toolName + ".";
        return result;
    }

    return classifyError(outcome.error(), message);
}

ChatSendResult AgentChatViewModel::sendDraft() {
    ChatSendResult result = sendMessage(draft_);
    // Preserve the draft on an auth rejection so the user can retry after
    // authenticating (8.5); otherwise the draft has been submitted — clear it.
    if (result.status != ChatSendStatus::AuthenticationRequired) {
        draft_.clear();
    }
    return result;
}

ChatSendResult AgentChatViewModel::classifyError(const Error& error,
                                                 const std::string& submitted) const {
    ChatSendResult result;
    result.notice = error.message();

    // 8.5 — auth is checked BEFORE mention resolution, so an unauthorized send is
    // reported as such even when it carries an unmatched/ambiguous mention; the
    // unsent text is already preserved by the orchestrator.
    if (error.code() == ErrorCode::Unauthenticated) {
        result.status = ChatSendStatus::AuthenticationRequired;
        return result;
    }

    // A mention-related rejection (8.3 NotFound / 8.4 FailedPrecondition) is
    // enriched into structured affordance data by re-resolving against the same
    // library, so the view can render "not found" or a disambiguation picker.
    if (mentionSource_ &&
        (error.code() == ErrorCode::NotFound || error.code() == ErrorCode::FailedPrecondition)) {
        const services::MentionResolver resolver(mentionSource_());
        const services::MentionResolution resolution = resolver.resolve(submitted);
        switch (resolution.status) {
            case services::MentionStatus::Unmatched:
                result.status = ChatSendStatus::MentionNotFound;
                result.problemMention = resolution.problemMention;
                return result;
            case services::MentionStatus::Ambiguous:
                result.status = ChatSendStatus::MentionAmbiguous;
                result.problemMention = resolution.problemMention;
                result.candidates = resolution.candidates;
                return result;
            case services::MentionStatus::Resolved:
                break;  // Not a mention problem — fall through to a generic failure.
        }
    }

    // 8.7 / interpretation failure — the project was left unchanged.
    result.status = ChatSendStatus::Failed;
    return result;
}

}  // namespace palmier::ui
