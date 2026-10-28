// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "preferences.h"
#include "sshsession.h"
#include "terminalappearance.h"

#include <QPointer>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QWidget>

class QAction;
class QFile;
class SessionEndedPanel;
class SftpPanel;
class QTermWidget;

class TerminalPage final : public QWidget
{
    Q_OBJECT

public:
    // Starts a local shell in a PTY.
    explicit TerminalPage(const TerminalAppearance &appearance, QWidget *parent = nullptr);
    // Connects to an SSH server and runs its login shell in this terminal.
    TerminalPage(const SshConnectionSettings &settings, const TerminalAppearance &appearance,
                 QWidget *parent = nullptr);
    ~TerminalPage() override;

    // The remote file browser for an SSH tab; nullptr for local shells. The
    // page owns it, but MainWindow shows it outside the page.
    SftpPanel *sftpPanel() const;
    // Applies new colors and font; this also resets any zoom.
    void setAppearance(const TerminalAppearance &appearance);
    void setBehaviour(const TerminalBehaviour &behaviour);
    // Shortcuts by ID from shortcutDefinitions(); only the terminal ones apply.
    void setShortcuts(const QHash<QString, QKeySequence> &shortcuts);
    // Whether an SSH tab has a live connection (local shells count as connected).
    bool isConnected() const;
    const SshConnectionSettings &sshSettings() const { return m_sshSettings; }
    // Starts or stops writing output to a log file; name is used in its file name.
    void setLogging(const LoggingPreferences &preferences, const QString &name);
    void setHighlightRules(const QList<HighlightRule> &rules);
    // Types text as if the user did (snippets).
    void typeText(const QString &text);
    // Input broadcast from another tab: goes straight to the shell.
    void sendInput(const QByteArray &data);
    // Zooms this many steps from the configured font size (presentation mode).
    void setZoomSteps(int steps);

signals:
    void statusMessage(const QString &message);
    // Output arrived or the bell rang; MainWindow marks the tab if it is not current.
    void activity();
    void bell();
    // Keys the user typed into a connected SSH tab, for broadcasting.
    void userInput(const QByteArray &data);
    // The user asked to close this tab after its SSH session ended.
    void closeRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class InputMode { Idle, Prompt, Remote, Closed };
    enum class PromptPurpose { Username, Authentication };

    void createTerminal();
    void showTerminalMenu(const QPoint &position);
    void saveTerminalOutput();

    void startSsh();
    void handleTerminalInput(const char *data, int size);
    void showLocalText(const QString &text);
    void confirmHostKey(SshHostKeyStatus status, const QString &keyType,
                        const QString &fingerprint);
    void beginPrompts(PromptPurpose purpose, const QString &instruction,
                      const QStringList &prompts, const QList<bool> &echo);
    void showNextPrompt();
    void handlePromptInput(const QByteArray &data);
    void submitPromptLine();
    void cancelPrompts();
    void handleSshDisconnected(const QString &reason, bool error);
    void logOutput(const QByteArray &data);
    QByteArray highlight(const QByteArray &data);

    QTermWidget *m_terminal = nullptr;
    SessionEndedPanel *m_endedBar = nullptr;
    QAction *m_zoomInAction = nullptr;
    QAction *m_zoomOutAction = nullptr;
    QAction *m_resetZoomAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_findAction = nullptr;
    QAction *m_clearAction = nullptr;
    TerminalBehaviour m_behaviour;
    QFile *m_log = nullptr;
    LoggingPreferences m_logging;
    bool m_logAtLineStart = true;
    struct CompiledRule
    {
        QRegularExpression expression;
        QByteArray start;
    };
    QList<CompiledRule> m_highlights;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    bool m_alternateScreen = false;
    int m_reconnectAttempts = 0;
    bool m_wasConnected = false;
    SshSession *m_ssh = nullptr;
    QPointer<SftpPanel> m_sftpPanel;
    TerminalAppearance m_appearance;
    int m_wheelZoomDelta = 0;
    SshConnectionSettings m_sshSettings;
    InputMode m_inputMode = InputMode::Idle;
    PromptPurpose m_promptPurpose = PromptPurpose::Authentication;
    QStringList m_prompts;
    QList<bool> m_promptEcho;
    QStringList m_promptAnswers;
    QByteArray m_promptLine;
};
