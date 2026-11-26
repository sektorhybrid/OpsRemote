// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// MainWindow: opening sessions in tabs: local terminals and programs, containers, session tabs and their menus, activity marks, startup tabs.

#include "mainwindow_p.h"

using namespace mainwindow_p;

namespace {

class SessionPage final : public QWidget
{
public:
    explicit SessionPage(const SessionDefinition &session, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(32, 28, 32, 28);

        m_title = new QLabel(session.name, this);
        applyTitleFont();

        auto *details = new QLabel(
            QStringLiteral("%1  ·  %2%3")
                .arg(session.protocol, session.host,
                     session.port == 0 ? QString() : QStringLiteral(":%1").arg(session.port)),
            this);
        details->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto *placeholder = new QLabel(
            QStringLiteral("This tab is reserved for one session.\n"
                           "The %1 connection backend has not been implemented yet.")
                .arg(session.protocol),
            this);
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

        layout->addWidget(m_title);
        layout->addWidget(details);
        layout->addStretch();
        layout->addWidget(placeholder, 0, Qt::AlignCenter);
        layout->addStretch();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange)
            applyTitleFont();
        QWidget::changeEvent(event);
    }

private:
    void applyTitleFont()
    {
        QFont titleFont = font();
        titleFont.setPointSize(titleFont.pointSize() + 5);
        titleFont.setBold(true);
        m_title->setFont(titleFont);
    }

    QLabel *m_title = nullptr;
};

} // namespace

void MainWindow::openLocalTerminal()
{
    const auto shell = localShellCommand(QString());
    TerminalPage *page = shell.program.isEmpty()
                             ? new TerminalPage(effectiveTerminalAppearance(), m_sessionTabs)
                             : newLocalProgramPage(shell.program, shell.arguments, shell.label,
                                                   effectiveTerminalAppearance());
    if (Platform::has(Platform::Feature::LocalFileDrop))
        page->setFileDropKind(TerminalPage::FileDropKind::Local); // dropped files go to its folder
    connectTerminalPage(page);
    applyTabSettings(page, QStringLiteral("local"));
    addSessionTab(page, shell.label, QStringLiteral("Local shell: %1").arg(shell.label));
}

// Session tree → Open a Container…: a Docker (or Kubernetes) shell on an
// SSH server, chosen from its containers, opened once (or saved).
void MainWindow::openContainerOn(const QString &serverId)
{
    const auto server = m_sessions.value(serverId);
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Open a Container on %1").arg(server.name));
    dialog.setMinimumWidth(480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *kind = new QComboBox(&dialog);
    kind->addItems({dockerProtocol(), kubernetesProtocol()});
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Type:"), kind);
    layout->addLayout(form);
    LocalSessionOptions options;
    options.via = serverId;
    auto *editor = new LocalSessionEditor(
        options, [server] { return QList<QPair<QString, QString>>{{server.id, server.name}}; },
        [this, server](const QString &) -> std::optional<SshConnectionSettings> {
            SshConnectionSettings settings;
            if (!sshSettingsForSession(server, &settings) || !settings.jumps.empty())
                return std::nullopt;
            return settings;
        },
        &dialog);
    editor->setProtocol(kind->currentText());
    connect(kind, &QComboBox::currentTextChanged, editor, &LocalSessionEditor::setProtocol);
    layout->addWidget(editor);
    auto *save = new QCheckBox(QStringLiteral("Also save it as a session, next to the server"), &dialog);
    layout->addWidget(save);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    SessionDefinition session;
    session.protocol = kind->currentText();
    session.local = editor->value();
    const auto target = session.protocol == dockerProtocol() ? session.local.container : session.local.pod;
    session.name = QStringLiteral("%1 on %2").arg(target.isEmpty() ? session.protocol : target, server.name);
    if (save->isChecked()) {
        session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QTreeWidgetItem *folder = nullptr;
        for (QTreeWidgetItemIterator it(m_sessionTree); *it && !folder; ++it) {
            if ((*it)->data(0, SessionIdRole).toString() == serverId && !isQuickItem(*it))
                folder = (*it)->parent();
        }
        addSession(session, folder);
        saveSessions();
    } else {
        session.id = QStringLiteral("container-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    openSession(session);
}

// The terminal of a Local (also as administrator), WSL, Docker or
// Kubernetes session; nullptr when it cannot open (the reason has been shown).
TerminalPage *MainWindow::openLocalSession(const SessionDefinition &session)
{
    const auto appearance = effectiveTerminalAppearance().overriddenBy(session.terminal);
    const auto &options = session.local;
    const auto refuse = [this, &session](const QString &why) {
        QMessageBox::information(this, session.name, why);
        return nullptr;
    };
    TerminalPage *terminal = nullptr;
    if (session.protocol == QStringLiteral("Local") && options.administrator) {
        QString label;
        terminal = administratorTerminal(options, appearance, &label);
        if (!terminal)
            return nullptr;
        connectTerminalPage(terminal);
        applyTabSettings(terminal, session.name);
        applySessionAutomation(terminal, session);
        return terminal;
    }
    const bool viaSsh = (session.protocol == dockerProtocol() || session.protocol == kubernetesProtocol())
                        && runsOverSsh(options);
    if (const auto problem = localSessionProblem(session.protocol, options.engine); !problem.isEmpty() && !viaSsh)
        return refuse(problem);
    if (session.protocol == QStringLiteral("Local")) {
        auto shell = localShellCommand(options.localShell, options.program);
        if (shell.program.isEmpty() && !options.localShell.isEmpty())
            return refuse(QStringLiteral("%1 is not installed on this computer; choose another shell "
                                         "on the session's Connection tab.").arg(shell.label));
#ifndef Q_OS_WIN
        // The start folder: through a shell that changes there first.
        if (!options.folder.isEmpty()) {
            const auto program = shell.program.isEmpty() ? qEnvironmentVariable("SHELL", QStringLiteral("/bin/sh"))
                                                         : shell.program;
            QStringList command{program};
            command += shell.arguments;
            for (auto &part : command)
                part = QLatin1Char('\'') + part.replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
            shell.arguments = {QStringLiteral("-c"),
                               QStringLiteral("cd '%1' 2>/dev/null; exec %2")
                                   .arg(QString(options.folder).replace(QLatin1Char('\''), QStringLiteral("'\\''")),
                                        command.join(QLatin1Char(' ')))};
            shell.program = QStringLiteral("/bin/sh");
        }
#endif
        terminal = shell.program.isEmpty()
                       ? new TerminalPage(appearance, m_sessionTabs)
                       : newLocalProgramPage(shell.program, shell.arguments, shell.label, appearance, options.folder);
        if (Platform::has(Platform::Feature::LocalFileDrop))
            terminal->setFileDropKind(TerminalPage::FileDropKind::Local); // dropped files go to its folder
    } else if (session.protocol == QStringLiteral("WSL")) {
        const auto command = wslCommand(options);
        terminal = newLocalProgramPage(command.program, command.arguments, QStringLiteral("WSL"), appearance);
    } else {
        const auto preferences = loadContainerPreferences();
        const bool docker = session.protocol == dockerProtocol();
        if (docker ? options.container.isEmpty() : options.pod.isEmpty())
            return refuse(docker ? QStringLiteral("Choose the container on the session's Connection tab.")
                                 : QStringLiteral("Choose the pod on the session's Connection tab."));
        QString secret;
        if (!docker && options.kubeAuth == QStringLiteral("saved")) {
            secret = kubeCredentialContents(options.kubeCredential);
            if (secret.isEmpty())
                return refuse(QStringLiteral("The Kubernetes credential \"%1\" could not be read (deleted, or the "
                                             "vault is locked).").arg(options.kubeCredential));
        } else if (!docker && options.kubeAuth == QStringLiteral("token")) {
            if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked())
                return nullptr;
            secret = m_vault.passwordForSession(session.id);
            if (secret.isEmpty())
                return refuse(QStringLiteral("The session has no token: type it as the session's password."));
        }
        const auto command = docker ? dockerCommand(options, preferences) : kubernetesCommand(options, preferences, secret);
        secret.fill(QChar(0));
        // kubectl reads the written kubeconfig when it starts.
        if (!command.temporaryFile.isEmpty())
            QTimer::singleShot(60000, this, [file = command.temporaryFile] { QFile::remove(file); });
        if (viaSsh) {
            const auto server = m_sessions.constFind(options.via);
            if (server == m_sessions.cend())
                return refuse(QStringLiteral("The SSH server this session runs on was deleted; choose another."));
            SshConnectionSettings settings;
            if (!sshSettingsForSession(*server, &settings))
                return nullptr;
            settings.remoteCommand = command.remoteCommand;
            settings.remoteFile = command.remoteFile;
            terminal = new TerminalPage(settings, appearance, m_sessionTabs);
            settings.password.fill(QChar(0));
        } else {
            terminal = newLocalProgramPage(command.program, command.arguments, session.protocol, appearance);
        }
    }
    // Saved scripts are sent into the container's shell (see runSavedScriptInTab).
    if (session.protocol == dockerProtocol() || session.protocol == kubernetesProtocol())
        terminal->setProperty("container", true);
    connectTerminalPage(terminal);
    applyTabSettings(terminal, session.name);
    applySessionAutomation(terminal, session);
    return terminal;
}

void MainWindow::openLocalProgram(const QString &program, const QStringList &arguments,
                                  const QString &label)
{
    auto *page = newLocalProgramPage(program, arguments, label, effectiveTerminalAppearance());
    connectTerminalPage(page);
    applyTabSettings(page, QStringLiteral("local"));
    addSessionTab(page, label, QStringLiteral("%1 %2").arg(program, arguments.join(QLatin1Char(' '))));
}

// A local program in a terminal tab: a PTY on Linux, a pseudo console on
// Windows.
TerminalPage *MainWindow::newLocalProgramPage(const QString &program, const QStringList &arguments,
                                              const QString &label,
                                              const TerminalAppearance &appearance,
                                              const QString &folder)
{
#ifdef Q_OS_WIN
    return new TerminalPage(new ConPtyConnection(program, arguments, label,
                                                 folder.isEmpty() ? QDir::homePath() : folder),
                            appearance, m_sessionTabs);
#else
    Q_UNUSED(label);
    Q_UNUSED(folder); // Linux: the caller starts the program in the folder.
    return new TerminalPage(program, arguments, appearance, m_sessionTabs);
#endif
}

// File → New Local Terminal (Administrator): the default shell as
// administrator, in a tab like any other.
void MainWindow::openLocalTerminalAsAdministrator()
{
    QString label;
    auto *page = administratorTerminal({}, effectiveTerminalAppearance(), &label);
    if (!page)
        return;
    connectTerminalPage(page);
    applyTabSettings(page, QStringLiteral("local-administrator"));
    addSessionTab(page, label, label);
    if (const int index = m_sessionTabs->indexOf(page); index >= 0)
        m_sessionTabs->setTabIcon(index, style()->standardIcon(QStyle::SP_VistaShield));
}

// Windows: through Windows sudo when it is on in inline mode (and chosen),
// otherwise through the elevated helper (ElevatedConsoleConnection), after
// UAC. Linux: through sudo (in the terminal) or pkexec (a system window).
// nullptr when it cannot start (the reason has been shown).
TerminalPage *MainWindow::administratorTerminal(const LocalSessionOptions &options,
                                                const TerminalAppearance &appearance, QString *label)
{
    const auto shell = localShellCommand(options.localShell, options.program);
#ifdef Q_OS_WIN
    if (shell.program.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Administrator Terminal"),
                                 QStringLiteral("%1 is not installed on this computer.").arg(shell.label));
        return nullptr;
    }
    *label = QStringLiteral("Administrator: %1").arg(shell.label);
    const QString start = options.folder.isEmpty() ? QDir::homePath() : options.folder;
    const auto method = options.elevation.isEmpty()
                            ? (m_localShell.useSudo ? QStringLiteral("sudo") : QStringLiteral("helper"))
                            : options.elevation;
    StreamConnection *connection = nullptr;
    if (method == QStringLiteral("sudo") && windowsSudoInlineEnabled())
        connection = new ConPtyConnection(windowsSudoPath(), QStringList{shell.program} + shell.arguments,
                                          *label, start);
    else
        connection = new ElevatedConsoleConnection(shell.program, shell.arguments, *label, start);
    return new TerminalPage(connection, appearance, m_sessionTabs);
#else
    const auto method = options.elevation.isEmpty() ? m_localShell.linuxElevation : options.elevation;
    const auto elevator = QStandardPaths::findExecutable(method);
    if (elevator.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Administrator Terminal"),
                                 QStringLiteral("%1 is not installed on this computer; choose the other way "
                                                "on the session's Connection tab, or in Preferences → "
                                                "Terminals → Administrator.").arg(method));
        return nullptr;
    }
    *label = QStringLiteral("root: %1").arg(shell.program.isEmpty() ? QStringLiteral("shell") : shell.label);
    const auto quote = [](QString text) {
        return QLatin1Char('\'') + text.replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
    };
    QStringList arguments;
    if (shell.program.isEmpty() && options.folder.isEmpty() && method == QStringLiteral("sudo")) {
        arguments << QStringLiteral("-i"); // root's login shell
    } else {
        // pkexec needs a full path; the login shell is $SHELL.
        const auto program = shell.program.isEmpty() ? qEnvironmentVariable("SHELL", QStringLiteral("/bin/sh"))
                                                     : shell.program;
        if (options.folder.isEmpty()) {
            arguments << program << shell.arguments;
        } else {
            QStringList command{quote(program)};
            for (const auto &argument : shell.arguments)
                command << quote(argument);
            arguments << QStringLiteral("/bin/sh") << QStringLiteral("-c")
                      << QStringLiteral("cd %1 2>/dev/null; exec %2").arg(quote(options.folder), command.join(QLatin1Char(' ')));
        }
    }
    return newLocalProgramPage(elevator, arguments, *label, appearance);
#endif
}

