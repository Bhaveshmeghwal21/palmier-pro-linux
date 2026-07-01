// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/AgentChatPanel.hpp — the Qt 6 Agent Chat panel widget (task 19.6).
//
// This is the thin QWidget surface for the in-app agent chat (design.md
// "CHAT[Agent Chat Panel]"). It owns NO logic: every decision — driving the
// agent, resolving @-mentions, and the GPU / generative-service notices — lives
// in the Qt-free palmier::ui::AgentChatViewModel, which this widget merely
// renders and forwards user actions to. That split keeps the chat logic
// unit-testable without a Qt install; only this rendering shell needs Qt.
//
// The whole translation unit is guarded by PALMIER_HAVE_QT (mirroring
// MainWindow and the project's PALMIER_HAVE_VULKAN/PALMIER_HAVE_FFMPEG guard
// style) so the module tree still configures and builds where Qt 6 is absent;
// the compiled panel is produced only when Qt is found.

#ifndef PALMIER_UI_AGENTCHATPANEL_HPP
#define PALMIER_UI_AGENTCHATPANEL_HPP

#ifdef PALMIER_HAVE_QT

#include <QWidget>

#include "ui/AgentChatViewModel.hpp"

class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;

namespace palmier::ui {

/// The agent-chat dock panel. Renders the transcript, an input box wired to the
/// view model's send pipeline, a non-blocking GPU-unavailable banner (Req 10.4),
/// and a generative-service-unavailable error banner that never disables the
/// panel (Req 13.4). All state lives in the referenced AgentChatViewModel, which
/// must outlive the panel.
class AgentChatPanel : public QWidget {
    Q_OBJECT

public:
    explicit AgentChatPanel(AgentChatViewModel& model, QWidget* parent = nullptr);
    ~AgentChatPanel() override;

    /// Re-render the transcript and notices from the model (call after external
    /// state changes, e.g. a new GPU/service status).
    void refresh();

private slots:
    /// Submit the current input; renders the outcome (applied result, auth
    /// prompt, @-mention picker, or error) per the ChatSendResult.
    void onSendClicked();

private:
    void buildUi();
    void renderTranscript();
    void renderNotices();

    AgentChatViewModel& model_;

    QTextEdit*   transcriptView_ = nullptr;
    QLineEdit*   input_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QLabel*      gpuNotice_ = nullptr;
    QLabel*      serviceNotice_ = nullptr;
    QLabel*      mentionHint_ = nullptr;
};

}  // namespace palmier::ui

#endif  // PALMIER_HAVE_QT

#endif  // PALMIER_UI_AGENTCHATPANEL_HPP
