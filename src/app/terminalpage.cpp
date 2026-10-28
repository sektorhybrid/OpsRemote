// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "terminalpage.h"
#include "sessionendedpanel.h"
#include "sftppanel.h"

#include <QAction>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QDir>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QTimer>
#include <QUrl>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <qtermwidget.h>

#include <algorithm>
#include <utility>

namespace {
void wipe(QByteArray &bytes)
{
    bytes.fill('\0');
    bytes.clear();
}

void wipe(QStringList &values)
{
    for (auto &value : values)
        value.fill(QChar(0));
    values.clear();
}
}

TerminalPage::TerminalPage(const TerminalAppearance &appearance, QWidget *parent)
    : QWidget(parent), m_appearance(appearance)
{
    createTerminal();
    m_terminal->setWorkingDirectory(QDir::homePath());
    m_terminal->setShellProgram(qEnvironmentVariable("SHELL", QStringLiteral("/bin/bash")));
    m_terminal->startShellProgram();
}

TerminalPage::TerminalPage(const SshConnectionSettings &settings,
                           const TerminalAppearance &appearance, QWidget *parent)
    : QWidget(parent), m_sshSettings(settings), m_appearance(appearance)
{
    createTerminal();
    // The remote PTY handles XON/XOFF; keep Ctrl+S/Ctrl+Q flowing to it.
    m_terminal->setFlowControlEnabled(false);
    connect(m_terminal, &QTermWidget::sendData, this, &TerminalPage::handleTerminalInput);
    connect(m_terminal, &QTermWidget::terminalSizeChanged, this, [this](int lines, int columns) {
        m_sshSettings.columns = columns;
        m_sshSettings.rows = lines;
        if (m_ssh)
            m_ssh->resizeTerminal(columns, lines);
    });
    m_terminal->startTerminalTeletype();
    m_sftpPanel = new SftpPanel;
    startSsh();
}

TerminalPage::~TerminalPage()
{
    delete m_sftpPanel.data();
    m_sshSettings.password.fill(QChar(0));
    wipe(m_promptLine);
    wipe(m_promptAnswers);
}

void TerminalPage::setAppearance(const TerminalAppearance &appearance)
{
    if (appearance == m_appearance)
        return;
    m_appearance = appearance;
    applyTerminalAppearance(m_terminal, m_appearance);
}

// Ctrl+wheel zooms instead of scrolling or reaching the remote program.
bool TerminalPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            // Touchpads deliver small steps; zoom once per notch (120 units).
            m_wheelZoomDelta += wheel->angleDelta().y();
            for (; m_wheelZoomDelta >= 120; m_wheelZoomDelta -= 120)
                m_terminal->zoomIn();
            for (; m_wheelZoomDelta <= -120; m_wheelZoomDelta += 120)
                m_terminal->zoomOut();
            wheel->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TerminalPage::setBehaviour(const TerminalBehaviour &behaviour)
{
    m_behaviour = behaviour;
    m_terminal->setHistorySize(behaviour.scrollbackLines == 0 ? -1 : behaviour.scrollbackLines);
    using Shape = QTermWidget::KeyboardCursorShape;
    m_terminal->setKeyboardCursorShape(
        behaviour.cursorShape == QStringLiteral("underline") ? Shape::UnderlineCursor
        : behaviour.cursorShape == QStringLiteral("ibeam")   ? Shape::IBeamCursor
                                                             : Shape::BlockCursor);
    m_terminal->setBlinkingCursor(behaviour.cursorBlinks);
    m_terminal->setScrollBarPosition(
        behaviour.scrollBar == QStringLiteral("left")     ? QTermWidget::ScrollBarLeft
        : behaviour.scrollBar == QStringLiteral("hidden") ? QTermWidget::NoScrollBar
                                                          : QTermWidget::ScrollBarRight);
    m_terminal->setMargin(behaviour.padding);
    m_terminal->setConfirmMultilinePaste(behaviour.confirmMultilinePaste);
    m_terminal->setTrimPastedTrailingNewlines(behaviour.trimPastedNewlines);
    m_terminal->setWordCharacters(behaviour.wordCharacters);
}

void TerminalPage::setShortcuts(const QHash<QString, QKeySequence> &shortcuts)
{
    const auto keypad = [](Qt::Key key) {
        return QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::KeypadModifier, key));
    };
    // Zoom keeps its alternatives (Ctrl+=, keypad) while the main key is the default.
    auto zoomKeys = [&](const QString &id, const QList<QKeySequence> &alternatives) {
        const auto key = shortcuts.value(id);
        const auto &definitions = shortcutDefinitions();
        const auto definition = std::find_if(definitions.cbegin(), definitions.cend(),
                                             [&](const auto &d) { return d.id == id; });
        if (definition != definitions.cend() && key == definition->defaultKey)
            return QList<QKeySequence>{key} + alternatives;
        return key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key};
    };
    m_zoomInAction->setShortcuts(zoomKeys(QStringLiteral("zoom_in"),
                                          {QKeySequence(Qt::CTRL | Qt::Key_Equal),
                                           QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus),
                                           keypad(Qt::Key_Plus)}));
    m_zoomOutAction->setShortcuts(zoomKeys(QStringLiteral("zoom_out"), {keypad(Qt::Key_Minus)}));
    m_resetZoomAction->setShortcuts(zoomKeys(QStringLiteral("zoom_reset"), {keypad(Qt::Key_0)}));
    m_copyAction->setShortcut(shortcuts.value(QStringLiteral("copy")));
    m_pasteAction->setShortcut(shortcuts.value(QStringLiteral("paste")));
    m_findAction->setShortcut(shortcuts.value(QStringLiteral("find")));
    m_clearAction->setShortcut(shortcuts.value(QStringLiteral("clear")));
}

bool TerminalPage::isConnected() const
{
    return !m_ssh ? m_sshSettings.host.isEmpty() : m_inputMode != InputMode::Closed;
}

SftpPanel *TerminalPage::sftpPanel() const
{
    return m_sftpPanel;
}

void TerminalPage::createTerminal()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_terminal = new QTermWidget(0, this);
    m_terminal->setAutoClose(false);
    registerTerminalResources(m_terminal);
    applyTerminalAppearance(m_terminal, m_appearance);
    m_terminal->setContextMenuPolicy(Qt::CustomContextMenu);
    // Above the terminal once an SSH session has ended.
    m_endedBar = new SessionEndedPanel(SessionEndedPanel::Style::Bar, this);
    m_endedBar->hide();
    connect(m_endedBar, &SessionEndedPanel::reconnectRequested, this, [this] {
        if (m_inputMode == InputMode::Closed && !m_ssh) {
            showLocalText(QStringLiteral("\n"));
            startSsh();
        }
        m_terminal->setFocus();
    });
    connect(m_endedBar, &SessionEndedPanel::closeRequested, this, &TerminalPage::closeRequested);
    layout->addWidget(m_endedBar);
    layout->addWidget(m_terminal);
    setFocusProxy(m_terminal);
    setBehaviour(loadTerminalBehaviour());

    // Page-level shortcuts take priority over the terminal, which does not
    // claim Ctrl+=/-/0 for the remote program. Ctrl+= and Ctrl+Shift++ cover
    // layouts where "+" needs Shift.
    auto addZoomAction = [this](const QString &text, const QList<QKeySequence> &shortcuts) {
        auto *action = new QAction(text, this);
        action->setShortcuts(shortcuts);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        return action;
    };
    const auto keypad = [](Qt::Key key) {
        return QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::KeypadModifier, key));
    };
    m_zoomInAction = addZoomAction(QStringLiteral("Zoom In"),
                                   {QKeySequence(Qt::CTRL | Qt::Key_Plus),
                                    QKeySequence(Qt::CTRL | Qt::Key_Equal),
                                    QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus),
                                    keypad(Qt::Key_Plus)});
    connect(m_zoomInAction, &QAction::triggered, m_terminal, &QTermWidget::zoomIn);
    m_zoomOutAction = addZoomAction(QStringLiteral("Zoom Out"),
                                    {QKeySequence(Qt::CTRL | Qt::Key_Minus),
                                     keypad(Qt::Key_Minus)});
    connect(m_zoomOutAction, &QAction::triggered, m_terminal, &QTermWidget::zoomOut);
    m_resetZoomAction = addZoomAction(QStringLiteral("Reset Zoom"),
                                      {QKeySequence(Qt::CTRL | Qt::Key_0), keypad(Qt::Key_0)});
    connect(m_resetZoomAction, &QAction::triggered, this, [this] {
        m_terminal->setTerminalFont(m_appearance.font());
    });
    m_copyAction = addZoomAction(QStringLiteral("Copy"), {});
    connect(m_copyAction, &QAction::triggered, m_terminal, &QTermWidget::copyClipboard);
    m_pasteAction = addZoomAction(QStringLiteral("Paste"), {});
    connect(m_pasteAction, &QAction::triggered, m_terminal, &QTermWidget::pasteClipboard);
    m_findAction = addZoomAction(QStringLiteral("Search…"), {});
    connect(m_findAction, &QAction::triggered, m_terminal, &QTermWidget::toggleShowSearchBar);
    m_clearAction = addZoomAction(QStringLiteral("Clear Terminal"), {});
    connect(m_clearAction, &QAction::triggered, m_terminal, &QTermWidget::clear);
    setShortcuts(loadShortcuts());

    connect(m_terminal, &QTermWidget::copyAvailable, this, [this](bool available) {
        if (available && m_behaviour.copyOnSelect)
            QApplication::clipboard()->setText(m_terminal->selectedText());
    });
    connect(m_terminal, &QTermWidget::urlActivated, this, [this](const QUrl &url, bool fromMenu) {
        if (fromMenu || m_behaviour.ctrlClickOpensLinks)
            QDesktopServices::openUrl(url);
    });
    connect(m_terminal, &QTermWidget::bell, this, [this] {
        if (m_behaviour.bell == QStringLiteral("sound")) {
            QApplication::beep();
        } else if (m_behaviour.bell == QStringLiteral("visual")) {
            // A short flash: the terminal is briefly dimmed.
            m_terminal->setTerminalOpacity(0.6);
            QTimer::singleShot(120, m_terminal, [this] { m_terminal->setTerminalOpacity(1.0); });
        }
        if (m_behaviour.bell != QStringLiteral("none"))
            emit bell();
    });
    // Local shells only; SSH output is handled where it arrives.
    connect(m_terminal, &QTermWidget::receivedData, this, [this](const QString &text) {
        logOutput(text.toLatin1());
        if (m_behaviour.markActivity)
            emit activity();
    });

    connect(m_terminal, &QWidget::customContextMenuRequested,
            this, &TerminalPage::showTerminalMenu);
    for (auto *child : m_terminal->findChildren<QWidget *>()) {
        child->installEventFilter(this);
        child->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(child, &QWidget::customContextMenuRequested, this,
                [this, child](const QPoint &position) {
                    showTerminalMenu(child->mapTo(m_terminal, position));
                });
    }
}

void TerminalPage::showTerminalMenu(const QPoint &position)
{
    // Like PuTTY and MobaXterm: right-click pastes, Shift+right-click opens the menu.
    if (m_behaviour.rightClick == QStringLiteral("paste")
        && !(QApplication::keyboardModifiers() & Qt::ShiftModifier)) {
        m_terminal->pasteClipboard();
        return;
    }
    QMenu menu(this);
    m_copyAction->setEnabled(!m_terminal->selectedText().isEmpty());
    menu.addAction(m_copyAction);
    menu.addAction(m_pasteAction);

    menu.addSeparator();
    menu.addAction(m_findAction);
    menu.addAction(m_clearAction);
    if (const auto snippets = loadSnippets(); !snippets.isEmpty()) {
        auto *snippetMenu = menu.addMenu(QStringLiteral("Send Snippet"));
        for (const auto &snippet : snippets) {
            snippetMenu->addAction(snippet.name, this, [this, snippet] {
                typeText(snippet.text + (snippet.pressEnter ? QStringLiteral("\r") : QString()));
            });
        }
    }
    auto *saveAction = menu.addAction(QStringLiteral("Save Terminal Output…"));
    connect(saveAction, &QAction::triggered, this, &TerminalPage::saveTerminalOutput);

    menu.addSeparator();
    menu.addAction(m_zoomInAction);
    menu.addAction(m_zoomOutAction);
    menu.addAction(m_resetZoomAction);

    const auto urlActions = m_terminal->filterActions(position);
    if (!urlActions.isEmpty()) {
        menu.addSeparator();
        for (auto *action : urlActions)
            menu.addAction(action);
    }
    menu.exec(m_terminal->mapToGlobal(position));
}

void TerminalPage::saveTerminalOutput()
{
    const auto filePath = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save Terminal Output"), QStringLiteral("terminal-output.txt"),
        QStringLiteral("Text files (*.txt);;All files (*)"));
    if (filePath.isEmpty())
        return;

    QSaveFile file(filePath);
    bool saved = file.open(QIODevice::WriteOnly);
    if (saved) {
        m_terminal->saveHistory(&file);
        saved = file.commit();
    }
    if (!saved)
        QMessageBox::warning(this, QStringLiteral("Save Terminal Output"),
                             QStringLiteral("Could not save %1: %2")
                                 .arg(QDir::toNativeSeparators(filePath), file.errorString()));
}

void TerminalPage::startSsh()
{
    if (m_endedBar)
        m_endedBar->hide();
    if (m_sshSettings.username.isEmpty()) {
        beginPrompts(PromptPurpose::Username, QString(), {QStringLiteral("login as: ")}, {true});
        return;
    }

    m_sftpPanel->setTitle(QStringLiteral("%1@%2").arg(m_sshSettings.username,
                                                       m_sshSettings.host));
    m_sshSettings.columns = m_terminal->screenColumnsCount();
    m_sshSettings.rows = m_terminal->screenLinesCount();
    m_inputMode = InputMode::Idle;
    m_ssh = new SshSession(this);
    connect(m_ssh, &SshSession::statusMessage, this, [this](const QString &message) {
        showLocalText(message + QLatin1Char('\n'));
    });
    connect(m_ssh, &SshSession::hostKeyVerificationRequired, this, &TerminalPage::confirmHostKey);
    connect(m_ssh, &SshSession::inputRequired, this,
            [this](const QString &instruction, const QStringList &prompts,
                   const QList<bool> &echo) {
                beginPrompts(PromptPurpose::Authentication, instruction, prompts, echo);
            });
    connect(m_ssh, &SshSession::connected, this, [this] {
        m_inputMode = InputMode::Remote;
        m_wasConnected = true;
        m_reconnectAttempts = 0;
        emit statusMessage(QStringLiteral("Connected to %1").arg(m_sshSettings.host));
    });
    connect(m_ssh, &SshSession::dataReceived, this, [this](const QByteArray &data) {
        logOutput(data);
        const auto shown = highlight(data);
        m_terminal->receiveData(shown.constData(), static_cast<int>(shown.size()));
        if (m_behaviour.markActivity)
            emit activity();
    });
    connect(m_ssh, &SshSession::disconnected, this, &TerminalPage::handleSshDisconnected);
    m_sftpPanel->setSession(m_ssh);
    m_ssh->connectToHost(m_sshSettings);
}

void TerminalPage::handleTerminalInput(const char *data, int size)
{
    const QByteArray bytes(data, size);
    switch (m_inputMode) {
    case InputMode::Remote:
        if (m_ssh) {
            m_ssh->write(bytes);
            emit userInput(bytes);
        }
        break;
    case InputMode::Prompt:
        handlePromptInput(bytes);
        break;
    case InputMode::Closed:
        // Esc alone (not an escape sequence such as an arrow key) closes.
        if (bytes == "\x1b") {
            emit closeRequested();
            break;
        }
        if (bytes.contains('\r') || bytes.contains('\n')) {
            showLocalText(QStringLiteral("\n"));
            startSsh();
        }
        break;
    case InputMode::Idle:
        break;
    }
}

void TerminalPage::showLocalText(const QString &text)
{
    auto normalized = text;
    normalized.remove(QLatin1Char('\r'));
    normalized.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    const auto bytes = normalized.toUtf8();
    m_terminal->receiveData(bytes.constData(), static_cast<int>(bytes.size()));
}

void TerminalPage::confirmHostKey(SshHostKeyStatus status, const QString &keyType,
                                  const QString &fingerprint)
{
    const auto host = m_sshSettings.port == 22
        ? m_sshSettings.host
        : QStringLiteral("%1:%2").arg(m_sshSettings.host).arg(m_sshSettings.port);
    const auto keyDetails = QStringLiteral("%1 key fingerprint:\n%2").arg(keyType, fingerprint);

    QMessageBox dialog(this);
    dialog.setWindowTitle(QStringLiteral("Verify SSH Host Key"));
    QPushButton *saveButton = nullptr;
    switch (status) {
    case SshHostKeyStatus::Unknown:
        dialog.setIcon(QMessageBox::Question);
        dialog.setText(QStringLiteral("The authenticity of %1 can't be established.").arg(host));
        dialog.setInformativeText(
            QStringLiteral("%1\n\nOnly connect if this matches the fingerprint published by the "
                           "server's administrator.").arg(keyDetails));
        saveButton = dialog.addButton(QStringLiteral("Trust and Connect"),
                                      QMessageBox::AcceptRole);
        break;
    case SshHostKeyStatus::Changed:
        dialog.setIcon(QMessageBox::Critical);
        dialog.setText(QStringLiteral("WARNING: the host key for %1 has changed!").arg(host));
        dialog.setInformativeText(
            QStringLiteral("Someone could be intercepting this connection (a man-in-the-middle "
                           "attack), or the server's key was replaced.\n\nNew %1\n\nDo not "
                           "continue unless you have confirmed the new key with the server's "
                           "administrator.").arg(keyDetails));
        saveButton = dialog.addButton(QStringLiteral("Replace Saved Key and Connect"),
                                      QMessageBox::DestructiveRole);
        break;
    case SshHostKeyStatus::OtherType:
        dialog.setIcon(QMessageBox::Warning);
        dialog.setText(QStringLiteral("%1 presented a different type of host key than the one "
                                      "saved for it.").arg(host));
        dialog.setInformativeText(
            QStringLiteral("%1\n\nThis can happen after a server upgrade, but it can also "
                           "indicate an attack. Only connect if you can verify this key.")
                .arg(keyDetails));
        saveButton = dialog.addButton(QStringLiteral("Trust and Connect"),
                                      QMessageBox::AcceptRole);
        break;
    }
    auto *onceButton = dialog.addButton(QStringLiteral("Connect Once"), QMessageBox::AcceptRole);
    auto *rejectButton = dialog.addButton(QStringLiteral("Disconnect"), QMessageBox::RejectRole);
    dialog.setDefaultButton(status == SshHostKeyStatus::Unknown ? saveButton : rejectButton);
    dialog.setEscapeButton(rejectButton);
    dialog.exec();

    if (!m_ssh)
        return;
    if (dialog.clickedButton() == saveButton)
        m_ssh->answerHostKey(SshHostKeyDecision::AcceptAndSave);
    else if (dialog.clickedButton() == onceButton)
        m_ssh->answerHostKey(SshHostKeyDecision::AcceptOnce);
    else
        m_ssh->answerHostKey(SshHostKeyDecision::Reject);
}

void TerminalPage::beginPrompts(PromptPurpose purpose, const QString &instruction,
                                const QStringList &prompts, const QList<bool> &echo)
{
    m_promptPurpose = purpose;
    m_prompts = prompts;
    m_promptEcho = echo;
    wipe(m_promptAnswers);
    if (!instruction.isEmpty())
        showLocalText(instruction + QLatin1Char('\n'));
    m_inputMode = InputMode::Prompt;
    showNextPrompt();
}

void TerminalPage::showNextPrompt()
{
    wipe(m_promptLine);
    showLocalText(m_prompts.value(m_promptAnswers.size()));
}

// Minimal line editor for prompts answered locally before the remote shell
// starts. Input is never echoed unless the server marked the prompt as echoed.
void TerminalPage::handlePromptInput(const QByteArray &data)
{
    const bool echo = m_promptEcho.value(m_promptAnswers.size(), false);
    for (qsizetype index = 0; index < data.size(); ++index) {
        const auto byte = static_cast<unsigned char>(data.at(index));
        if (byte == '\r' || byte == '\n') {
            submitPromptLine();
            return;
        }
        if (byte == 0x03 || (byte == 0x04 && m_promptLine.isEmpty())) {
            cancelPrompts();
            return;
        }
        if (byte == 0x1b) {
            // Ignore cursor and function keys: ESC [ ... final or ESC O final.
            if (index + 1 < data.size() && (data.at(index + 1) == '[' || data.at(index + 1) == 'O')) {
                index += 2;
                while (index < data.size()
                       && !(data.at(index) >= 0x40 && data.at(index) <= 0x7e))
                    ++index;
            } else {
                ++index;
            }
            continue;
        }
        if (byte == 0x7f || byte == 0x08) {
            if (m_promptLine.isEmpty())
                continue;
            while (!m_promptLine.isEmpty()
                   && (static_cast<unsigned char>(m_promptLine.back()) & 0xc0) == 0x80)
                m_promptLine.chop(1);
            m_promptLine.chop(1);
            if (echo)
                showLocalText(QStringLiteral("\b \b"));
            continue;
        }
        if (byte == 0x15) {
            if (echo) {
                for (qsizetype count = QString::fromUtf8(m_promptLine).size(); count > 0; --count)
                    showLocalText(QStringLiteral("\b \b"));
            }
            wipe(m_promptLine);
            continue;
        }
        if (byte < 0x20)
            continue;
        m_promptLine.append(static_cast<char>(byte));
        if (echo)
            m_terminal->receiveData(data.constData() + index, 1);
    }
}

void TerminalPage::submitPromptLine()
{
    showLocalText(QStringLiteral("\n"));
    m_promptAnswers.append(QString::fromUtf8(m_promptLine));
    wipe(m_promptLine);
    if (m_promptAnswers.size() < m_prompts.size()) {
        showNextPrompt();
        return;
    }

    m_inputMode = InputMode::Idle;
    if (m_promptPurpose == PromptPurpose::Username) {
        m_sshSettings.username = m_promptAnswers.value(0).trimmed();
        wipe(m_promptAnswers);
        startSsh();
        return;
    }
    if (m_ssh)
        m_ssh->answerPrompts(m_promptAnswers);
    wipe(m_promptAnswers);
}

void TerminalPage::cancelPrompts()
{
    showLocalText(QStringLiteral("^C\n"));
    wipe(m_promptLine);
    wipe(m_promptAnswers);
    if (m_promptPurpose == PromptPurpose::Authentication && m_ssh) {
        m_inputMode = InputMode::Idle;
        m_ssh->cancelPrompts();
        return;
    }
    handleSshDisconnected(QStringLiteral("Login cancelled."), false);
}

void TerminalPage::handleSshDisconnected(const QString &reason, bool error)
{
    if (m_ssh) {
        m_ssh->deleteLater();
        m_ssh = nullptr;
    }
    if (m_sftpPanel)
        m_sftpPanel->setSession(nullptr);
    m_inputMode = InputMode::Closed;
    // Reset attributes, then show the reason in bold (red for errors).
    showLocalText(QStringLiteral("\x1b[0m\n\x1b[1%1m%2\x1b[0m\nPress Enter to reconnect, or Esc to close the tab.\n")
                      .arg(error ? QStringLiteral(";31") : QString(), reason));
    m_endedBar->setMessage(QStringLiteral("Disconnected from %1").arg(m_sshSettings.host), reason,
                           error);
    m_endedBar->show();
    emit statusMessage(QStringLiteral("%1: %2").arg(m_sshSettings.host, reason));

    // A dropped connection (not a failed login or a clean logout) reconnects
    // by itself when enabled.
    const auto &ssh = m_sshSettings;
    if (ssh.autoReconnect && error && m_wasConnected
        && m_reconnectAttempts < ssh.reconnectAttempts) {
        ++m_reconnectAttempts;
        showLocalText(QStringLiteral("Reconnecting in %1 s (attempt %2 of %3)…\n")
                          .arg(ssh.reconnectDelay)
                          .arg(m_reconnectAttempts)
                          .arg(ssh.reconnectAttempts));
        QTimer::singleShot(ssh.reconnectDelay * 1000, this, [this] {
            if (m_inputMode == InputMode::Closed && !m_ssh)
                startSsh();
        });
    }
}

void TerminalPage::setLogging(const LoggingPreferences &preferences, const QString &name)
{
    m_logging = preferences;
    if (!preferences.enabled) {
        delete m_log;
        m_log = nullptr;
        return;
    }
    if (m_log)
        return;
    const QString folder = sessionLogFolder(preferences);
    QDir().mkpath(folder);
    QString safeName = name;
    safeName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._@-]+")), QStringLiteral("_"));
    const auto path = QDir(folder).filePath(
        QStringLiteral("%1_%2.log")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")),
                 safeName.isEmpty() ? QStringLiteral("terminal") : safeName));
    m_log = new QFile(path, this);
    if (!m_log->open(QIODevice::Append)) {
        emit statusMessage(QStringLiteral("Cannot write the session log %1")
                               .arg(QDir::toNativeSeparators(path)));
        delete m_log;
        m_log = nullptr;
        return;
    }
    m_logAtLineStart = true;
}

