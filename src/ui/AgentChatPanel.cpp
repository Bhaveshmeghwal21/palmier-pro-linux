// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AgentChatPanel.cpp — implementation of the Qt 6 Agent Chat panel (task
// 19.6). Compiled only when Qt 6 is available (PALMIER_HAVE_QT). See
// AgentChatPanel.hpp; the panel is a thin renderer over AgentChatViewModel.

#include "ui/AgentChatPanel.hpp"

#ifdef PALMIER_HAVE_QT

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTextEdit>
#include <QVBoxLayout>

#include <string>

namespace palmier::ui {
namespace {

[[nodiscard]] QString fromStd(const std::string& text) {
    return QString::fromStdString(text);
}

[[nodiscard]] QString roleLabel(services::ChatRole role) {
    return fromStd(std::string(services::toStringView(role)));
}

}  // namespace

AgentChatPanel::AgentChatPanel(AgentChatViewModel& model, QWidget* parent)
    : QWidget(parent), model_(model) {
    buildUi();
    refresh();
}

AgentChatPanel::~AgentChatPanel() = default;

void AgentChatPanel::buildUi() {
    auto* layout = new QVBoxLayout(this);

    // Non-blocking notices at the top: they inform, never disable the panel.
    gpuNotice_ = new QLabel(this);
    gpuNotice_->setWordWrap(true);
    gpuNotice_->setStyleSheet(QStringLiteral("color: #a15c00;"));  // advisory tone
    gpuNotice_->hide();
    layout->addWidget(gpuNotice_);

    serviceNotice_ = new QLabel(this);
    serviceNotice_->setWordWrap(true);
    serviceNotice_->setStyleSheet(QStringLiteral("color: #b00020;"));  // error tone
    serviceNotice_->hide();
    layout->addWidget(serviceNotice_);

    transcriptView_ = new QTextEdit(this);
    transcriptView_->setReadOnly(true);
    layout->addWidget(transcriptView_, /*stretch=*/1);

    mentionHint_ = new QLabel(this);
    mentionHint_->setWordWrap(true);
    mentionHint_->hide();
    layout->addWidget(mentionHint_);

    auto* inputRow = new QHBoxLayout();
    input_ = new QLineEdit(this);
    input_->setPlaceholderText(QStringLiteral("Message the agent (use @ to mention media)…"));
    connect(input_, &QLineEdit::returnPressed, this, &AgentChatPanel::onSendClicked);
    inputRow->addWidget(input_, /*stretch=*/1);

    sendButton_ = new QPushButton(QStringLiteral("Send"), this);
    connect(sendButton_, &QPushButton::clicked, this, &AgentChatPanel::onSendClicked);
    inputRow->addWidget(sendButton_);

    layout->addLayout(inputRow);
}

void AgentChatPanel::refresh() {
    renderNotices();
    renderTranscript();
    // Reflect any preserved pending message (e.g. after an auth rejection, 8.5).
    if (model_.hasPendingMessage() && input_->text().isEmpty()) {
        input_->setText(fromStd(model_.pendingMessage()));
    }
}

void AgentChatPanel::renderNotices() {
    // Requirement 10.4 — non-blocking GPU-unavailable notification.
    if (const auto& gpu = model_.gpuUnavailableNotice(); gpu.has_value()) {
        gpuNotice_->setText(fromStd(*gpu));
        gpuNotice_->show();
    } else {
        gpuNotice_->hide();
    }

    // Requirement 13.4 — generative-service-unavailable error; the panel and the
    // editor remain fully functional (the input stays enabled).
    if (const auto& svc = model_.generativeServiceError(); svc.has_value()) {
        serviceNotice_->setText(fromStd(*svc));
        serviceNotice_->show();
    } else {
        serviceNotice_->hide();
    }
}

void AgentChatPanel::renderTranscript() {
    QString html;
    for (const services::ChatMessage& message : model_.transcript()) {
        const QString cls = message.isError ? QStringLiteral("error") : roleLabel(message.role);
        html += QStringLiteral("<p><b>%1:</b> %2</p>")
                    .arg(cls, fromStd(message.content).toHtmlEscaped());
    }
    transcriptView_->setHtml(html);
}

void AgentChatPanel::onSendClicked() {
    model_.setDraft(input_->text().toStdString());
    const ChatSendResult result = model_.sendDraft();

    mentionHint_->hide();
    switch (result.status) {
        case ChatSendStatus::Applied:
            input_->clear();
            break;
        case ChatSendStatus::AuthenticationRequired:
            // 8.5 — keep the text so the user can retry after authenticating.
            input_->setText(fromStd(model_.pendingMessage()));
            mentionHint_->setText(fromStd(result.notice));
            mentionHint_->show();
            break;
        case ChatSendStatus::MentionNotFound:
            mentionHint_->setText(
                QStringLiteral("No media item matches @%1.").arg(fromStd(result.problemMention)));
            mentionHint_->show();
            break;
        case ChatSendStatus::MentionAmbiguous: {
            // 8.4 — offer the candidates for selection before submitting.
            QString hint =
                QStringLiteral("@%1 is ambiguous — pick one: ").arg(fromStd(result.problemMention));
            for (const services::MentionCandidate& c : result.candidates) {
                hint += fromStd(c.displayName.empty() ? c.assetId.toString() : c.displayName);
                hint += QStringLiteral("  ");
            }
            mentionHint_->setText(hint);
            mentionHint_->show();
            break;
        }
        case ChatSendStatus::Failed:
            mentionHint_->setText(fromStd(result.notice));
            mentionHint_->show();
            break;
    }

    renderTranscript();
}

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT
