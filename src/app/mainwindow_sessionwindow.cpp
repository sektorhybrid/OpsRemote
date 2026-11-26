// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// MainWindow: the New Session and Edit Session window: its tabs and editors, inherited jump hosts, proxies and automation, tunnel servers, saving passwords.

#include "mainwindow_p.h"

using namespace mainwindow_p;

namespace {

// A protocol's own options below the common fields of the session window
// (the editor's own tabs, no frame around them), with ↺ per row, shown only
// while that protocol is chosen; the window refits when it shows or hides.
template <class Editor>
Editor *addProtocolOptions(QFormLayout *form, QComboBox *protocolCombo, const QString &protocol, Editor *editor)
{
    auto *group = new QWidget(form->parentWidget());
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    applyInheritance(editor); // ↺ per row, as in every session window
    layout->addWidget(editor);
    form->addRow(group);
    auto updateVisible = [protocolCombo, group, protocol] {
        group->setVisible(protocolCombo->currentText() == protocol);
        if (auto *dialog = group->window())
            QTimer::singleShot(0, dialog, [dialog] { WindowFit::adjustSize(dialog); });
    };
    QObject::connect(protocolCombo, &QComboBox::currentTextChanged, group, updateVisible);
    updateVisible();
    return editor;
}

} // namespace

struct MainWindow::SessionWindow
{
    QLineEdit *name = nullptr;
    QComboBox *protocol = nullptr;
    QLineEdit *host = nullptr;
    QLineEdit *port = nullptr;
    QLineEdit *username = nullptr;
    QLineEdit *password = nullptr;
    QComboBox *store = nullptr;
    SshOptionsEditor *ssh = nullptr;
    SerialOptionsEditor *serial = nullptr;
    TerminalAppearanceEditor *appearance = nullptr;
    AutomationEditor *automation = nullptr;
    LocalSessionEditor *local = nullptr;
    RdpOptionsEditor *rdp = nullptr;
    VncOptionsEditor *vnc = nullptr;
    FtpOptionsEditor *ftp = nullptr;
    RshOptionsEditor *rsh = nullptr;
    TelnetOptionsEditor *telnet = nullptr;
    XdmcpOptionsEditor *xdmcp = nullptr;
    JumpHostEditor *jumpHost = nullptr;
    PortForwardListEditor *forwards = nullptr;
    ProxyEditor *proxy = nullptr;
    WakeOnLanEditor *wake = nullptr;

    // session with the window's values; the options of other protocols are
    // emptied (a session that changed protocol keeps nothing stale).
    SessionDefinition read(SessionDefinition session) const
    {
        const auto portText = port->text().trimmed();
        session.name = name->text().trimmed();
        session.protocol = protocol->currentText();
        session.host = host->text().trimmed();
        session.port = portText.isEmpty() ? quint16(0) : portText.toUShort();
        session.username = username->text().trimmed();
        session.jumpHost = jumpHost->value();
        session.proxy = proxy->value();
        session.wakeOnLan = wake->value();
        const QString &kind = session.protocol;
        const bool sshKind = isSshProtocol(kind);
        const bool terminalKind = isTerminalProtocol(kind);
        session.forwards = sshKind ? forwards->value() : QList<PortForward>();
        session.ssh = sshKind ? ssh->value() : SshOptions();
        session.terminal = terminalKind ? appearance->value() : TerminalAppearance();
        session.automation = terminalKind ? automation->value() : AutomationOptions();
        session.local = isLocalSessionProtocol(kind) ? local->value() : LocalSessionOptions();
        session.serial = kind == QStringLiteral("Serial") ? serial->value() : SerialOptions();
        session.rdp = kind == QStringLiteral("RDP") ? rdp->value() : RdpOptions();
        session.vnc = kind == QStringLiteral("VNC") ? vnc->value() : VncOptions();
        session.ftp = kind == QStringLiteral("FTP") ? ftp->value() : FtpOptions();
        session.rsh = kind == QStringLiteral("Rlogin") || kind == QStringLiteral("RSH") ? rsh->value() : RshOptions();
        session.telnet = kind == QStringLiteral("Telnet") ? telnet->value() : TelnetOptions();
        session.xdmcp = kind == QStringLiteral("XDMCP") ? xdmcp->value() : XdmcpOptions();
        session.passwordStore = store->currentData().toString();
        return session;
    }
};

MainWindow::SessionWindow MainWindow::buildSessionWindow(QDialog &dialog, const SessionDefinition &value,
                                                         QTreeWidgetItem *folder, bool isNew, bool fixedProtocol)
{
    SessionWindow window;
    // Wide enough for the terminal preview and the settings tabs.
    dialog.setMinimumWidth(560);
    auto *form = new QFormLayout(&dialog);

    window.name = new QLineEdit(value.name, &dialog);
    window.protocol = new QComboBox(&dialog);
    window.protocol->addItems(sessionProtocols());
    // Before the protocol's tabs are made, which show for the chosen one.
    if (!value.protocol.isEmpty())
        window.protocol->setCurrentText(value.protocol);
    // From a list of SSH sessions: only that kind fits there.
    window.protocol->setEnabled(!fixedProtocol);
    window.host = new QLineEdit(value.host, &dialog);
    window.host->setPlaceholderText(QStringLiteral("hostname or IP address"));
    window.port = new QLineEdit(value.port == 0 ? QString() : QString::number(value.port), &dialog);
    window.port->setPlaceholderText(QStringLiteral("Use protocol default"));
    window.username = new QLineEdit(value.username, &dialog);
    window.username->setObjectName(QStringLiteral("usernameEdit"));
    window.password = new QLineEdit(&dialog);
    window.password->setEchoMode(QLineEdit::Password);
    if (!isNew)
        window.password->setPlaceholderText(QStringLiteral("Leave blank to keep the stored password"));

    form->addRow(QStringLiteral("Name:"), window.name);
    form->addRow(QStringLiteral("Protocol:"), window.protocol);
    form->addRow(QStringLiteral("Host:"), window.host);
    form->addRow(QStringLiteral("Port:"), window.port);
    form->addRow(QStringLiteral("Username:"), window.username);
    form->addRow(isNew ? QStringLiteral("Password (optional):") : QStringLiteral("New password (optional):"),
                 window.password);
    window.store = passwordStoreChoice(value.passwordStore, &dialog);
    form->addRow(QStringLiteral("Keep the password in:"), window.store);
    auto *protocolCombo = window.protocol;
    window.appearance = addTerminalAppearanceGroup(form, protocolCombo, value.terminal, value.ssh, &window.ssh,
                                                   value.serial, &window.serial);
    window.automation = addAutomationTab(window.appearance, value.automation);
    window.local = addLocalSessionTab(protocolCombo, window.appearance, value.local, window.password, value.id);
    connectHostField(protocolCombo, window.host, window.port);
    // Mosh needs the mosh program on this computer.
    auto *moshNote = new AvailabilityNote(
        [protocolCombo] {
            return protocolCombo->currentText() == QStringLiteral("Mosh") ? LocalResources::moshProblem()
                                                                          : QString();
        },
        {}, &dialog);
    int protocolRow = -1;
    form->getWidgetPosition(protocolCombo, &protocolRow, nullptr);
    form->insertRow(protocolRow + 1, QString(), moshNote);
    connect(protocolCombo, &QComboBox::currentTextChanged, moshNote, &AvailabilityNote::check);
    window.rdp = addProtocolOptions(form, protocolCombo, QStringLiteral("RDP"), new RdpOptionsEditor(value.rdp, loadRdpDefaults()));
    window.vnc = addProtocolOptions(form, protocolCombo, QStringLiteral("VNC"), new VncOptionsEditor(value.vnc, loadVncDefaults()));
    window.ftp = addProtocolOptions(form, protocolCombo, QStringLiteral("FTP"), new FtpOptionsEditor(value.ftp, loadFtpDefaults()));
    window.rsh = addRshOptionsTab(protocolCombo, window.appearance, value.rsh);
    window.telnet = addTelnetOptionsTab(window.appearance, value.telnet);
    window.xdmcp = addProtocolOptions(form, protocolCombo, QStringLiteral("XDMCP"),
                                      new XdmcpOptionsEditor(value.xdmcp, loadXdmcpDefaults()));
    window.jumpHost = newJumpHostEditor(value.jumpHost, folder, value.id);
    window.forwards = new PortForwardListEditor(value.forwards);
    window.proxy = newProxyEditor(value.proxy, folder);
    window.wake = new WakeOnLanEditor(value.wakeOnLan);
    addNetworkTab(protocolCombo, window.appearance, window.rdp, window.vnc, window.jumpHost, window.forwards,
                  window.proxy, window.wake);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    // Save waits while the proxy lacks its host or port.
    auto *proxyGuard = new SaveGuard(buttons->button(QDialogButtonBox::Save), &dialog);
    auto *proxy = window.proxy;
    proxyGuard->addCheck([proxy] { return proxy->problem(); });
    connect(proxy, &ProxyEditor::changed, proxyGuard, &SaveGuard::recheck);
    form->addRow(buttons);
    validateSessionDialog(&dialog, form, buttons, protocolCombo, window.name, window.host, window.port, window.wake);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return window;
}

void MainWindow::createSession()
{
    createSessionOver(this, QString());
}

QString MainWindow::createSshSession(QWidget *over)
{
    return createSessionOver(over, QStringLiteral("SSH"));
}

QString MainWindow::createSessionOver(QWidget *over, const QString &protocol)
{
    // In the selected folder, or at the top level when none is selected.
    auto *parentFolder = selectedFolder();

    QDialog dialog(over ? over : this);
    dialog.setWindowTitle(QStringLiteral("New Session"));
    SessionDefinition fresh;
    fresh.protocol = protocol;
    const auto window = buildSessionWindow(dialog, fresh, parentFolder, true, !protocol.isEmpty());
    auto *passwordEdit = window.password;
    if (dialog.exec() != QDialog::Accepted)
        return QString();

    fresh.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const SessionDefinition session = window.read(fresh);
    saveProxyPassword(session.proxy, window.proxy->password());
    // Added first, so the password goes to the store this session chose.
    addSession(session, parentFolder);
    saveSessions();
    auto sessionPassword = passwordEdit->text();
    if (!sessionPassword.isEmpty()) {
        QString vaultError;
        if ((m_vault.needsVault(session.id) && !ensureVaultUnlocked())
            || !m_vault.setPassword(session.id, sessionPassword, &vaultError)) {
            QMessageBox::warning(this, QStringLiteral("Could not save password"),
                                 vaultError.isEmpty() ? QStringLiteral("The vault stays locked.") : vaultError);
        }
        sessionPassword.fill(QChar(0));
        passwordEdit->clear();
    }
    return session.id;
}

void MainWindow::editSelectedSession()
{
    auto *item = selectedItem();
    if (!item || isFolder(item)) {
        QMessageBox::information(this, QStringLiteral("Edit Session"),
                                 QStringLiteral("Select a saved session first."));
        return;
    }

    const auto sessionId = item->data(0, SessionIdRole).toString();
    const auto sessionIterator = m_sessions.find(sessionId);
    if (sessionIterator == m_sessions.end())
        return;

    const SessionDefinition original = *sessionIterator;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit Saved Session"));
    const auto window = buildSessionWindow(dialog, original, item->parent(), false, false);
    auto *passwordEdit = window.password;
    if (dialog.exec() != QDialog::Accepted)
        return;

    auto updated = window.read(original);
    saveProxyPassword(updated.proxy, window.proxy->password());
    auto newPassword = passwordEdit->text();
    // A different store and no new password: the saved one moves along.
    const PasswordStore storeBefore = m_vault.storeFor(updated.id);
    QString moving;
    if (newPassword.isEmpty() && updated.passwordStore != original.passwordStore) {
        if (m_vault.needsUnlock(updated.id))
            ensureVaultUnlocked();
        moving = m_vault.passwordForSession(updated.id);
    }

    // Looked up again: "New…" in the jump host list may have added a
    // session while the window was open.
    m_sessions.insert(sessionId, updated);
    if (!moving.isEmpty() && m_vault.storeFor(updated.id) != storeBefore) {
        newPassword = moving;
        moving.fill(QChar(0));
    }
    if (!newPassword.isEmpty()) {
        QString vaultError;
        if ((m_vault.needsVault(updated.id) && !ensureVaultUnlocked())
            || !m_vault.setPassword(updated.id, newPassword, &vaultError)) {
            QMessageBox::warning(this, QStringLiteral("Could not save password"),
                                 vaultError.isEmpty() ? QStringLiteral("The vault stays locked.") : vaultError);
        } else if (m_vault.storeFor(updated.id) == PasswordStore::Vault
                   && storeBefore != PasswordStore::Vault) {
            SystemCredentials::remove(updated.id); // no copy left behind
        }
        newPassword.fill(QChar(0));
        passwordEdit->clear();
    }

    item->setText(0, sessionLabel(updated));
    item->setToolTip(0, QStringLiteral("%1 — %2:%3")
                            .arg(updated.protocol, updated.host,
                                 updated.port == 0 ? QStringLiteral("default")
                                                   : QString::number(updated.port)));
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *page = m_sessionTabs->widget(index);
        if (page && page->property("sessionId").toString() == updated.id) {
            m_sessionTabs->setTabText(index, updated.name);
            m_sessionTabs->setTabToolTip(index, QStringLiteral("%1 — %2")
                                                    .arg(updated.protocol, updated.host));
        }
    }
    applyTerminalAppearanceToTabs();
    saveSessions();
}

// The SSH settings of the session window: appearance and connection tabs.
// The protocols a page of the session window's shared tabs belongs to
// (pages without it are SSH's).
const char *const ProtocolsProperty = "sessionTabProtocols";

TerminalAppearanceEditor *MainWindow::addTerminalAppearanceGroup(
    QFormLayout *form, QComboBox *protocolCombo, const TerminalAppearance &value,
    const SshOptions &ssh, SshOptionsEditor **sshEditor, const SerialOptions &serial,
    SerialOptionsEditor **serialEditor)
{
    // One row of tabs for the terminal and SSH protocols, in the order every
    // protocol's tabs follow: its own settings first (Connection, Serial
    // line), then Appearance, then the network tabs (Jump host, Port
    // forwarding, Proxy), Wake-on-LAN last.
    auto *tabs = new QTabWidget(form->parentWidget());
    tabs->setObjectName(QStringLiteral("sessionTabs"));
    *sshEditor = new SshOptionsEditor(ssh, loadSshPreferences(), tabs);
    applyInheritance(*sshEditor); // ↺ per row, as in every session window
    tabs->addTab(*sshEditor, QStringLiteral("Connection"));
    *serialEditor = new SerialOptionsEditor(serial, tabs);
    (*serialEditor)->setProperty(ProtocolsProperty, QStringList{QStringLiteral("Serial")});
    tabs->addTab(*serialEditor, QStringLiteral("Connection")); // the first tab is always Connection
    auto *editor = new TerminalAppearanceEditor(value, true, m_terminalAppearance, tabs);
    applyInheritance(editor); // ↺ per row, as in every session window
    QStringList terminals;
    for (const auto &protocol : sessionProtocols()) {
        if (isTerminalProtocol(protocol))
            terminals << protocol;
    }
    editor->setProperty(ProtocolsProperty, terminals);
    tabs->addTab(editor, QStringLiteral("Appearance"));
    form->addRow(tabs);
    // Only the selected protocol's tabs are shown; the dialog shrinks to fit.
    // Tabs other windows' editors move here (addProtocolTab) say their
    // protocols the same way when they are not only SSH's.
    auto *connectionEditor = *sshEditor;
    auto updateVisible = [protocolCombo, tabs, connectionEditor] {
        const QString protocol = protocolCombo->currentText();
        connectionEditor->setFileOnly(protocol == QStringLiteral("SFTP") || protocol == QStringLiteral("SCP"));
        bool any = false;
        for (int index = 0; index < tabs->count(); ++index) {
            const auto protocols = tabs->widget(index)->property(ProtocolsProperty);
            const bool shown = protocols.isValid() ? protocols.toStringList().contains(protocol)
                                                   : isSshProtocol(protocol);
            tabs->setTabVisible(index, shown);
            any = any || shown;
        }
        tabs->setVisible(any);
        // Another protocol starts at its first tab (its own settings).
        const bool changed = tabs->property("shownProtocol").toString() != protocol;
        tabs->setProperty("shownProtocol", protocol);
        if (changed || tabs->currentIndex() < 0 || !tabs->isTabVisible(tabs->currentIndex())) {
            for (int index = 0; index < tabs->count(); ++index) {
                if (tabs->isTabVisible(index)) {
                    tabs->setCurrentIndex(index);
                    break;
                }
            }
        }
        if (auto *dialog = tabs->window())
            QTimer::singleShot(0, dialog, [dialog] { WindowFit::adjustSize(dialog); });
    };
    // After the shared tabs (Wake-on-LAN, Jump host, Proxy) have moved to the
    // new protocol's tabs, which happens later in the same change.
    connect(protocolCombo, &QComboBox::currentTextChanged, tabs,
            [tabs, updateVisible] { QTimer::singleShot(0, tabs, updateVisible); });
    // Once every tab is in (Connection tabs of other protocols go first,
    // later), the window opens on the protocol's first tab, its own settings.
    QTimer::singleShot(0, tabs, [tabs, updateVisible] {
        tabs->setProperty("shownProtocol", QString());
        updateVisible();
    });
    updateVisible();
    return editor;
}

// The host and port fields mean other things for some protocols: a serial
// session's host is the port (COM3, /dev/ttyUSB0), and it has no TCP port.
void MainWindow::connectHostField(QComboBox *protocolCombo, QLineEdit *hostEdit, QLineEdit *portEdit)
{
    auto *form = qobject_cast<QFormLayout *>(hostEdit->parentWidget()->layout());
    QStringList ports;
    for (const auto &[name, label] : availableSerialPorts())
        ports.append(name);
    auto *completer = new QCompleter(ports, hostEdit);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    const auto update = [protocolCombo, hostEdit, portEdit, form, completer, ports] {
        const bool serial = protocolCombo->currentText() == QStringLiteral("Serial");
        hostEdit->setCompleter(serial ? completer : nullptr);
        hostEdit->setPlaceholderText(
            serial ? (ports.isEmpty() ? QStringLiteral("serial port, e.g. COM3 or /dev/ttyUSB0")
                                      : QStringLiteral("serial port: %1").arg(ports.join(QStringLiteral(", "))))
                   : QStringLiteral("hostname or IP address"));
        if (form) {
            if (auto *label = qobject_cast<QLabel *>(form->labelForField(hostEdit)))
                label->setText(serial ? QStringLiteral("Port:") : QStringLiteral("Host:"));
            // Local and container sessions have no host (their Connection tab says where).
            const bool hostless = !protocolNeedsHost(protocolCombo->currentText());
            form->setRowVisible(hostEdit, !hostless);
            form->setRowVisible(portEdit, !serial && !hostless);
            if (auto *username = hostEdit->window()->findChild<QLineEdit *>(QStringLiteral("usernameEdit")))
                form->setRowVisible(username, !hostless);
        }
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, hostEdit, update);
    update();
}

// What an empty jump host means in a folder: the nearest folder (this one
// or above) that sets one, or connecting directly.
QString MainWindow::inheritedJumpHost(const QTreeWidgetItem *folder) const
{
    for (; folder; folder = folder->parent()) {
        const auto jumpHost = folder->data(0, FolderJumpHostRole).toString();
        if (!jumpHost.isEmpty())
            return jumpHost;
    }
    return JumpHost::Direct;
}

QString MainWindow::effectiveJumpHost(const SessionDefinition &session) const
{
    if (!session.jumpHost.isEmpty())
        return session.jumpHost;
    QTreeWidgetItem *item = nullptr;
    for (QTreeWidgetItemIterator it(m_sessionTree); *it && !item; ++it) {
        if ((*it)->data(0, SessionIdRole).toString() == session.id && !isQuickItem(*it))
            item = *it;
    }
    const auto inherited = inheritedJumpHost(item ? item->parent() : nullptr);
    // A bastion kept in the folder it serves does not go through itself.
    return inherited == JumpHost::SessionPrefix + session.id ? JumpHost::Direct : inherited;
}

// A label for a jump host value, as shown in the session and folder windows.
QString MainWindow::jumpHostLabel(const QString &jumpHost) const
{
    if (jumpHost.isEmpty() || jumpHost == JumpHost::Direct)
        return QStringLiteral("None (connect directly)");
    if (jumpHost.startsWith(JumpHost::SessionPrefix)) {
        const auto session = m_sessions.constFind(jumpHost.mid(JumpHost::SessionPrefix.size()));
        return session == m_sessions.cend() ? QStringLiteral("A deleted session") : session->name;
    }
    return jumpHost;
}

// The jump hosts to pass through, the first one reached directly. Shows a
// message and returns false when one cannot be used.
bool MainWindow::resolveJumpHosts(const SessionDefinition &session,
                                  std::vector<SshConnectionSettings> *jumps)
{
    jumps->clear();
    QSet<QString> visited{session.id};
    QString jumpHost = effectiveJumpHost(session);
    while (jumpHost != JumpHost::Direct && !jumpHost.isEmpty()) {
        SshConnectionSettings hop;
        QString next;
        if (jumpHost.startsWith(JumpHost::SessionPrefix)) {
            const auto id = jumpHost.mid(JumpHost::SessionPrefix.size());
            const auto gateway = m_sessions.constFind(id);
            if (gateway == m_sessions.cend()
                || gateway->protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
                QMessageBox::warning(this, QStringLiteral("Jump Host"),
                                     QStringLiteral("The jump host of %1 is no longer a saved SSH "
                                                    "session. Choose another in the session's or "
                                                    "folder's settings.")
                                         .arg(session.name));
                return false;
            }
            if (visited.contains(id)) {
                QMessageBox::warning(this, QStringLiteral("Jump Host"),
                                     QStringLiteral("The jump hosts of %1 lead back to %2. Check "
                                                    "the jump hosts of these sessions and their "
                                                    "folders.")
                                         .arg(session.name, gateway->name));
                return false;
            }
            visited.insert(id);
            hop = sshSettingsFor(gateway->host, gateway->port, gateway->username, sessionSshOptions(*gateway));
            hop.password = m_vault.passwordForSession(id);
            hop.savedSession = id;
            next = effectiveJumpHost(*gateway);
        } else {
            QString user, host;
            quint16 port = 0;
            if (!JumpHost::parseAddress(jumpHost, &user, &host, &port)) {
                QMessageBox::warning(this, QStringLiteral("Jump Host"),
                                     QStringLiteral("\"%1\" is not a jump host. Write it as "
                                                    "user@host or user@host:port.")
                                         .arg(jumpHost));
                return false;
            }
            hop = sshSettingsFor(host, port, user.isEmpty() ? session.username : user);
        }
        jumps->insert(jumps->begin(), std::move(hop));
        jumpHost = next;
    }
    return true;
}

// The SSH connection settings for a host, with the SSH preferences (no
// password; the caller adds it from the vault).
SshConnectionSettings MainWindow::sshSettingsFor(const QString &host, quint16 port,
                                                 const QString &username,
                                                 const SshOptions &options) const
{
    SshConnectionSettings settings;
    settings.host = host;
    settings.port = port == 0 ? 22 : port;
    settings.username = username;
    settings.knownHostsPath = stateFilePath(QStringLiteral("known_hosts"));
    const auto ssh = options.appliedTo(loadSshPreferences());
    settings.connectTimeout = ssh.connectTimeout;
    settings.keepaliveSeconds = ssh.keepaliveSeconds;
    settings.compression = ssh.compression;
    settings.tryPublicKeys = ssh.tryPublicKeys;
    settings.forwardAgent = ssh.forwardAgent;
    settings.followTerminalFolder = ssh.followTerminalFolder;
    settings.forwardX11 = ssh.forwardX11;
    settings.fileTransfer = ssh.fileTransfer;
    settings.agent = ssh.agent;
    // A relative key path (like settings/keys/id_ed25519) is in the
    // program's folder, so it moves with it.
    settings.identityFile = ssh.identityFile.isEmpty() || QDir::isAbsolutePath(ssh.identityFile)
                                ? ssh.identityFile
                                : portableFilePath(ssh.identityFile);
    // A passphrase remembered by the key manager; otherwise it is asked for.
    if (!ssh.identityFile.isEmpty())
        settings.keyPassphrase = m_vault.passwordForSession(keyPassphraseVaultKey(ssh.identityFile));
    settings.useCertificates = ssh.useCertificates;
    settings.certificateFile = options.certificateFile.isEmpty()
                                       || options.certificateFile == QStringLiteral("none")
                                       || QDir::isAbsolutePath(options.certificateFile)
                                   ? options.certificateFile
                                   : portableFilePath(options.certificateFile);
    settings.securityKeys = ssh.securityKeys;
    settings.kerberos = ssh.kerberos;
    settings.kerberosDelegate = ssh.kerberosDelegate;
    settings.kerberosOrder = ssh.kerberosOrder;
    settings.kerberosServer = options.kerberosServer;
    settings.terminalType = ssh.terminalType;
    if (!options.backspace.isEmpty())
        settings.backspace = options.backspace;
    settings.reconnect = ssh.reconnect;
    return settings;
}

// The Connection tab of Rlogin and RSH sessions, first in the shared tabs.
RshOptionsEditor *MainWindow::addRshOptionsTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                                               const RshOptions &value)
{
    QTabWidget *tabs = nullptr;
    for (auto *widget = sshTabsMember->parentWidget(); widget && !tabs; widget = widget->parentWidget())
        tabs = qobject_cast<QTabWidget *>(widget);
    auto *editor = new RshOptionsEditor(value, loadRshDefaults(), tabs);
    applyInheritance(editor); // ↺ per row, as in every session window
    editor->setProperty(ProtocolsProperty, QStringList{QStringLiteral("Rlogin"), QStringLiteral("RSH")});
    tabs->insertTab(0, editor, QStringLiteral("Connection"));
    auto updateCommand = [protocolCombo, editor] {
        editor->setCommandShown(protocolCombo->currentText() == QStringLiteral("RSH"));
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, editor, updateCommand);
    updateCommand();
    return editor;
}

TelnetOptionsEditor *MainWindow::addTelnetOptionsTab(TerminalAppearanceEditor *sshTabsMember,
                                                     const TelnetOptions &value)
{
    QTabWidget *tabs = nullptr;
    for (auto *widget = sshTabsMember->parentWidget(); widget && !tabs; widget = widget->parentWidget())
        tabs = qobject_cast<QTabWidget *>(widget);
    auto *editor = new TelnetOptionsEditor(value, loadTelnetDefaults(), tabs);
    applyInheritance(editor); // ↺ per row, as in every session window
    editor->setProperty(ProtocolsProperty, QStringList{QStringLiteral("Telnet")});
    tabs->insertTab(0, editor, QStringLiteral("Connection"));
    return editor;
}

// The session window's jump host choice (a card of the Network tab).
// folder: where the session is (for what "Inherit" means).
JumpHostEditor *MainWindow::newJumpHostEditor(const QString &value, const QTreeWidgetItem *folder,
                                              const QString &sessionId)
{
    auto *editor = new JumpHostEditor(value, jumpHostLabel(inheritedJumpHost(folder)),
                                      jumpHostChoices(), sessionId);
    editor->offerNew([this](QWidget *over) { return createSshSession(over); },
                     [this] { return jumpHostChoices(); });
    return editor;
}

// A tab shown with whichever protocol is selected in the session window,
// moved between the SSH, RDP and VNC settings' tabs.
// The session window's Automation tab, after Appearance, for terminal
// protocols.
AutomationEditor *MainWindow::addAutomationTab(TerminalAppearanceEditor *sshTabsMember,
                                               const AutomationOptions &value)
{
    QTabWidget *tabs = nullptr;
    for (auto *widget = sshTabsMember->parentWidget(); widget && !tabs; widget = widget->parentWidget())
        tabs = qobject_cast<QTabWidget *>(widget);
    auto *editor = new AutomationEditor(value, QStringLiteral("The folder's"), savedScriptNames(), tabs);
    editor->setProperty(ProtocolsProperty, sshTabsMember->property(ProtocolsProperty));
    tabs->insertTab(tabs->indexOf(sshTabsMember) + 1, editor, QStringLiteral("Automation"));
    return editor;
}

// The Connection tab of Local, WSL, Docker and Kubernetes sessions, first.
LocalSessionEditor *MainWindow::addLocalSessionTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                                                   const LocalSessionOptions &value, QLineEdit *passwordEdit,
                                                   const QString &sessionId)
{
    QTabWidget *tabs = nullptr;
    for (auto *widget = sshTabsMember->parentWidget(); widget && !tabs; widget = widget->parentWidget())
        tabs = qobject_cast<QTabWidget *>(widget);
    auto *editor = new LocalSessionEditor(
        value,
        [this] {
            QList<QPair<QString, QString>> choices;
            for (const auto &session : std::as_const(m_sessions)) {
                if (session.protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0)
                    choices.append({session.id, session.name});
            }
            std::sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) {
                return a.second.localeAwareCompare(b.second) < 0;
            });
            return choices;
        },
        [this](const QString &id) -> std::optional<SshConnectionSettings> {
            const auto session = m_sessions.constFind(id);
            if (session == m_sessions.cend())
                return std::nullopt;
            SshConnectionSettings settings;
            if (!sshSettingsForSession(*session, &settings) || !settings.jumps.empty())
                return std::nullopt;
            return settings;
        },
        tabs);
    editor->setProperty(ProtocolsProperty, localSessionProtocols());
    editor->setCredentialNames(&kubeCredentialNames);
    // For listing: the token typed above (or the stored one), or a saved
    // credential's kubeconfig.
    editor->setSecretSource([this, passwordEdit = QPointer<QLineEdit>(passwordEdit), sessionId](const LocalSessionOptions &options) {
        if (options.kubeAuth == QStringLiteral("saved"))
            return kubeCredentialContents(options.kubeCredential);
        if (passwordEdit && !passwordEdit->text().isEmpty())
            return passwordEdit->text();
        if (sessionId.isEmpty() || (m_vault.needsUnlock(sessionId) && !ensureVaultUnlocked()))
            return QString();
        return m_vault.passwordForSession(sessionId);
    });
    connect(editor, &LocalSessionEditor::manageCredentialsRequested, editor, [this, editor] {
        showKubeCredentials(editor->window());
        editor->setCredentialNames(&kubeCredentialNames);
    });
    tabs->insertTab(0, editor, QStringLiteral("Connection"));
    connect(protocolCombo, &QComboBox::currentTextChanged, editor, &LocalSessionEditor::setProtocol);
    editor->setProtocol(protocolCombo->currentText());
    return editor;
}

// A saved SSH session's connection, with its jump hosts, proxy and password
// (the vault is unlocked if needed); false when it cannot be made.
bool MainWindow::sshSettingsForSession(const SessionDefinition &session, SshConnectionSettings *settings)
{
    *settings = sshSettingsFor(session.host, session.port, session.username, sessionSshOptions(session));
    if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked())
        return false;
    if (!resolveJumpHosts(session, &settings->jumps))
        return false;
    applyProxy(session, settings, settings->jumps);
    settings->password = m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0));
    return true;
}

void MainWindow::addProtocolTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                                QWidget *rdpEditor, QWidget *vncEditor, QWidget *page,
                                const QString &title)
{
    QTabWidget *sshTabs = nullptr;
    for (auto *widget = sshTabsMember->parentWidget(); widget && !sshTabs;
         widget = widget->parentWidget())
        sshTabs = qobject_cast<QTabWidget *>(widget);
    QHash<QString, QTabWidget *> tabs{
        {QStringLiteral("SSH"), sshTabs},
        {QStringLiteral("SFTP"), sshTabs},
        {QStringLiteral("SCP"), sshTabs},
        {QStringLiteral("RDP"), rdpEditor->findChild<QTabWidget *>()},
        {QStringLiteral("VNC"), vncEditor->findChild<QTabWidget *>()}};
    // Telnet, Mosh, Rlogin, RSH, XDMCP and FTP have it too (for Wake-on-LAN;
    // a serial line has no host to wake, a local session no network).
    QStringList protocols;
    for (const auto &protocol : sessionProtocols()) {
        if (protocol != QStringLiteral("Serial") && !localSessionProtocols().contains(protocol))
            protocols << protocol;
    }
    page->setProperty(ProtocolsProperty, protocols);
    for (const auto *name : {"Telnet", "Mosh", "Rlogin", "RSH"})
        tabs.insert(QLatin1String(name), sshTabs);
    if (auto *xdmcpTabs = protocolCombo->window()->findChild<QTabWidget *>(QStringLiteral("xdmcpTabs")))
        tabs.insert(QStringLiteral("XDMCP"), xdmcpTabs);
    if (auto *ftpTabs = protocolCombo->window()->findChild<QTabWidget *>(QStringLiteral("ftpTabs")))
        tabs.insert(QStringLiteral("FTP"), ftpTabs);
    auto place = [protocolCombo, page, tabs, title] {
        auto *target = tabs.value(protocolCombo->currentText());
        for (auto *other : tabs) {
            if (other && other != target && other->indexOf(page) >= 0)
                other->removeTab(other->indexOf(page));
        }
        if (target && target->indexOf(page) < 0)
            target->addTab(page, title);
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, page, place);
    place();
}

// The session window's proxy (a card of the Network tab).
ProxyEditor *MainWindow::newProxyEditor(const QString &value, const QTreeWidgetItem *folder)
{
    return new ProxyEditor(value, QString(), ProxySettings::label(inheritedProxy(folder)));
}

// The session window's Network tab: jump host, port forwarding, proxy and
// Wake-on-LAN as cards, each shown for the protocols it applies to.
void MainWindow::addNetworkTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                               QWidget *rdpEditor, QWidget *vncEditor, JumpHostEditor *jumpHost,
                               PortForwardListEditor *forwards, ProxyEditor *proxy, QWidget *wakeOnLan)
{
    auto *page = new QWidget;
    auto *cards = new QVBoxLayout(page);
    cards->setSpacing(6);
    const QStringList ssh{QStringLiteral("SSH"), QStringLiteral("SFTP"), QStringLiteral("SCP")};
    const QList<std::pair<QWidget *, QStringList>> shown{
        {addWidgetCard(cards, QStringLiteral("Jump host"), jumpHost),
         ssh + QStringList{QStringLiteral("RDP"), QStringLiteral("VNC")}},
        {addWidgetCard(cards, QStringLiteral("Proxy"), proxy),
         ssh + QStringList{QStringLiteral("RDP"), QStringLiteral("VNC"), QStringLiteral("FTP")}},
        {addWidgetCard(cards, QStringLiteral("Port forwarding"), forwards), ssh},
        {addWidgetCard(cards, QStringLiteral("Wake-on-LAN"), wakeOnLan), {}}, // every one with a host
    };
    cards->addStretch();
    alignFormLabels(page);
    const auto update = [protocolCombo, proxy, shown] {
        const auto protocol = protocolCombo->currentText();
        for (const auto &[card, protocols] : shown)
            card->setVisible(protocols.isEmpty() || protocols.contains(protocol));
        proxy->setProtocol(protocol);
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, page, update);
    update();
    // Four cards are more than a window's height: the tab scrolls.
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);
    addProtocolTab(protocolCombo, sshTabsMember, rdpEditor, vncEditor, scroll, QStringLiteral("Network"));
}

// What an empty proxy means in a folder: the nearest folder (this one or
// above) that sets one, then the Preferences.
QString MainWindow::inheritedProxy(const QTreeWidgetItem *folder) const
{
    for (; folder; folder = folder->parent()) {
        const auto proxy = folder->data(0, FolderProxyRole).toString();
        if (!proxy.isEmpty())
            return proxy;
    }
    return loadSshPreferences().proxy;
}

bool MainWindow::inheritedKerberos(const QTreeWidgetItem *folder) const
{
    for (; folder; folder = folder->parent()) {
        if (const auto own = folder->data(0, FolderKerberosRole); own.isValid())
            return own.toInt() == 1;
    }
    return loadSshPreferences().kerberos;
}

// The first login script and triggers set on a folder or its parents; mode
// "own" when one was found.
AutomationOptions MainWindow::inheritedAutomation(const QTreeWidgetItem *folder) const
{
    for (; folder; folder = folder->parent()) {
        const auto own = automationFromText(folder->data(0, FolderAutomationRole).toString());
        if (!own.mode.isEmpty())
            return own;
    }
    return {};
}

// Starts a terminal tab's login script and triggers: the session's own, or
// its folders', with the Preferences' global triggers first.
void MainWindow::applySessionAutomation(TerminalPage *page, const SessionDefinition &session)
{
    auto options = session.automation;
    if (options.mode.isEmpty()) {
        const QTreeWidgetItem *item = nullptr;
        for (QTreeWidgetItemIterator it(m_sessionTree); *it && !item; ++it) {
            if ((*it)->data(0, SessionIdRole).toString() == session.id.section(QLatin1Char('#'), 0, 0)
                && !isQuickItem(*it))
                item = *it;
        }
        options = inheritedAutomation(item ? item->parent() : nullptr);
    }
    const auto preferences = loadAutomationPreferences();
    const bool own = options.mode == QStringLiteral("own");
    auto triggers = preferences.globalTriggers;
    if (own)
        triggers += options.triggers;
    QHash<QString, QString> values{{QStringLiteral("user"), session.username},
                                   {QStringLiteral("host"), session.host}};
    const auto script = own ? options.loginScript : QList<LoginStep>();
    if (!script.isEmpty())
        values.insert(QStringLiteral("password"),
                      m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0)));
    page->setAutomation(script, triggers, preferences, values);
    if (own && !options.afterLoginScript.isEmpty()) {
        const auto name = options.afterLoginScript;
        connect(page, &TerminalPage::loggedOn, this, [this, page, name] { runSavedScriptInTab(page, name); });
    }
}

// A session's SSH options with what its folders set (Kerberos) filled in.
SshOptions MainWindow::sessionSshOptions(const SessionDefinition &session) const
{
    auto options = session.ssh;
    if (options.kerberos < 0) {
        for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
            if ((*it)->data(0, SessionIdRole).toString() == session.id && !isQuickItem(*it)) {
                options.kerberos = inheritedKerberos((*it)->parent()) ? 1 : 0;
                break;
            }
        }
    }
    return options;
}

ProxySettings MainWindow::effectiveProxy(const SessionDefinition &session) const
{
    QString value = session.proxy;
    if (value.isEmpty()) {
        const QTreeWidgetItem *item = nullptr;
        for (QTreeWidgetItemIterator it(m_sessionTree); *it && !item; ++it) {
            if ((*it)->data(0, SessionIdRole).toString() == session.id && !isQuickItem(*it))
                item = *it;
        }
        value = inheritedProxy(item ? item->parent() : nullptr);
    }
    auto proxy = ProxySettings::parse(value);
    if (proxy.isSet() && !proxy.user.isEmpty())
        proxy.password = m_vault.passwordForSession(proxy.vaultKey());
    return proxy;
}

// Puts a session's proxy on the first connection: the first jump host, or
// the server itself.
void MainWindow::applyProxy(const SessionDefinition &session, SshConnectionSettings *server,
                            std::vector<SshConnectionSettings> &jumps) const
{
    const auto proxy = effectiveProxy(session);
    if (!proxy.isSet())
        return;
    if (!jumps.empty())
        jumps.front().proxy = proxy;
    else if (server)
        server->proxy = proxy;
}

// Stores a proxy password typed in a Proxy tab (blank keeps the stored one).
void MainWindow::saveProxyPassword(const QString &value, QString password)
{
    const auto proxy = ProxySettings::parse(value);
    if (password.isEmpty() || !proxy.isSet() || proxy.user.isEmpty())
        return;
    if (!m_vault.needsVault(proxy.vaultKey()) || ensureVaultUnlocked()) {
        QString error;
        if (!m_vault.setPassword(proxy.vaultKey(), password, &error))
            QMessageBox::warning(this, QStringLiteral("Could not save password"), error);
    }
    password.fill(QChar(0));
}

// A saved tunnel's SSH server: a saved SSH session (with its jump hosts and
// password) or user@host[:port].
bool MainWindow::resolveTunnelServer(const QString &server, SshConnectionSettings *settings)
{
    if (server.startsWith(JumpHost::SessionPrefix)) {
        const auto id = server.mid(JumpHost::SessionPrefix.size());
        const auto session = m_sessions.constFind(id);
        if (session == m_sessions.cend()
            || session->protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                                 QStringLiteral("The tunnel's SSH server is no longer a saved SSH "
                                                "session. Choose another in the tunnel's settings."));
            return false;
        }
        *settings = sshSettingsFor(session->host, session->port, session->username, sessionSshOptions(*session));
        if (!resolveJumpHosts(*session, &settings->jumps))
            return false;
        applyProxy(*session, settings, settings->jumps);
        // Without the vault, keys and the log-on prompt still work.
        if (m_vault.needsUnlock(id))
            ensureVaultUnlocked();
        settings->password = m_vault.passwordForSession(id);
        return true;
    }
    QString user, host;
    quint16 port = 0;
    if (!JumpHost::parseAddress(server, &user, &host, &port)) {
        QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                             QStringLiteral("\"%1\" is not an SSH server. Write it as "
                                            "user@host[:port].").arg(server));
        return false;
    }
    *settings = sshSettingsFor(host, port, user);
    settings->proxy = ProxySettings::parse(loadSshPreferences().proxy);
    if (settings->proxy.isSet() && !settings->proxy.user.isEmpty())
        settings->proxy.password = m_vault.passwordForSession(settings->proxy.vaultKey());
    return true;
}