void TerminalPage::logOutput(const QByteArray &data)
{
    if (!m_log)
        return;
    QByteArray text = data;
    if (m_logging.plainText) {
        // Escape sequences (colours, cursor movement, window titles) and
        // carriage returns make a log unreadable in a text editor.
        static const QRegularExpression escapes(QStringLiteral(
            "\x1b\\[[0-?]*[ -/]*[@-~]|\x1b\\][^\x07\x1b]*(?:\x07|\x1b\\\\)|\x1b[()][0-9A-Za-z]|\x1b[@-Z\\\\-_=>]|\r|\x07"));
        text = QString::fromUtf8(text).remove(escapes).toUtf8();
    }
    if (m_logging.timestamps) {
        QByteArray stamped;
        for (const char c : std::as_const(text)) {
            if (m_logAtLineStart) {
                stamped += QDateTime::currentDateTime()
                               .toString(QStringLiteral("[HH:mm:ss] "))
                               .toUtf8();
                m_logAtLineStart = false;
            }
            stamped += c;
            if (c == '\n')
                m_logAtLineStart = true;
        }
        text = stamped;
    }
    m_log->write(text);
    m_log->flush();
}

void TerminalPage::setHighlightRules(const QList<HighlightRule> &rules)
{
    m_highlights.clear();
    for (const auto &rule : rules) {
        if (!rule.enabled)
            continue;
        QRegularExpression expression(
            rule.pattern, rule.caseSensitive ? QRegularExpression::NoPatternOption
                                             : QRegularExpression::CaseInsensitiveOption);
        if (!expression.isValid())
            continue;
        QByteArray start = QStringLiteral("\x1b[38;2;%1;%2;%3m")
                               .arg(rule.color.red())
                               .arg(rule.color.green())
                               .arg(rule.color.blue())
                               .toLatin1();
        if (rule.bold)
            start += "\x1b[1m";
        m_highlights.append({expression, start});
    }
}

// Colours rule matches in plain text, leaving escape sequences alone. Full
// screen programs (the alternate screen: vim, top, less) are not touched.
QByteArray TerminalPage::highlight(const QByteArray &data)
{
    if (m_highlights.isEmpty())
        return data;
    const QString text = m_decoder.decode(data);
    static const QRegularExpression escape(QStringLiteral(
        "\x1b\\[[0-?]*[ -/]*[@-~]|\x1b\\][^\x07\x1b]*(?:\x07|\x1b\\\\)|\x1b."));
    QByteArray result;
    qsizetype position = 0;
    auto addText = [&](const QString &plain) {
        if (m_alternateScreen) {
            result += plain.toUtf8();
            return;
        }
        // Mark the characters covered by the first matching rule.
        QList<int> owner(plain.size(), -1);
        for (int index = 0; index < m_highlights.size(); ++index) {
            auto matches = m_highlights[index].expression.globalMatch(plain);
            while (matches.hasNext()) {
                const auto match = matches.next();
                for (qsizetype c = match.capturedStart(); c < match.capturedEnd(); ++c) {
                    if (owner[c] < 0)
                        owner[c] = index;
                }
            }
        }
        int current = -1;
        for (qsizetype c = 0; c < plain.size(); ++c) {
            if (owner[c] != current) {
                if (current >= 0)
                    result += "\x1b[39;22m";
                if (owner[c] >= 0)
                    result += m_highlights[owner[c]].start;
                current = owner[c];
            }
            result += QString(plain[c]).toUtf8();
        }
        if (current >= 0)
            result += "\x1b[39;22m";
    };
    auto matches = escape.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        addText(text.mid(position, match.capturedStart() - position));
        const auto sequence = match.captured();
        if (sequence == QStringLiteral("\x1b[?1049h") || sequence == QStringLiteral("\x1b[?1047h")
            || sequence == QStringLiteral("\x1b[?47h"))
            m_alternateScreen = true;
        else if (sequence == QStringLiteral("\x1b[?1049l") || sequence == QStringLiteral("\x1b[?1047l")
                 || sequence == QStringLiteral("\x1b[?47l"))
            m_alternateScreen = false;
        result += sequence.toUtf8();
        position = match.capturedEnd();
    }
    addText(text.mid(position));
    return result;
}

void TerminalPage::typeText(const QString &text)
{
    m_terminal->sendText(text);
    m_terminal->setFocus();
}

void TerminalPage::sendInput(const QByteArray &data)
{
    if (m_ssh && m_inputMode == InputMode::Remote)
        m_ssh->write(data);
    else if (!m_ssh && m_sshSettings.host.isEmpty())
        m_terminal->sendText(QString::fromUtf8(data));
}

void TerminalPage::setZoomSteps(int steps)
{
    m_terminal->setTerminalFont(m_appearance.font());
    for (int step = 0; step < steps; ++step)
        m_terminal->zoomIn();
}