void MainWindow::setLocalShellPreferences(const LocalShellPreferences &preferences)
{
    m_localShell = preferences;
    saveLocalShellPreferences(preferences);
}

void MainWindow::openSelectedSession()
{
    auto *item = selectedItem();
    if (!item || isFolder(item)) {
        notify(QStringLiteral("Select a saved session first"));
        return;
    }

    const auto sessionId = item->data(0, SessionIdRole).toString();
    const auto session = m_sessions.constFind(sessionId);
    if (session != m_sessions.cend())
        openSession(*session);
}

void MainWindow::closeSessionTab(int index)
{
    auto *page = m_sessionTabs->widget(index);
    if (!page)
        return;
    // Remote sessions ask first; local shells close directly.
    auto *terminal = qobject_cast<TerminalPage *>(page);
    auto *screen = qobject_cast<RemoteScreenPage *>(page);
    QString host = terminal ? terminal->sshSettings().host : screen ? screen->host() : QString();
    if (auto *split = qobject_cast<SplitTab *>(page))
        host = QStringLiteral("%1 sessions").arg(split->panes().size());
    if (m_startup.confirmCloseConnected && tabIsConnected(page) && !host.isEmpty()
        && QMessageBox::question(this, QStringLiteral("Close Tab"),
                                 QStringLiteral("Disconnect from %1 and close the tab?").arg(host))
               != QMessageBox::Yes)
        return;
    m_sessionTabs->removeTab(index);
    page->deleteLater();
}

// Whether closing this tab would end a live session: a local shell, or a
// connected SSH or remote desktop session.
bool MainWindow::tabIsConnected(QWidget *page) const
{
    if (auto *split = qobject_cast<SplitTab *>(page)) {
        for (auto *pane : split->panes()) {
            if (pane->isConnected())
                return true;
        }
        return false;
    }
    if (auto *terminal = qobject_cast<TerminalPage *>(page))
        return terminal->isConnected();
    if (auto *screen = qobject_cast<RemoteScreenPage *>(page))
        return screen->isConnected();
    return false;
}

void MainWindow::openSession(const SessionDefinition &session)
{
    if (!m_startup.allowDuplicateSessionTabs) {
        for (int index = 0; index < m_sessionTabs->count(); ++index) {
            auto *page = m_sessionTabs->widget(index);
            bool open = page && page->property("sessionId").toString() == session.id;
            for (auto *pane : terminalPagesIn(page))
                open = open || pane->property("sessionId").toString() == session.id;
            if (open) {
                m_sessionTabs->setCurrentIndex(index);
                return;
            }
        }
    }

    // Wake the computer first, then come back here after the wait.
    const auto &wake = session.wakeOnLan;
    if (wake.isSet() && wake.beforeConnecting && !m_waking.contains(session.id)) {
        QString error;
        if (!wake.send(&error)) {
            notify(QStringLiteral("Wake-on-LAN failed: %1").arg(error), true);
        } else {
            m_waking.insert(session.id);
            notify(QStringLiteral("Waking %1; connecting in %2 s…")
                   .arg(session.name).arg(wake.effectiveWait()));
            QTimer::singleShot(wake.effectiveWait() * 1000, this, [this, session] {
                const auto current = m_sessions.value(session.id, session);
                openSession(current);
                m_waking.remove(session.id);
            });
            return;
        }
    }

    QWidget *page = nullptr;
    if (session.protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0) {
        auto settings = sshSettingsFor(session.host, session.port, session.username, sessionSshOptions(session));
        // A locked vault must be unlocked before a session opens (when its
        // password is kept there); a wrong master password or cancelling the
        // prompt does not open the session.
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            notify(QStringLiteral("Session not opened: the credential vault is locked"), true);
            return;
        }
        if (!resolveJumpHosts(session, &settings.jumps))
            return;
        applyProxy(session, &settings, settings.jumps);
        settings.forwards = session.forwards;
        settings.password = m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0));
        auto *terminal = new TerminalPage(
            settings, effectiveTerminalAppearance().overriddenBy(session.terminal), m_sessionTabs);
        settings.password.fill(QChar(0));
        for (auto &jump : settings.jumps)
            jump.password.fill(QChar(0));
        connectTerminalPage(terminal);
        applyTabSettings(terminal, session.name);
        applySessionAutomation(terminal, session);
        page = terminal;
    } else if (session.protocol.compare(QStringLiteral("RDP"), Qt::CaseInsensitive) == 0) {
        RdpConnectionSettings settings;
        settings.host = session.host;
        settings.port = session.port == 0 ? 3389 : session.port;
        // "DOMAIN\user" or "user@domain" both work; FreeRDP needs them apart.
        settings.username = session.username;
        if (const int slash = session.username.indexOf(QLatin1Char('\\')); slash > 0) {
            settings.domain = session.username.left(slash);
            settings.username = session.username.mid(slash + 1);
        }
        settings.configPath = stateFilePath(QStringLiteral("rdp"));
        settings.options = loadRdpDefaults().overriddenBy(session.rdp);
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            notify(QStringLiteral("Session not opened: the credential vault is locked"), true);
            return;
        }
        if (!resolveJumpHosts(session, &settings.jumps))
            return;
        applyProxy(session, nullptr, settings.jumps);
        settings.password = m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0));
        auto *rdp = new RdpPage(settings, m_sessionTabs);
        settings.password.fill(QChar(0));
        for (auto &jump : settings.jumps)
            jump.password.fill(QChar(0));
        connect(rdp, &RdpPage::statusMessage, this, [this, rdp](const QString &message) {
            tabMessage(rdp, message);
        });
        page = rdp;
    } else if (session.protocol.compare(QStringLiteral("VNC"), Qt::CaseInsensitive) == 0) {
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            notify(QStringLiteral("Session not opened: the credential vault is locked"), true);
            return;
        }
        VncPageSettings settings;
        auto &connection = settings.connection;
        connection.host = session.host;
        connection.port = session.port == 0 ? 5900 : session.port;
        // "host:1" is display 1, port 5901 (a bare IPv6 address has more colons).
        if (session.port == 0 && session.host.count(QLatin1Char(':')) == 1) {
            bool ok = false;
            const int display = session.host.section(QLatin1Char(':'), 1).toInt(&ok);
            if (ok && display >= 0 && display < 100) {
                connection.host = session.host.section(QLatin1Char(':'), 0, 0);
                connection.port = quint16(5900 + display);
            }
        }
        connection.username = session.username;
        connection.options = loadVncDefaults().overriddenBy(session.vnc);
        const QString baseId = session.id.section(QLatin1Char('#'), 0, 0);
        connection.password = m_vault.passwordForSession(baseId);
        settings.canSavePassword = m_security.offerToSave;
        if (!resolveJumpHosts(session, &settings.jumps))
            return;
        applyProxy(session, nullptr, settings.jumps);
        auto *vnc = new VncPage(settings, m_sessionTabs);
        connection.password.fill(QChar(0));
        for (auto &jump : settings.jumps)
            jump.password.fill(QChar(0));
        connect(vnc, &VncPage::statusMessage, this, [this, vnc](const QString &message) {
            tabMessage(vnc, message);
        });
        connect(vnc, &VncPage::passwordSaveRequested, this, [this, baseId](const QString &password) {
            if (m_vault.needsVault(baseId) && !ensureVaultUnlocked())
                return;
            QString error;
            if (!m_vault.setPassword(baseId, password, &error))
                QMessageBox::warning(this, QStringLiteral("Could not save password"), error);
        });
        page = vnc;
    } else if (session.protocol.compare(QStringLiteral("SFTP"), Qt::CaseInsensitive) == 0
               || session.protocol.compare(QStringLiteral("SCP"), Qt::CaseInsensitive) == 0) {
        // A file session: SSH without a terminal, the file browser in the tab.
        auto settings = sshSettingsFor(session.host, session.port, session.username, sessionSshOptions(session));
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            notify(QStringLiteral("Session not opened: the credential vault is locked"), true);
            return;
        }
        if (!resolveJumpHosts(session, &settings.jumps))
            return;
        applyProxy(session, &settings, settings.jumps);
        settings.forwards = session.forwards;
        settings.openShell = false;
        settings.filesOnly = true;
        settings.forwardAgent = false;
        settings.forwardX11 = false;
        settings.followTerminalFolder = false;
        settings.fileTransfer = session.protocol.toLower();
        settings.password = m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0));
        auto *files = new FilesPage(settings, m_sessionTabs);
        settings.password.fill(QChar(0));
        for (auto &jump : settings.jumps)
            jump.password.fill(QChar(0));
        connect(files, &FilesPage::statusMessage, this, [this, files](const QString &message) {
            tabMessage(files, message);
        });
        page = files;
    } else if (session.protocol.compare(QStringLiteral("FTP"), Qt::CaseInsensitive) == 0) {
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            notify(QStringLiteral("Session not opened: the credential vault is locked"), true);
            return;
        }
        const auto options = loadFtpDefaults().overriddenBy(session.ftp);
        FtpConnectionSettings settings;
        settings.host = session.host;
        settings.security = options.security;
        settings.port = session.port != 0 ? session.port
                                          : options.security == QStringLiteral("implicit") ? 990 : 21;
        settings.username = session.username;
        settings.passive = options.mode != QStringLiteral("active");
        settings.listing = options.listing;
        settings.connectTimeout = options.connectTimeout;
        settings.keepaliveSeconds = options.keepaliveSeconds;
        settings.knownCertificatesPath = stateFilePath(QStringLiteral("ftp_certificates"));
        settings.proxy = effectiveProxy(session);
        const QString baseId = session.id.section(QLatin1Char('#'), 0, 0);
        settings.password = m_vault.passwordForSession(baseId);
        auto *ftp = new FilesPage(settings, m_security.offerToSave, m_sessionTabs);
        settings.password.fill(QChar(0));
        settings.proxy.password.fill(QChar(0));
        connect(ftp, &FilesPage::statusMessage, this, [this, ftp](const QString &message) {
            tabMessage(ftp, message);
        });
        connect(ftp, &FilesPage::passwordSaveRequested, this, [this, baseId](const QString &password) {
            if (m_vault.needsVault(baseId) && !ensureVaultUnlocked())
                return;
            QString error;
            if (!m_vault.setPassword(baseId, password, &error))
                QMessageBox::warning(this, QStringLiteral("Could not save password"), error);
        });
        page = ftp;
    } else if (session.protocol.compare(QStringLiteral("XDMCP"), Qt::CaseInsensitive) == 0) {
        page = new XdmcpPage(session.name, session.host, session.port,
                             loadXdmcpDefaults().overriddenBy(session.xdmcp), m_sessionTabs);
    } else if (session.protocol.compare(QStringLiteral("Telnet"), Qt::CaseInsensitive) == 0
               || session.protocol.compare(QStringLiteral("Serial"), Qt::CaseInsensitive) == 0
               || session.protocol.compare(QStringLiteral("Rlogin"), Qt::CaseInsensitive) == 0
               || session.protocol.compare(QStringLiteral("RSH"), Qt::CaseInsensitive) == 0) {
        StreamConnection *connection = nullptr;
        if (session.protocol.compare(QStringLiteral("Telnet"), Qt::CaseInsensitive) == 0)
            connection = new TelnetConnection(session.host, session.port,
                                              loadSshPreferences().terminalType);
        else if (session.protocol.compare(QStringLiteral("Rlogin"), Qt::CaseInsensitive) == 0)
            connection = new RloginConnection(session.host, session.port, session.username,
                                              loadRshDefaults().overriddenBy(session.rsh),
                                              loadSshPreferences().terminalType);
        else if (session.protocol.compare(QStringLiteral("RSH"), Qt::CaseInsensitive) == 0)
            connection = new RshConnection(session.host, session.port, session.username,
                                           loadRshDefaults().overriddenBy(session.rsh));
        else
            connection = new SerialConnection(session.host, session.serial);
        auto *terminal = new TerminalPage(
            connection, effectiveTerminalAppearance().overriddenBy(session.terminal), m_sessionTabs);
        if (session.protocol.compare(QStringLiteral("Telnet"), Qt::CaseInsensitive) == 0)
            terminal->setStreamReconnect(loadTelnetDefaults().overriddenBy(session.telnet).reconnect);
        else if (session.protocol.compare(QStringLiteral("Rlogin"), Qt::CaseInsensitive) == 0)
            terminal->setStreamReconnect(loadRshDefaults().overriddenBy(session.rsh).reconnect);
        connectTerminalPage(terminal);
        applyTabSettings(terminal, session.name);
        applySessionAutomation(terminal, session);
        page = terminal;
    } else if (session.protocol.compare(QStringLiteral("Mosh"), Qt::CaseInsensitive) == 0) {
        // The system's mosh client, which logs in with the system's ssh (on
        // Windows, one on the PATH, e.g. from Cygwin or MSYS2).
        const QString mosh = QStandardPaths::findExecutable(QStringLiteral("mosh"));
        if (mosh.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Mosh"),
                QStringLiteral("Mosh is not installed. Install the \"mosh\" package on this "
                               "computer, and on the server (it provides mosh-server)."));
            return;
        }
        QStringList arguments;
        if (session.port != 0)
            arguments << QStringLiteral("--ssh=ssh -p %1").arg(session.port);
        arguments << (session.username.isEmpty()
                          ? session.host
                          : QStringLiteral("%1@%2").arg(session.username, session.host));
        auto *terminal = newLocalProgramPage(mosh, arguments, QStringLiteral("Mosh"),
                                             effectiveTerminalAppearance().overriddenBy(session.terminal));
        connectTerminalPage(terminal);
        applyTabSettings(terminal, session.name);
        applySessionAutomation(terminal, session);
        page = terminal;
    } else if (isLocalSessionProtocol(session.protocol)) {
        auto *terminal = openLocalSession(session);
        if (!terminal)
            return;
        page = terminal;
    } else {
        page = new SessionPage(session, m_sessionTabs);
    }
    page->setProperty("sessionId", session.id);
    QString title = session.name;
    if (m_startup.tabTitle != QStringLiteral("name") && !session.host.isEmpty()) {
        const auto userHost = session.username.isEmpty()
                                  ? session.host
                                  : QStringLiteral("%1@%2").arg(session.username, session.host);
        title = m_startup.tabTitle == QStringLiteral("userhost")
                    ? userHost
                    : QStringLiteral("%1 (%2)").arg(session.name, userHost);
    }
    // A password typed at log-on (the tab's own, or a jump host's): offer to keep it.
    const auto offer = [this, page, ownId = session.id.section(QLatin1Char('#'), 0, 0),
                        name = session.name](const QString &saved, const QString &password, bool replaces) {
        offerToSavePassword(page, saved.isEmpty() ? ownId : saved,
                            saved.isEmpty() ? name : m_sessions.value(saved).name, password, replaces);
    };
    if (auto *terminal = qobject_cast<TerminalPage *>(page))
        connect(terminal, &TerminalPage::typedPasswordWorked, this, offer);
    else if (auto *files = qobject_cast<FilesPage *>(page))
        connect(files, &FilesPage::typedPasswordWorked, this, offer);
    else if (auto *screen = qobject_cast<RemoteScreenPage *>(page))
        connect(screen, &RemoteScreenPage::typedPasswordWorked, this, offer);
    addSessionTab(page, title, QStringLiteral("%1 — %2").arg(session.protocol, session.host));
    recordRecentSession(session.id);
}

// The connection of an open, connected tab of a saved session.
SshSession *MainWindow::openTabConnection(const QString &sessionId) const
{
    for (auto *page : allTerminalPages()) {
        if (page->property("sessionId").toString().section(QLatin1Char('#'), 0, 0) == sessionId
            && page->isConnected() && page->sshSession())
            return page->sshSession();
    }
    return nullptr;
}

void MainWindow::showSftpPanelForCurrentTab()
{
    auto *page = activeTerminal(m_sessionTabs->currentWidget());
    auto *panel = page ? page->sftpPanel() : nullptr;
    if (!panel) {
        m_sftpPlaceholder->showPlaceholder(
            QStringLiteral("Files"),
            m_sessionTabs->count() == 0
                ? QStringLiteral("Open an SSH session to browse its files here.")
                : qobject_cast<FilesPage *>(m_sessionTabs->currentWidget())
                ? QStringLiteral("This file tab shows its files itself.")
                : QStringLiteral("The current tab has no remote files to show."));
        m_sftpStack->setCurrentWidget(m_sftpPlaceholder);
        return;
    }
    if (m_sftpStack->indexOf(panel) < 0)
        m_sftpStack->addWidget(panel);
    m_sftpStack->setCurrentWidget(panel);
}

void MainWindow::markTabActivity(QWidget *page)
{
    const int index = tabIndexOf(page);
    if (index < 0 || index == m_sessionTabs->currentIndex()
        || m_sessionTabs->widget(index)->property("opsremoteActivity").toBool())
        return;
    m_sessionTabs->widget(index)->setProperty("opsremoteActivity", true);
    showTabActivity(index);
}

void MainWindow::clearTabActivity(int index)
{
    m_sessionTabs->widget(index)->setProperty("opsremoteActivity", false);
    showTabActivity(index);
}

// Unless chosen in Preferences, from the look: the first of the accent,
// link, amber and blue that stands out against the tab bar, pulsing towards
// a faint copy of it.
std::pair<QColor, QColor> MainWindow::activityColors() const
{
    const auto palette = m_sessionTabs->tabBar()->palette();
    const auto colour = m_behaviour.activityColor.isValid()
        ? m_behaviour.activityColor
        : visibleColor(palette.color(QPalette::Window),
                       {palette.color(QPalette::Highlight), palette.color(QPalette::Link),
                        QColor(0xf0, 0xa0, 0x30), QColor(0x3d, 0xae, 0xe9)});
    auto faded = colour;
    faded.setAlphaF(0.2);
    return {colour, m_behaviour.activityPulseColor.isValid() ? m_behaviour.activityPulseColor : faded};
}

// With a dot, every tab keeps an idle one on the side without the close
// button, so a tab does not grow when it is marked. Where another widget
// sits there, or with "Title colour", the title is coloured instead.
void MainWindow::showTabActivity(int index)
{
    auto *bar = m_sessionTabs->tabBar();
    auto *page = m_sessionTabs->widget(index);
    if (!m_behaviour.markActivity)
        page->setProperty("opsremoteActivity", false);
    const bool marked = page->property("opsremoteActivity").toBool();
    const bool wantDot = m_behaviour.markActivity && m_behaviour.activityStyle != QStringLiteral("text");

    const auto closeSide = static_cast<QTabBar::ButtonPosition>(
        bar->style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, bar));
    const auto side = closeSide == QTabBar::LeftSide ? QTabBar::RightSide : QTabBar::LeftSide;
    auto *dot = qobject_cast<ActivityDot *>(bar->tabButton(index, side));
    if (wantDot && !dot && !bar->tabButton(index, side)) {
        dot = new ActivityDot(bar);
        bar->setTabButton(index, side, dot);
    } else if (!wantDot && dot) {
        bar->setTabButton(index, side, nullptr);
        dot->deleteLater();
        dot = nullptr;
    }

    const auto [colour, second] = activityColors();
    if (dot) {
        dot->setColors(colour, second);
        dot->setPulsing(m_behaviour.activityStyle == QStringLiteral("pulse"));
        dot->setActive(marked);
    }
    bar->setTabTextColor(index, marked && !dot ? colour : QColor());
}

// The host has its own column (Preferences → Appearance → Session list).
QString MainWindow::sessionLabel(const SessionDefinition &session) const
{
    return session.name;
}

void MainWindow::connectTerminalPage(TerminalPage *page)
{
    connect(page, &TerminalPage::statusMessage, this, [this, page](const QString &message) {
        tabMessage(page, message);
    });
    // Background tabs with new output or a bell are marked.
    auto mark = [this, page] { markTabActivity(page); };
    connect(page, &TerminalPage::activity, this, mark);
    connect(page, &TerminalPage::bell, this, mark);
    connect(page, &TerminalPage::automationMark, this, mark);
    connect(page, &TerminalPage::scriptMenuRequested, this, [this, page](QMenu *menu) { addScriptMenu(menu, page); });
    connect(page, &TerminalPage::automationNotice, this, [this, page, mark](const QString &message) {
        tabMessage(page, message);
        mark();
        QApplication::alert(this);
    });
    connect(page, &TerminalPage::userInput, this, [this, page](const QByteArray &data) {
        if (m_broadcasting)
            return;
        // Broadcast: every terminal. Multi-exec: the chosen ones, when this is one.
        const bool multiExec = !m_broadcast && page->isMultiExecTarget();
        if (!m_broadcast && !multiExec)
            return;
        m_broadcasting = true;
        for (auto *other : allTerminalPages()) {
            if (other != page && (m_broadcast || other->isMultiExecTarget()))
                other->sendInput(data);
        }
        m_broadcasting = false;
    });
    connect(page, &TerminalPage::multiExecToggled, this, &MainWindow::updateMultiExecIndicator);
    // The terminal's own menu: split or duplicate this terminal.
    const auto makeActive = [this, page] {
        const int index = tabIndexOf(page);
        if (index < 0)
            return false;
        m_sessionTabs->setCurrentIndex(index);
        if (auto *split = qobject_cast<SplitTab *>(m_sessionTabs->widget(index)))
            split->setActivePane(page);
        return true;
    };
    connect(page, &TerminalPage::splitMenuRequested, this, [this, makeActive, page](QMenu *menu) {
        addSplitMenus(menu, makeActive);
        if (m_startup.moveTerminals)
            fillMoveMenu(menu->addMenu(QStringLiteral("Move")), page);
    });
    connect(page, &TerminalPage::duplicateRequested, this, [this, page, makeActive] {
        if (makeActive())
            duplicateTab(tabIndexOf(page));
    });
    // A closed terminal leaves the multi-exec count.
    connect(page, &QObject::destroyed, this, [this] {
        QTimer::singleShot(0, this, &MainWindow::updateMultiExecIndicator);
    });
}

void MainWindow::addSessionTab(QWidget *page, const QString &title, const QString &toolTip)
{
    // Kept for a tab of its own, should it be moved out of a split tab.
    page->setProperty("opsremoteTabTitle", title);
    page->setProperty("opsremoteTabToolTip", toolTip);
    // A terminal opened for Split Right / Split Down goes into that tab.
    if (const auto pending = std::exchange(m_pendingSplit, {}); pending.beside) {
        if (auto *terminal = qobject_cast<TerminalPage *>(page)) {
            insertPane(pending.beside, terminal, pending.orientation);
            return;
        }
    }
    const int index = m_startup.newTabNextToCurrent && m_sessionTabs->currentIndex() >= 0
                          ? m_sessionTabs->insertTab(m_sessionTabs->currentIndex() + 1, page, title)
                          : m_sessionTabs->addTab(page, title);
    m_sessionTabs->setTabToolTip(index, toolTip);
    m_sessionTabs->setCurrentIndex(index);
    page->setFocus();
    // "Close Tab" on a tab whose session ended.
    const auto close = [this, page] {
        if (const int at = m_sessionTabs->indexOf(page); at >= 0)
            closeSessionTab(at);
    };
    if (auto *terminal = qobject_cast<TerminalPage *>(page))
        connect(terminal, &TerminalPage::closeRequested, this, [this, terminal] { closePane(terminal); });
    else if (auto *screen = qobject_cast<RemoteScreenPage *>(page))
        connect(screen, &RemoteScreenPage::closeRequested, this, close);
}

void MainWindow::duplicateTab(int index)
{
    QWidget *page = m_sessionTabs->widget(index);
    if (auto *split = qobject_cast<SplitTab *>(page))
        page = split->activePane();
    if (!page)
        return;
    const auto sessionId = page->property("sessionId").toString();
    if (sessionId.isEmpty()) {
        openLocalTerminal();
        return;
    }
    const auto session = m_sessions.constFind(sessionId);
    if (session == m_sessions.cend())
        return;
    // openSession() switches to an open tab of the same session, so open a
    // second copy under a distinct ID.
    auto copy = *session;
    copy.id = sessionId + QStringLiteral("#") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    openSession(copy);
}

// Send Keys and Save Screenshot for a remote desktop (RDP or VNC) tab.
void MainWindow::addRemoteScreenActions(QMenu *menu, RemoteScreenPage *screen)
{
    auto *keys = menu->addMenu(QStringLiteral("Send Keys"));
    keys->setEnabled(screen->isConnected());
    for (const auto &[combination, label] : RemoteScreenPage::keyCombinations()) {
        keys->addAction(label, screen, [screen, combination] {
            screen->sendKeyCombination(combination);
        });
    }
    if (screen->canRefresh()) {
        auto *refresh = menu->addAction(QStringLiteral("Refresh Screen"), screen,
                                        [screen] { screen->refreshScreen(); });
        refresh->setEnabled(screen->isConnected());
    }
    menu->addAction(QStringLiteral("Save Screenshot…"), this, [this, screen] {
        const QImage image = screen->screenshot();
        if (image.isNull()) {
            tabMessage(screen, QStringLiteral("Nothing to save yet: not connected."));
            return;
        }
        const QString name = QStringLiteral("%1 %2.png")
                                 .arg(screen->host(), QDateTime::currentDateTime().toString(
                                                          QStringLiteral("yyyy-MM-dd HH.mm.ss")));
        const QString path = FileDialogs::saveFileName(
            this, QStringLiteral("Save Screenshot"),
            QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).filePath(name),
            QStringLiteral("PNG image (*.png);;JPEG image (*.jpg)"));
        if (path.isEmpty())
            return;
        if (image.save(path))
            tabMessage(screen, QStringLiteral("Screenshot saved to %1").arg(QDir::toNativeSeparators(path)));
        else
            QMessageBox::warning(this, QStringLiteral("Save Screenshot"),
                                 QStringLiteral("Could not save %1.").arg(QDir::toNativeSeparators(path)));
    });
}

void MainWindow::showTabContextMenu(const QPoint &position)
{
    const int index = m_sessionTabs->tabBar()->tabAt(position);
    if (index < 0)
        return;
    QMenu menu(this);
    menu.addAction(QStringLiteral("Duplicate Tab"), this, [this, index] { duplicateTab(index); });
    if (activeTerminal(m_sessionTabs->widget(index))) {
        m_sessionTabs->setCurrentIndex(index);
        if (auto *terminal = activeTerminal(m_sessionTabs->widget(index))) {
            auto *include = menu.addAction(QStringLiteral("Multi-exec: Type Into This Terminal Too"));
            include->setCheckable(true);
            include->setChecked(terminal->isMultiExecTarget());
            connect(include, &QAction::toggled, this, [this, terminal](bool on) {
                terminal->setMultiExecTarget(on);
                updateMultiExecIndicator();
            });
        }
        addSplitMenus(&menu, [] { return true; });
        if (qobject_cast<SplitTab *>(m_sessionTabs->widget(index)))
            menu.addAction(QStringLiteral("Close Pane"), this, [this, index] {
                if (auto *pane = activeTerminal(m_sessionTabs->widget(index)))
                    closePane(pane);
            });
    }
    if (auto *screen = qobject_cast<RemoteScreenPage *>(m_sessionTabs->widget(index))) {
        menu.addSeparator();
        addRemoteScreenActions(&menu, screen);
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("Close Tab"), this, [this, index] { closeSessionTab(index); });
    menu.addAction(QStringLiteral("Close Other Tabs"), this, [this, index] {
        auto *keep = m_sessionTabs->widget(index);
        for (int other = m_sessionTabs->count() - 1; other >= 0; --other) {
            if (m_sessionTabs->widget(other) != keep)
                closeSessionTab(other);
        }
    });
    // The tabs on one side; by page, since each may ask first or stay open.
    const auto closeSide = [this](int from, int to) {
        QList<QPointer<QWidget>> pages;
        for (int other = from; other < to; ++other)
            pages.append(m_sessionTabs->widget(other));
        for (int page = int(pages.size()) - 1; page >= 0; --page) {
            if (pages[page] && m_sessionTabs->indexOf(pages[page]) >= 0)
                closeSessionTab(m_sessionTabs->indexOf(pages[page]));
        }
    };
    menu.addAction(QStringLiteral("Close Tabs to the Left"), this, [closeSide, index] { closeSide(0, index); })
        ->setEnabled(index > 0);
    menu.addAction(QStringLiteral("Close Tabs to the Right"), this, [this, closeSide, index] {
        closeSide(index + 1, m_sessionTabs->count());
    })->setEnabled(index < m_sessionTabs->count() - 1);
    menu.exec(m_sessionTabs->tabBar()->mapToGlobal(position));
}

QStringList MainWindow::openSessionIds() const
{
    QStringList ids;
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        // Duplicated tabs are stored under their session's ID; the panes of a
        // split tab come back as tabs of their own.
        auto *tab = m_sessionTabs->widget(index);
        QList<QWidget *> pages{tab};
        if (auto *split = qobject_cast<SplitTab *>(tab)) {
            pages.clear();
            for (auto *pane : split->panes())
                pages.append(pane);
        }
        for (auto *page : std::as_const(pages)) {
            const auto id = page->property("sessionId").toString().section(QLatin1Char('#'), 0, 0);
            ids.append(id.isEmpty() ? QStringLiteral("local") : id);
        }
    }
    return ids;
}

void MainWindow::restoreStartupTabs()
{
    if (m_security.unlockAtStartup && m_vault.exists() && !m_vault.isUnlocked())
        ensureVaultUnlocked();
    if (m_startup.onStartup == QStringLiteral("local")) {
        if (LocalTerminalSupported)
            openLocalTerminal();
    } else if (m_startup.onStartup == QStringLiteral("restore")) {
        // Startup restoration opens each saved definition once. The separate
        // duplicate-tabs preference controls only a deliberate Open action in
        // this run; it must not turn a former duplicated view into a new
        // persistence rule.
        QSet<QString> restored;
        for (const auto &id : loadOpenSessions()) {
            if (id == QStringLiteral("local")) {
                if (LocalTerminalSupported)
                    openLocalTerminal();
                continue;
            }
            if (restored.contains(id))
                continue;
            restored.insert(id);
            const auto session = m_sessions.constFind(id);
            if (session != m_sessions.cend())
                openSession(*session);
        }
    }
}

void MainWindow::offerToSavePassword(QWidget *page, const QString &key, const QString &label,
                                     const QString &password, bool replaces)
{
    // Saved sessions only (quick connections have nowhere to keep it), and
    // nothing to ask when the saved password is the one that worked.
    if (!m_security.offerToSave || password.isEmpty() || !m_sessions.contains(key.section(QLatin1Char('#'), 0, 0))
        || (!replaces && !m_vault.needsUnlock(key) && !m_vault.passwordForSession(key).isEmpty()))
        return;
    const std::shared_ptr<QString> secret(new QString(password), [](QString *text) {
        text->fill(QChar(0));
        delete text;
    });
    const auto forget = [secret] {
        secret->fill(QChar(0));
        secret->clear();
    };
    const QPointer<QWidget> target(page);
    const auto save = [this, secret, forget, key, label, target] {
        if (secret->isEmpty())
            return;
        QString error;
        if (m_vault.needsVault(key) && !ensureVaultUnlocked()) {
            forget();
            return;
        }
        const bool saved = m_vault.setPassword(key, *secret, &error);
        forget();
        if (!saved)
            QMessageBox::warning(this, QStringLiteral("Could not save password"), error);
        else if (target)
            tabMessage(target, QStringLiteral("Password for %1 saved.").arg(label));
        qInfo("Password: saved for %s", qUtf8Printable(label));
    };
    const QString text = replaces
        ? QStringLiteral("The saved password for %1 was rejected; the one you typed worked. Replace it?").arg(label)
        : QStringLiteral("Save the password for %1?").arg(label);
    qInfo("Password: offered to save for %s", qUtf8Printable(label));
    if (TabPrompt::inStrip()) {
        static const QString Owner = QStringLiteral("password offer");
        auto *prompt = TabPrompt::of(page);
        prompt->set(text, false,
                    {{replaces ? QStringLiteral("Replace") : QStringLiteral("Save"),
                      QStringLiteral("Keep it in the session's password store"),
                      [prompt, save] {
                          prompt->clear(Owner);
                          save();
                      }},
                     {QStringLiteral("Not now"), QStringLiteral("Type it again next time"), [prompt, forget] {
                          prompt->clear(Owner);
                          forget();
                      }}},
                    {}, Owner);
        return;
    }
    // Without the bars above the panes: a question that does not block the window.
    auto *box = new QMessageBox(QMessageBox::Question, QStringLiteral("Save Password"), text,
                                QMessageBox::Save | QMessageBox::Cancel, this);
    box->button(QMessageBox::Cancel)->setText(QStringLiteral("Not now"));
    if (replaces)
        box->button(QMessageBox::Save)->setText(QStringLiteral("Replace"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [box, save, forget] {
        if (box->clickedButton() == box->button(QMessageBox::Save))
            save();
        forget();
    });
    box->open();
}