// A tunnel's server with its jump hosts: a session's own, or (for a
// user@host server) the tunnel's, each a session or user@host[:port].
bool MainWindow::resolveTunnel(const SavedTunnel &tunnel, SshConnectionSettings *settings)
{
    if (!resolveTunnelServer(tunnel.server, settings))
        return false;
    if (tunnel.server.startsWith(JumpHost::SessionPrefix) || tunnel.via.isEmpty())
        return true;
    settings->jumps.clear();
    for (const auto &hop : tunnel.via) {
        SshConnectionSettings jump;
        if (hop.trimmed().isEmpty())
            continue;
        if (hop.startsWith(JumpHost::SessionPrefix)) {
            const auto id = hop.mid(JumpHost::SessionPrefix.size());
            const auto gateway = m_sessions.constFind(id);
            if (gateway == m_sessions.cend()) {
                QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                                     QStringLiteral("A jump host of the tunnel is no longer a saved session."));
                return false;
            }
            jump = sshSettingsFor(gateway->host, gateway->port, gateway->username, sessionSshOptions(*gateway));
            jump.password = m_vault.passwordForSession(id);
            jump.savedSession = id;
        } else {
            QString user, host;
            quint16 port = 0;
            if (!JumpHost::parseAddress(hop, &user, &host, &port)) {
                QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                                     QStringLiteral("\"%1\" is not a jump host. Write it as user@host[:port].")
                                         .arg(hop));
                return false;
            }
            jump = sshSettingsFor(host, port, user.isEmpty() ? settings->username : user);
        }
        settings->jumps.push_back(std::move(jump));
    }
    // The proxy (if any) is for the first connection made.
    if (!settings->jumps.empty() && settings->proxy.isSet()) {
        settings->jumps.front().proxy = settings->proxy;
        settings->proxy = ProxySettings();
    }
    return true;
}

TunnelServerInfo MainWindow::describeTunnelServer(const QString &server, const QStringList &via) const
{
    TunnelServerInfo info;
    const auto address = [](const QString &user, const QString &host, quint16 port) {
        return (user.isEmpty() ? host : QStringLiteral("%1@%2").arg(user, host))
               + (port && port != 22 ? QStringLiteral(":%1").arg(port) : QString());
    };
    const auto describeHop = [this, &address](const QString &hop) {
        if (hop.startsWith(JumpHost::SessionPrefix)) {
            const auto session = m_sessions.constFind(hop.mid(JumpHost::SessionPrefix.size()));
            return session == m_sessions.cend() ? QStringLiteral("(deleted session)")
                                                : address(session->username, session->host, session->port);
        }
        return hop;
    };
    if (server.startsWith(JumpHost::SessionPrefix)) {
        const auto id = server.mid(JumpHost::SessionPrefix.size());
        const auto session = m_sessions.constFind(id);
        if (session == m_sessions.cend())
            return info;
        info.valid = true;
        info.user = session->username;
        info.host = session->host;
        info.port = session->port ? session->port : 22;
        info.label = QStringLiteral("%1 (%2)").arg(session->name, address(info.user, info.host, info.port));
        info.fromSession = true;
        // The session's chain of jump hosts, nearest first (names only).
        QString jump = effectiveJumpHost(*session);
        QSet<QString> seen{id};
        while (!jump.isEmpty() && jump != JumpHost::Direct && info.jumps.size() < 10) {
            info.jumps.prepend(describeHop(jump));
            if (!jump.startsWith(JumpHost::SessionPrefix))
                break;
            const auto next = m_sessions.constFind(jump.mid(JumpHost::SessionPrefix.size()));
            if (next == m_sessions.cend() || seen.contains(next->id))
                break;
            seen.insert(next->id);
            jump = effectiveJumpHost(*next);
        }
        return info;
    }
    QString user, host;
    quint16 port = 0;
    if (!JumpHost::parseAddress(server, &user, &host, &port))
        return info;
    info.valid = true;
    info.user = user;
    info.host = host;
    info.port = port ? port : 22;
    info.label = address(user, host, info.port);
    for (const auto &hop : via) {
        if (!hop.trimmed().isEmpty())
            info.jumps << describeHop(hop);
    }
    return info;
}

// Saved SSH sessions that can be a jump host: (id, label).
QList<QPair<QString, QString>> MainWindow::jumpHostChoices() const
{
    QList<QPair<QString, QString>> choices;
    for (const auto &session : m_sessions) {
        if (session.protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0)
            continue;
        const QString target = session.username.isEmpty()
                                   ? session.host
                                   : QStringLiteral("%1@%2").arg(session.username, session.host);
        choices.append({session.id, QStringLiteral("%1 (%2)").arg(session.name, target)});
    }
    std::sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) {
        return a.second.localeAwareCompare(b.second) < 0;
    });
    return choices;
}

// Preferences → Security → Password storage: copies saved passwords from the
// vault to the system store, or the saved sessions' ones back. A window
// shows first what would happen (passwords themselves are never shown):
// new ones are added, ones the target has with another value are replaced
// only when ticked. Returns what happened, for a message; empty when the
// user cancelled.
QString MainWindow::copyPasswords(bool toSystem)
{
    QString why;
    if (!SystemCredentials::isAvailable(&why))
        return why;
    if (m_vault.exists() && !m_vault.isUnlocked() && !ensureVaultUnlocked())
        return QStringLiteral("The vault stays locked; nothing was copied.");
    if (!toSystem && !m_vault.exists() && !ensureVaultUnlocked())
        return QStringLiteral("There is no vault to copy into.");

    // What would be copied, and what the target holds for it now.
    struct Entry
    {
        QString key, name, password;
        enum { New, Same, Different } status;
    };
    QList<Entry> entries;
    const auto nameOf = [this](const QString &key) {
        for (const auto &session : std::as_const(m_sessions))
            if (session.id == key)
                return session.name;
        return key; // not a session's password
    };
    const auto add = [&](const QString &key, const QString &password, const QString &existing) {
        entries.append({key, nameOf(key), password,
                        existing.isEmpty() ? Entry::New
                        : existing == password ? Entry::Same
                                               : Entry::Different});
    };
    if (toSystem) {
        for (const auto &key : m_vault.vaultKeys())
            add(key, m_vault.vaultPassword(key), SystemCredentials::read(key));
    } else {
        for (const auto &session : std::as_const(m_sessions)) {
            const QString password = SystemCredentials::read(session.id);
            if (!password.isEmpty())
                add(session.id, password, m_vault.vaultPassword(session.id));
        }
    }
    const auto target = toSystem ? SystemCredentials::name() : QStringLiteral("the vault");
    if (entries.isEmpty())
        return QStringLiteral("There are no saved passwords to copy to %1.").arg(target);
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    int counts[3] = {0, 0, 0};
    for (const auto &entry : entries)
        ++counts[entry.status];

    QDialog dialog(QApplication::activeWindow() ? QApplication::activeWindow() : this);
    dialog.setWindowTitle(QStringLiteral("Copy Passwords to %1").arg(toSystem ? QStringLiteral("the System Store")
                                                                              : QStringLiteral("the Vault")));
    dialog.setSizeGripEnabled(true);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("<b>New: %1</b> · Same: %2 · <b>Different: %3</b>")
                                     .arg(counts[Entry::New]).arg(counts[Entry::Same]).arg(counts[Entry::Different])));
    auto *explanation = new QLabel(
        QStringLiteral("New passwords are added to %1. A different one replaces the one there only "
                       "if you tick it. Nothing is deleted, and the passwords are not shown.")
            .arg(target));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    if (toSystem) {
        auto *warning = new QLabel(QStringLiteral(
            "The system store opens with your login, while the vault needs its own master "
            "password: copied passwords are protected less there."));
        warning->setWordWrap(true);
        warning->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
        layout->addWidget(warning);
    }
    auto *table = new QTableWidget(int(entries.size()), 3);
    table->setHorizontalHeaderLabels({QStringLiteral("Password of"), QStringLiteral("In %1").arg(target),
                                      QStringLiteral("Replace")});
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    QList<QCheckBox *> replaceBoxes;
    for (int row = 0; row < entries.size(); ++row) {
        const auto &entry = entries.at(row);
        table->setItem(row, 0, new QTableWidgetItem(entry.name));
        table->setItem(row, 1, new QTableWidgetItem(entry.status == Entry::New    ? QStringLiteral("not there: added")
                                                    : entry.status == Entry::Same ? QStringLiteral("the same")
                                                                                  : QStringLiteral("different")));
        if (entry.status == Entry::Different) {
            auto *box = new QCheckBox;
            box->setToolTip(QStringLiteral("Replace the password in %1 with this one").arg(target));
            table->setCellWidget(row, 2, box);
            replaceBoxes.append(box);
        } else {
            replaceBoxes.append(nullptr);
        }
    }
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *copy = buttons->addButton(QStringLiteral("Copy"), QDialogButtonBox::AcceptRole);
    if (counts[Entry::Different] > 0) {
        auto *all = buttons->addButton(QStringLiteral("Replace All"), QDialogButtonBox::ActionRole);
        auto *none = buttons->addButton(QStringLiteral("Replace None"), QDialogButtonBox::ActionRole);
        for (auto [button, on] : {std::pair{all, true}, std::pair{none, false}})
            QObject::connect(button, &QPushButton::clicked, &dialog, [replaceBoxes, on] {
                for (auto *box : replaceBoxes)
                    if (box)
                        box->setChecked(on);
            });
    }
    copy->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(560, 420);
    if (dialog.exec() != QDialog::Accepted) {
        for (auto &entry : entries)
            entry.password.fill(QChar(0));
        return QString();
    }

    int added = 0, replaced = 0;
    QStringList problems;
    for (int row = 0; row < entries.size(); ++row) {
        auto &entry = entries[row];
        const bool write = entry.status == Entry::New
                           || (entry.status == Entry::Different && replaceBoxes.at(row)->isChecked());
        if (write) {
            QString error;
            const bool ok = toSystem ? SystemCredentials::write(entry.key, entry.password, &error)
                                     : m_vault.setVaultPassword(entry.key, entry.password, &error);
            if (ok)
                ++(entry.status == Entry::New ? added : replaced);
            else
                problems.append(error);
        }
        entry.password.fill(QChar(0));
    }
    problems.removeDuplicates();
    QString message = QStringLiteral("%1 added and %2 replaced in %3; %4 left as they were.")
                          .arg(added).arg(replaced).arg(target)
                          .arg(entries.size() - added - replaced);
    if (!problems.isEmpty())
        message += QStringLiteral("\n\n") + problems.join(QLatin1Char('\n'));
    return message;
}

