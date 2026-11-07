// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "automation.h"
#include "preferences.h"
#include "sshsession.h"
#include "terminalappearance.h"

#include <QPointer>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QWidget>

#include <memory>

class QAction;
class QMenu;
class QFile;
class QTimer;
class JumpChain;
class SessionEndedPanel;
class CastRecorder;
class SftpPanel;
class StreamConnection;
class QTermWidget;

class TerminalPage final : public QWidget
{
    Q_OBJECT

public:
    // Starts a local shell in a PTY.
    explicit TerminalPage(const TerminalAppearance &appearance, QWidget *parent = nullptr);
    // Runs a local program in a PTY instead of the shell (mosh, for example).
    TerminalPage(const QString &program, const QStringList &arguments,
                 const TerminalAppearance &appearance, QWidget *parent = nullptr);
    // Connects to an SSH server and runs its login shell in this terminal.
    TerminalPage(const SshConnectionSettings &settings, const TerminalAppearance &appearance,
                 QWidget *parent = nullptr);
    // Shows a Telnet or serial connection; the page takes ownership.
    TerminalPage(StreamConnection *connection, const TerminalAppearance &appearance,
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
    // Where files dropped on this terminal can go (filedrop.h); None types
    // their paths. Set by MainWindow for the kinds that support it.
    enum class FileDropKind { None, Local, Ssh, Docker, Kubernetes, Wsl };
    void setFileDropKind(FileDropKind kind) { m_fileDropKind = kind; }
    // Local files dropped on the terminal: true when handled (uploaded or
    // asked about); false: their paths are to be typed.
    bool dropFiles(const QStringList &files, bool shift);
    // A shell or program on this computer (not SSH, Telnet, serial…).
    bool isLocal() const { return !m_ssh && !m_stream && m_sshSettings.host.isEmpty(); }
    const SshConnectionSettings &sshSettings() const { return m_sshSettings; }
    // The SSH connection of this tab (tunnels can use it); nullptr when none.
    SshSession *sshSession() const { return m_ssh; }
    // Whether the Files pane follows the shell's folder; switched for this
    // tab only (the session's saved setting is not changed).
    bool followsFolder() const;
    void setFollowsFolder(bool on);
    // Starts or stops writing output to a log file; name is used in its file name.
    void setLogging(const LoggingPreferences &preferences, const QString &name);
    // Records this tab as an asciinema .cast in the log folder.
    void setRecording(bool on);
    bool isRecording() const;
    void setHighlightRules(const QList<HighlightRule> &rules);
    // Types text as if the user did (snippets).
    void typeText(const QString &text);
    // Input broadcast from another tab: goes straight to the shell.
    void sendInput(const QByteArray &data);
    // Zooms this many steps from the configured font size (presentation mode).
    void setZoomSteps(int steps);
    // Multi-exec: typing into one chosen terminal goes to all chosen ones.
    // A chosen terminal has an accent bar along its top.
    void setMultiExecTarget(bool on);
    bool isMultiExecTarget() const { return m_multiExecTarget; }
    // A login script and triggers (see AutomationRunner), started on each
    // connect; values fill {password}, {user} and {host}.
    void setAutomation(const QList<LoginStep> &script, const QList<OutputTrigger> &triggers,
                       const AutomationPreferences &preferences, const QHash<QString, QString> &values);
    // Runs the login script again, or stops it.
    void runLoginScript();
    void stopLoginScript();

signals:
    void statusMessage(const QString &message);
    // Output arrived or the bell rang; MainWindow marks the tab if it is not current.
    void activity();
    void bell();
    // Keys the user typed into a connected SSH tab, for broadcasting.
    void userInput(const QByteArray &data);
    // The user asked to close this tab after its SSH session ended.
    void closeRequested();
    // The terminal's own menu turned multi-exec on or off for it.
    void multiExecToggled(bool on);
    // The terminal's own menu: a new pane beside it, or the same session in
    // a new tab.
    // Asks MainWindow to add Split Right ▸ / Split Down ▸ to this menu.
    void splitMenuRequested(QMenu *menu);
    void duplicateRequested();
    // Asks MainWindow to add Run Script ▸ (saved scripts) to this menu.
    void scriptMenuRequested(QMenu *menu);
    // A trigger asked to notify, or to mark the tab.
    void automationNotice(const QString &message);
    void automationMark();
    // Logged on: the login script finished, or there was none (after
    // connecting); for the session's "after logging on" script.
    void loggedOn();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void checkRecording();
    enum class InputMode { Idle, Prompt, Remote, Closed };
    enum class PromptPurpose { Username, Authentication };

    void applyMouseClipboard();
    void createTerminal();
    // always: open the menu even when right-click is set to paste.
    void showTerminalMenu(const QPoint &position, bool always = false);
    void paste(bool middleClick);
    FileDropKind m_fileDropKind = FileDropKind::None;
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
    void startStream();
    void handleStreamDisconnected(const QString &reason, bool error);
    void reconnect();
    void logOutput(const QByteArray &data);
    QByteArray highlight(const QByteArray &data);
    void followReportedFolder(const QByteArray &data);
    void reportLocalTyping();

    QTermWidget *m_terminal = nullptr;
    // SFTP follows the shell: asks the server shortly after each command,
    // unless the shell reports its folder itself (OSC 7).
    QTimer *m_folderTimer = nullptr;
    bool m_folderReported = false;
    // The folder the shell last told (OSC 7 or a folder query): used for
    // dropped files when the server cannot be asked.
    QString m_lastShellFolder;
    // Files dropped on an SSH tab, waiting for the server's answer.
    QStringList m_droppedFiles;
    void uploadDropped(const QString &answer);
    SessionEndedPanel *m_endedBar = nullptr;
    QWidget *m_multiExecBar = nullptr;
    bool m_multiExecTarget = false;
    QAction *m_zoomInAction = nullptr;
    QAction *m_zoomOutAction = nullptr;
    QAction *m_resetZoomAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_findAction = nullptr;
    QAction *m_clearAction = nullptr;
    TerminalBehaviour m_behaviour;
    QFile *m_log = nullptr;
    QString m_logName;
    std::unique_ptr<CastRecorder> m_recorder;
    LoggingPreferences m_logging;
    bool m_logAtLineStart = true;
    struct CompiledRule
    {
        QRegularExpression expression;
        QByteArray start;
    };
    QList<CompiledRule> m_highlights;
    AutomationRunner *m_automation = nullptr;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    bool m_alternateScreen = false;
    int m_reconnectAttempts = 0;
    bool m_wasConnected = false;
    SshSession *m_ssh = nullptr;
    StreamConnection *m_stream = nullptr;
    QPointer<JumpChain> m_jumpChain;
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