// "Keep the password in": the session's own store, or the Preferences'.
QComboBox *MainWindow::passwordStoreChoice(const QString &value, QWidget *parent) const
{
    static const QHash<QString, QString> labels{{QStringLiteral("vault"), QStringLiteral("OpsRemote vault")},
                                                {QStringLiteral("system"), QStringLiteral("System credential store")},
                                                {QStringLiteral("both"), QStringLiteral("Both")}};
    auto *box = new QComboBox(parent);
    addInheritedItem(box, labels.value(m_security.passwordStore), QString());
    for (const auto *key : {"vault", "system", "both"})
        if (QLatin1String(key) != m_security.passwordStore) // listed once, as the inherited one
            box->addItem(labels.value(QLatin1String(key)), QLatin1String(key));
    box->setCurrentIndex(qMax(0, box->findData(value)));
    QString why;
    const bool available = SystemCredentials::isAvailable(&why);
    box->setToolTip(QStringLiteral("The vault travels with the portable folder; the system store (%1) "
                                   "needs no master password but stays on this computer.%2")
                        .arg(SystemCredentials::name(),
                             available ? QString() : QStringLiteral("\n\nNot available here: ") + why));
    // Without a system store, its choices are off (a session's current one stays).
    if (!available) {
        if (auto *model = qobject_cast<QStandardItemModel *>(box->model())) {
            for (const auto *key : {"system", "both"}) {
                const int row = box->findData(QLatin1String(key));
                if (auto *item = model->item(row); item && row != box->currentIndex())
                    item->setEnabled(false);
            }
        }
    }
    return box;
}

// Save in the session window: checks the fields first and keeps the window
// open with a message in it (and the missing fields marked) until they are
// complete, instead of closing and complaining in another window.
void MainWindow::validateSessionDialog(QDialog *dialog, QFormLayout *form, QDialogButtonBox *buttons,
                                       QComboBox *protocolCombo, QLineEdit *nameEdit,
                                       QLineEdit *hostEdit, QLineEdit *portEdit,
                                       WakeOnLanEditor *wakeEditor)
{
    auto *message = new QLabel(dialog);
    message->setWordWrap(true);
    message->setStyleSheet(QStringLiteral("color: #c0392b; font-weight: bold;"));
    message->hide();
    form->insertRow(form->rowCount() - 1, message);
    const QString marked = QStringLiteral("QLineEdit { border: 1px solid #d9534f; border-radius: 3px; }");
    for (auto *field : {nameEdit, hostEdit, portEdit}) {
        connect(field, &QLineEdit::textEdited, field, [field, message] {
            field->setStyleSheet(QString());
            message->hide();
        });
    }
    connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        const bool serial = protocolCombo->currentText() == QStringLiteral("Serial");
        QStringList missing;
        QLineEdit *first = nullptr;
        const auto require = [&](QLineEdit *field, bool ok, const QString &label) {
            field->setStyleSheet(ok ? QString() : marked);
            if (!ok) {
                missing.append(label);
                if (!first)
                    first = field;
            }
        };
        require(nameEdit, !nameEdit->text().trimmed().isEmpty(), QStringLiteral("Name"));
        require(hostEdit, !hostEdit->text().trimmed().isEmpty() || !protocolNeedsHost(protocolCombo->currentText()),
                serial ? QStringLiteral("Port (the serial port)") : QStringLiteral("Host"));
        QString text;
        if (!missing.isEmpty())
            text = QStringLiteral("Fill in the required fields to save the connection: %1.")
                       .arg(missing.join(QStringLiteral(", ")));
        bool portOk = true;
        const QString port = portEdit->text().trimmed();
        if (!serial && !port.isEmpty()) {
            const uint number = port.toUInt(&portOk);
            portOk = portOk && number >= 1 && number <= 65535;
        }
        if (!portOk) {
            portEdit->setStyleSheet(marked);
            if (!first)
                first = portEdit;
            text += (text.isEmpty() ? QString() : QStringLiteral(" "))
                    + QStringLiteral("The port must be a number from 1 to 65535, or empty for the "
                                     "protocol's usual one.");
        }
        if (text.isEmpty()) {
            if (const auto problem = wakeEditor->problem(); !problem.isEmpty())
                text = QStringLiteral("Wake-on-LAN: %1").arg(problem);
        }
        if (text.isEmpty()) {
            dialog->accept();
            return;
        }
        message->setText(text);
        message->show();
        if (first)
            first->setFocus();
    });
}
