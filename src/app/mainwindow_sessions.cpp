// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// MainWindow: the session list: sessions.yaml, folders, import and export, recent sessions, filtering and fitting the side panel, host status.

#include "mainwindow_p.h"

using namespace mainwindow_p;

void MainWindow::addExampleSessions()
{
    auto *examples = new QTreeWidgetItem(m_sessionTree, {QStringLiteral("Examples")});
    setItemKind(examples, ItemKind::Folder);
    auto *linuxFolder = new QTreeWidgetItem(examples, {QStringLiteral("Linux")});
    setItemKind(linuxFolder, ItemKind::Folder);
    auto *servers = new QTreeWidgetItem(linuxFolder, {QStringLiteral("Servers")});
    setItemKind(servers, ItemKind::Folder);

    SessionDefinition sample;
    sample.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sample.name = QStringLiteral("Example SSH Session");
    sample.protocol = QStringLiteral("SSH");
    sample.host = QStringLiteral("server.example.com");
    sample.port = 22;
    addSession(sample, servers);

    examples->setExpanded(true);
    linuxFolder->setExpanded(true);
    servers->setExpanded(true);
}

// Every section of settings.yaml with its defaults, as OpsRemote saves
// them: tests/settings-migration/schema.yaml holds it, so a change of any
// key or default shows in its diff and asks whether a migration is needed.
// Nothing is read from or written to settings.yaml meanwhile. A new settings
// writer belongs here too.
QByteArray settingsSnapshot()
{
    YAML::Node document(YAML::NodeType::Map);
    document["settings_version"] = CurrentSettingsVersion;
    captureSettings(&document);
    // Appearance (this file).
    saveUiStylePreference(loadUiStylePreference());
    saveColorThemePreference(loadColorThemePreference(), loadCustomColors());
    saveIconThemePreference(loadIconThemePreference());
    saveInterfaceFont(loadInterfaceFont());
    saveWindowBorderPreference(loadWindowBorderPreference());
    saveCornerRadiusPreference(loadCornerRadiusPreference());
    saveSeeThroughListsPreference(loadSeeThroughListsPreference());
    saveFolderExpansionStates({});
    saveTerminalAppearance(loadTerminalAppearance());
    saveDarkTerminalColorScheme(loadDarkTerminalColorScheme());
    // preferences.cpp
    saveStartupPreferences(loadStartupPreferences());
    saveOpenSessions({});
    saveWindowGeometry({});
    saveTerminalBehaviour(loadTerminalBehaviour());
    saveMouseClipboardPreferences(loadMouseClipboardPreferences());
    saveSshPreferences(loadSshPreferences());
    saveSftpPreferences(loadSftpPreferences());
    saveSecurityPreferences(loadSecurityPreferences());
    saveLoggingPreferences(loadLoggingPreferences());
    saveLocalServersPreferences(loadLocalServersPreferences());
    saveLocalShellPreferences(loadLocalShellPreferences());
    saveSessionListPreferences(loadSessionListPreferences());
    savePresentationPreferences(loadPresentationPreferences());
    saveBackupPreferences(loadBackupPreferences());
    saveRunCommandPreferences(loadRunCommandPreferences());
    saveHttpRequestPreferences(loadHttpRequestPreferences());
    saveCommandsPreferences(loadCommandsPreferences());
    saveSnippets(loadSnippets());
    saveHighlightRules(loadHighlightRules());
    saveShortcuts(loadShortcuts());
    // Modules with their own section.
    saveAutomationPreferences(loadAutomationPreferences());
    saveScriptPreferences(loadScriptPreferences());
    saveContainerPreferences(loadContainerPreferences());
    saveFtpDefaults(loadFtpDefaults());
    saveRshDefaults(loadRshDefaults());
    saveTelnetDefaults(loadTelnetDefaults());
    saveVncDefaults(loadVncDefaults());
    saveRdpDefaults(loadRdpDefaults());
    saveXdmcpDefaults(loadXdmcpDefaults());
    captureSettings(nullptr);
    YAML::Emitter emitter;
    emitter << document;
    return QByteArray(emitter.c_str()) + '\n';
}

void MainWindow::loadSessions()
{
    const auto dataDirectory = stateDirectory();
    const auto yamlPath = QDir(dataDirectory).filePath(QStringLiteral("sessions.yaml"));
    const auto legacyJsonPath = QDir(dataDirectory).filePath(QStringLiteral("sessions.json"));
    const bool importingLegacyJson = !QFile::exists(yamlPath) && QFile::exists(legacyJsonPath);
    const auto sourcePath = importingLegacyJson ? legacyJsonPath : yamlPath;
    bool migrateToCompactFormat = importingLegacyJson;

    if (!QFile::exists(sourcePath)) {
        addExampleSessions();
        saveSessions();
        return;
    }

    try {
        const auto document = YAML::LoadFile(sourcePath.toStdString());
        const auto version = document["version"].as<int>(1);
        migrateToCompactFormat = migrateToCompactFormat || version < 2;
        QString error;
        const bool loaded = version == 2
            ? appendCompactSessionYaml(document["folder"], nullptr, &error, document["sessions"])
            : appendSessionYaml(document["items"], nullptr, &error);
        if (!loaded)
            throw YAML::Exception(YAML::Mark::null_mark(), error.toStdString());
    } catch (const YAML::Exception &error) {
        QMessageBox::warning(this, QStringLiteral("Invalid sessions file"),
                             QStringLiteral("Could not read %1: %2")
                                 .arg(sourcePath, QString::fromLocal8Bit(error.what())));
        return;
    }

    if (migrateToCompactFormat)
        saveSessions();
}

bool MainWindow::appendSessionYaml(const YAML::Node &roots, QTreeWidgetItem *parent,
                                  QString *error)
{
    if (!roots || !roots.IsSequence()) {
        if (error)
            *error = QStringLiteral("The YAML document must contain an 'items' sequence.");
        return false;
    }

    QSet<QString> importedIds;
    const int startingDepth = parent ? folderDepth(parent) : 0;
    std::function<bool(const YAML::Node &, int)> validate;
    validate = [&](const YAML::Node &node, int depth) {
        if (!node.IsMap()) {
            if (error) *error = QStringLiteral("Each session or folder must be a YAML mapping.");
            return false;
        }
        if (node["password"] || node["credential"] || node["credentials"]) {
            if (error) *error = QStringLiteral("Passwords cannot be imported from session YAML. Use the credential vault.");
            return false;
        }

        const auto kind = QString::fromStdString(node["kind"].as<std::string>(""));
        if (kind == QStringLiteral("folder")) {
            const auto name = QString::fromStdString(node["name"].as<std::string>("" )).trimmed();
            if (name.isEmpty() || depth + 1 > MaximumFolderDepth) {
                if (error) *error = QStringLiteral("Folder names must be set and nesting cannot exceed %1 levels.").arg(MaximumFolderDepth);
                return false;
            }
            const auto children = node["children"];
            if (children && !children.IsSequence()) {
                if (error) *error = QStringLiteral("Folder children must be a YAML sequence.");
                return false;
            }
            if (children) {
                for (const auto &child : children) {
                    if (!validate(child, depth + 1))
                        return false;
                }
            }
            return true;
        }
        if (kind != QStringLiteral("session")) {
            if (error) *error = QStringLiteral("Unknown item kind in session YAML.");
            return false;
        }

        const auto value = node["session"];
        if (!value || !value.IsMap() || value["password"] || value["credential"]
            || value["credentials"]) {
            if (error) *error = QStringLiteral("Session data must be a mapping without inline credentials.");
            return false;
        }
        const auto name = QString::fromStdString(value["name"].as<std::string>(
            node["name"].as<std::string>(""))).trimmed();
        const auto protocol = QString::fromStdString(value["protocol"].as<std::string>(""));
        const auto host = QString::fromStdString(value["host"].as<std::string>(""));
        // Earlier names (Docker, Kubernetes) are today's container types.
        if (name.isEmpty() || protocol.isEmpty()
            || (host.isEmpty() && protocolNeedsHost(currentProtocolName(protocol)))) {
            if (error) *error = QStringLiteral("Every session needs a name, protocol, and host.");
            return false;
        }
        const auto port = value["port"].as<int>(0);
        if (port < 0 || port > 65535) {
            if (error) *error = QStringLiteral("A session port must be between 0 and 65535.");
            return false;
        }
        const auto id = QString::fromStdString(value["id"].as<std::string>(""));
        if (!id.isEmpty()) {
            if (importedIds.contains(id)) {
                if (error) *error = QStringLiteral("Session IDs must be unique in an import file.");
                return false;
            }
            importedIds.insert(id);
        }
        return true;
    };

    for (const auto &root : roots) {
        if (!validate(root, startingDepth))
            return false;
    }

    std::function<void(const YAML::Node &, QTreeWidgetItem *)> add;
    add = [this, &add](const YAML::Node &node, QTreeWidgetItem *destination) {
        const auto kind = QString::fromStdString(node["kind"].as<std::string>());
        if (kind == QStringLiteral("folder")) {
            const auto name = QString::fromStdString(node["name"].as<std::string>());
            auto *folder = destination ? new QTreeWidgetItem(destination, {name})
                                       : new QTreeWidgetItem(m_sessionTree, {name});
            setItemKind(folder, ItemKind::Folder);
            const auto children = node["children"];
            if (children) {
                for (const auto &child : children)
                    add(child, folder);
            }
            folder->setExpanded(true);
            return;
        }

        const auto value = node["session"];
        SessionDefinition session;
        session.id = QString::fromStdString(value["id"].as<std::string>(""));
        if (session.id.isEmpty() || m_sessions.contains(session.id))
            session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        session.name = QString::fromStdString(value["name"].as<std::string>(
            node["name"].as<std::string>("")));
        session.protocol = QString::fromStdString(value["protocol"].as<std::string>());
        session.host = QString::fromStdString(value["host"].as<std::string>());
        session.port = static_cast<quint16>(value["port"].as<int>(0));
        session.username = QString::fromStdString(value["username"].as<std::string>(""));
        addSession(session, destination);
    };

    for (const auto &root : roots)
        add(root, parent);
    return true;
}

bool MainWindow::appendCompactSessionYaml(const YAML::Node &folders, QTreeWidgetItem *parent,
                                          QString *error, const YAML::Node &rootSessions)
{
    if (!folders || !folders.IsSequence()) {
        if (error)
            *error = QStringLiteral("The YAML document must contain a 'folder' sequence.");
        return false;
    }

    QSet<QString> importedIds;
    const auto validateSessions = [&](const YAML::Node &sessions) {
        if (sessions && !sessions.IsSequence()) {
            if (error) *error = QStringLiteral("Folder sessions must be a YAML sequence.");
            return false;
        }
        if (!sessions)
            return true;
        for (const auto &session : sessions) {
            if (!session.IsMap() || session["password"] || session["credential"]
                || session["credentials"]) {
                if (error) *error = QStringLiteral("Session data must be a mapping without inline credentials.");
                return false;
            }
            const auto sessionName = QString::fromStdString(
                session["name"].as<std::string>("")).trimmed();
            const auto protocol = QString::fromStdString(session["protocol"].as<std::string>(""));
            const auto host = QString::fromStdString(session["host"].as<std::string>(""));
            const auto port = session["port"].as<int>(0);
            if (sessionName.isEmpty() || protocol.isEmpty()
                || (host.isEmpty() && protocolNeedsHost(currentProtocolName(protocol)))
                || port < 0 || port > 65535) {
                if (error) *error = QStringLiteral("Every session needs a name, protocol, host, and valid port.");
                return false;
            }
            const auto id = QString::fromStdString(session["id"].as<std::string>(""));
            if (!id.isEmpty()) {
                if (importedIds.contains(id)) {
                    if (error) *error = QStringLiteral("Session IDs must be unique in an import file.");
                    return false;
                }
                importedIds.insert(id);
            }
        }
        return true;
    };
    const int startingDepth = parent ? folderDepth(parent) : 0;
    std::function<bool(const YAML::Node &, int)> validateFolder;
    validateFolder = [&](const YAML::Node &folder, int depth) {
        if (!folder.IsMap()) {
            if (error) *error = QStringLiteral("Each folder must be a YAML mapping.");
            return false;
        }
        if (folder["password"] || folder["credential"] || folder["credentials"]) {
            if (error) *error = QStringLiteral("Passwords cannot be imported from session YAML.");
            return false;
        }
        const auto name = QString::fromStdString(folder["name"].as<std::string>("")).trimmed();
        if (name.isEmpty() || depth + 1 > MaximumFolderDepth) {
            if (error) *error = QStringLiteral("Folder names must be set and nesting cannot exceed %1 levels.").arg(MaximumFolderDepth);
            return false;
        }
        if (!validateSessions(folder["sessions"]))
            return false;
        const auto children = folder["folder"];
        if (children && !children.IsSequence()) {
            if (error) *error = QStringLiteral("Child folders must be a YAML sequence named 'folder'.");
            return false;
        }
        if (children) {
            for (const auto &child : children) {
                if (!validateFolder(child, depth + 1))
                    return false;
            }
        }
        return true;
    };

    for (const auto &folder : folders) {
        if (!validateFolder(folder, startingDepth))
            return false;
    }
    if (!validateSessions(rootSessions))
        return false;

    const auto addSessions = [this](const YAML::Node &sessions, QTreeWidgetItem *destination) {
        if (!sessions)
            return;
        for (const auto &value : sessions) {
            SessionDefinition session;
            session.id = QString::fromStdString(value["id"].as<std::string>(""));
            if (session.id.isEmpty() || m_sessions.contains(session.id))
                session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            session.name = QString::fromStdString(value["name"].as<std::string>());
            session.protocol = QString::fromStdString(value["protocol"].as<std::string>());
            session.host = QString::fromStdString(value["host"].as<std::string>());
            session.port = static_cast<quint16>(value["port"].as<int>(0));
            session.username = QString::fromStdString(value["username"].as<std::string>(""));
            session.terminal = terminalAppearanceFromYaml(value["terminal"]);
            session.rdp = rdpOptionsFromYaml(value["rdp"]);
            session.vnc = vncOptionsFromYaml(value["vnc"]);
            session.ssh = sshOptionsFromYaml(value["ssh"]);
            session.serial = serialOptionsFromYaml(value["serial"]);
            session.ftp = ftpOptionsFromYaml(value["ftp"]);
            session.rsh = rshOptionsFromYaml(value["rsh"]);
            session.telnet = telnetOptionsFromYaml(value["telnet"]);
            session.xdmcp = xdmcpOptionsFromYaml(value["xdmcp"]);
            session.automation = automationFromYaml(value["automation"]);
            session.local = localSessionOptionsFromYaml(value["local"]);
            session.protocol = currentProtocolName(session.protocol);
            // Before the administrator switch, its own session type.
            if (session.protocol == QStringLiteral("Local (Administrator)")) {
                session.protocol = QStringLiteral("Local");
                session.local.administrator = true;
            }
            session.jumpHost = QString::fromStdString(value["jump_host"].as<std::string>(""));
            session.forwards = portForwardsFromYaml(value["forwards"]);
            session.proxy = QString::fromStdString(value["proxy"].as<std::string>(""));
            session.wakeOnLan = wakeOnLanFromYaml(value["wake_on_lan"]);
            session.pinned = value["pinned"].as<bool>(false);
            session.passwordStore = QString::fromStdString(value["password_store"].as<std::string>(""));
            // Older files kept a VNC session's SSH tunnel with its VNC options.
            if (session.jumpHost.isEmpty())
                session.jumpHost = session.vnc.sshTunnel;
            session.vnc.sshTunnel.clear();
            addSession(session, destination);
        }
    };
    std::function<void(const YAML::Node &, QTreeWidgetItem *)> addFolder;
    addFolder = [this, &addFolder, &addSessions](const YAML::Node &node,
                                                 QTreeWidgetItem *destination) {
        const auto name = QString::fromStdString(node["name"].as<std::string>());
        auto *folder = destination ? new QTreeWidgetItem(destination, {name})
                                   : new QTreeWidgetItem(m_sessionTree, {name});
        setItemKind(folder, ItemKind::Folder);
        folder->setData(0, FolderJumpHostRole,
                        QString::fromStdString(node["jump_host"].as<std::string>("")));
        folder->setData(0, FolderProxyRole,
                        QString::fromStdString(node["proxy"].as<std::string>("")));
        if (node["kerberos"] && node["kerberos"].IsScalar())
            folder->setData(0, FolderKerberosRole, node["kerberos"].as<bool>(false) ? 1 : 0);
        folder->setData(0, FolderAutomationRole,
                        automationToText(automationFromYaml(node["automation"])));
        addSessions(node["sessions"], folder);
        const auto children = node["folder"];
        if (children) {
            for (const auto &child : children)
                addFolder(child, folder);
        }
        folder->setExpanded(true);
    };

    for (const auto &folder : folders)
        addFolder(folder, parent);
    // Sessions outside any folder (or, when importing, into the chosen one).
    addSessions(rootSessions, parent);
    return true;
}

void MainWindow::importSessions()
{
    const auto filePath = FileDialogs::openFileName(
        this, QStringLiteral("Import YAML Sessions"), QString(),
        QStringLiteral("YAML files (*.yaml *.yml);;All files (*)"));
    if (filePath.isEmpty())
        return;

    try {
        const auto document = YAML::LoadFile(filePath.toStdString());
        const auto version = document["version"].as<int>(1);
        QString error;
        const bool imported = version == 2
            ? appendCompactSessionYaml(document["folder"], selectedFolder(), &error,
                                       document["sessions"])
            : appendSessionYaml(document["items"], selectedFolder(), &error);
        if (!imported) {
            QMessageBox::warning(this, QStringLiteral("Could not import sessions"), error);
            return;
        }
        saveSessions();
    } catch (const YAML::Exception &error) {
        QMessageBox::warning(this, QStringLiteral("Invalid YAML file"),
                             QString::fromLocal8Bit(error.what()));
    }
}

// Puts imported sessions in a new top-level folder "Imported from <source>",
// keeping their folders up to the nesting limit (deeper ones are joined).
void MainWindow::importResult(const QString &source, const SessionImport::Result &result)
{
    if (!result.error.isEmpty() && result.sessions.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Import from %1").arg(source), result.error);
        return;
    }
    if (result.sessions.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Import from %1").arg(source),
                                 QStringLiteral("Nothing to import: %1 entries use protocols "
                                                "OpsRemote does not have.").arg(result.skipped));
        return;
    }
    auto *top = addFolder(QStringLiteral("Imported from %1").arg(source), nullptr);
    QHash<QString, QTreeWidgetItem *> folders;
    for (const auto &imported : result.sessions) {
        QStringList path = imported.folders;
        if (path.size() > MaximumFolderDepth - 1) {
            const QString deepest = path.mid(MaximumFolderDepth - 2).join(QStringLiteral(" / "));
            path = path.mid(0, MaximumFolderDepth - 2) << deepest;
        }
        QTreeWidgetItem *parent = top;
        QString key;
        for (const auto &name : std::as_const(path)) {
            key += QLatin1Char('/') + name;
            if (!folders.contains(key))
                folders.insert(key, addFolder(name, parent));
            parent = folders.value(key);
        }
        addSession(imported.session, parent);
    }
    saveSessions();
    QString message = QStringLiteral("Imported %1 session(s) from %2 into \"%3\".")
                          .arg(result.sessions.size())
                          .arg(source, top->text(0));
    if (result.skipped > 0)
        message += QStringLiteral(" %1 entries were left out (patterns, or protocols OpsRemote "
                                  "does not have).").arg(result.skipped);
    message += QStringLiteral(" Passwords are not imported; OpsRemote asks for them and can keep "
                              "them in its vault.");
    QMessageBox::information(this, QStringLiteral("Import from %1").arg(source), message);
}

void MainWindow::exportSessions()
{
    const auto filePath = FileDialogs::saveFileName(
        this, QStringLiteral("Export YAML Sessions"), QStringLiteral("sessions.yaml"),
        QStringLiteral("YAML files (*.yaml *.yml)"));
    if (filePath.isEmpty())
        return;

    // The selected folder (or the one holding the selected session), or everything.
    YAML::Node document;
    const auto *selected = selectedItem();
    const auto *folder = selected && isFolder(selected) ? selected
                         : selected                     ? selected->parent()
                                                        : nullptr;
    if (folder) {
        YAML::Node folders(YAML::NodeType::Sequence);
        folders.push_back(folderToYaml(folder, m_sessions));
        document["version"] = 2;
        document["folder"] = folders;
    } else {
        document = sessionsDocument(m_sessionTree->invisibleRootItem());
    }
    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << document;

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Could not export sessions"), file.errorString());
        return;
    }
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Could not export sessions"), file.errorString());
        return;
    }
    QMessageBox::information(this, QStringLiteral("Sessions exported"),
                             QStringLiteral("Session configuration was exported. Passwords are never included in YAML; they remain in the encrypted credential vault."));
}

bool MainWindow::ensureVaultUnlocked()
{
    if (m_vault.isUnlocked())
        return true;

    bool accepted = false;
    if (m_vault.exists()) {
        constexpr int MaximumUnlockAttempts = 3;
        QString error;
        for (int attempt = 1; attempt <= MaximumUnlockAttempts; ++attempt) {
            const auto label = error.isEmpty()
                ? QStringLiteral("Master password:")
                : QStringLiteral("%1\nMaster password (attempt %2 of %3):")
                      .arg(error).arg(attempt).arg(MaximumUnlockAttempts);
            auto masterPassword = QInputDialog::getText(
                this, QStringLiteral("Unlock Credential Vault"), label, QLineEdit::Password,
                QString(), &accepted);
            if (!accepted)
                return false;
            const bool unlocked = m_vault.unlock(masterPassword, &error);
            masterPassword.fill(QChar(0));
            if (unlocked) {
                return true;
            }
        }
        QMessageBox::warning(this, QStringLiteral("Could not unlock vault"), error);
        return false;
    }

    MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Create, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    auto masterPassword = dialog.newPassword();
    QString error;
    const bool created = m_vault.initialize(masterPassword, &error);
    masterPassword.fill(QChar(0));
    if (!created) {
        QMessageBox::warning(this, QStringLiteral("Could not create vault"), error);
        return false;
    }
    return true;
}

// The sessions.yaml document for the whole tree: top-level folders under
// "folder", sessions outside any folder under "sessions".
YAML::Node MainWindow::sessionsDocument(const QTreeWidgetItem *root) const
{
    YAML::Node folders(YAML::NodeType::Sequence);
    YAML::Node sessions(YAML::NodeType::Sequence);
    for (int index = 0; index < root->childCount(); ++index) {
        const auto *item = root->child(index);
        if (isFolder(item)) {
            folders.push_back(folderToYaml(item, m_sessions));
            continue;
        }
        const auto session = m_sessions.constFind(item->data(0, SessionIdRole).toString());
        if (session != m_sessions.cend())
            sessions.push_back(sessionToYaml(*session));
    }
    YAML::Node document;
    document["version"] = 2;
    document["folder"] = folders;
    if (sessions.size() > 0)
        document["sessions"] = sessions;
    return document;
}

void MainWindow::saveSessions()
{
    const auto dataDirectory = stateDirectory();
    if (!QDir().mkpath(dataDirectory)) {
        QMessageBox::warning(this, QStringLiteral("Could not save sessions"),
                             QStringLiteral("Could not create the settings folder next to the application."));
        return;
    }

    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << sessionsDocument(m_sessionTree->invisibleRootItem());

    QSaveFile file(QDir(dataDirectory).filePath(QStringLiteral("sessions.yaml")));
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Could not save sessions"), file.errorString());
        return;
    }
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Could not save sessions"), file.errorString());
    }
    refreshSessionList();
}

// The selected item; a copy in Pinned or Recent stands for its session,
// and Pinned and Recent themselves are not something to act on.
QTreeWidgetItem *MainWindow::selectedItem() const
{
    auto *item = m_sessionTree->currentItem();
    if (!item)
        return nullptr;
    const auto kind = item->data(0, ItemKindRole).toString();
    if (kind == QStringLiteral("quick"))
        return nullptr;
    if (kind == QStringLiteral("clone"))
        return realSessionItem(item->data(0, SessionIdRole).toString());
    return item;
}

bool MainWindow::isQuickItem(const QTreeWidgetItem *item) const
{
    const auto kind = item ? item->data(0, ItemKindRole).toString() : QString();
    return kind == QStringLiteral("quick") || kind == QStringLiteral("clone");
}

QTreeWidgetItem *MainWindow::realSessionItem(const QString &id) const
{
    for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
        if ((*it)->data(0, ItemKindRole).toString() == QStringLiteral("session")
            && (*it)->data(0, SessionIdRole).toString() == id)
            return *it;
    }
    return nullptr;
}

QTreeWidgetItem *MainWindow::selectedFolder() const
{
    auto *item = selectedItem();
    if (!item)
        return nullptr;
    if (isFolder(item))
        return item;
    return item->parent();
}

int MainWindow::folderDepth(const QTreeWidgetItem *folder) const
{
    int depth = 0;
    for (auto *item = folder; item; item = item->parent()) {
        if (isFolder(item))
            ++depth;
    }
    return depth;
}

void MainWindow::createFolder()
{
    auto *parent = selectedFolder();
    const int newDepth = parent ? folderDepth(parent) + 1 : 1;
    if (newDepth > MaximumFolderDepth) {
        QMessageBox::information(this, QStringLiteral("Folder depth limit"),
                                 QStringLiteral("Folders can be nested up to %1 levels.").arg(MaximumFolderDepth));
        return;
    }

    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("New Folder"),
                                            QStringLiteral("Folder name:"), QLineEdit::Normal,
                                            QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;

    addFolder(name, parent);
    saveSessions();
}

// A folder's settings, in tabs: the jump host and proxy its sessions inherit.
void MainWindow::editFolderSettings(QTreeWidgetItem *folder)
{
    if (!folder || !isFolder(folder))
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Folder Settings: %1").arg(folder->text(0)));
    dialog.setMinimumWidth(480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    auto *jumpHost = new JumpHostEditor(folder->data(0, FolderJumpHostRole).toString(),
                                        jumpHostLabel(inheritedJumpHost(folder->parent())),
                                        jumpHostChoices(), QString(), tabs);
    jumpHost->offerNew([this](QWidget *over) { return createSshSession(over); },
                       [this] { return jumpHostChoices(); });
    tabs->addTab(jumpHost, QStringLiteral("Jump host"));
    auto *proxy = new ProxyEditor(folder->data(0, FolderProxyRole).toString(), QString(),
                                  ProxySettings::label(inheritedProxy(folder->parent())), tabs);
    tabs->addTab(proxy, QStringLiteral("Proxy"));
    auto *kerberosPage = new QWidget(tabs);
    auto *kerberosForm = new QFormLayout(kerberosPage);
    auto *kerberos = new QComboBox(kerberosPage);
    // The inherited choice first, then only the other one (inheritance.h).
    const bool kerberosInherited = inheritedKerberos(folder->parent());
    addInheritedItem(kerberos, kerberosInherited ? QStringLiteral("On") : QStringLiteral("Off"), QVariant());
    if (kerberosInherited)
        kerberos->addItem(QStringLiteral("Off"), 0);
    else
        kerberos->addItem(QStringLiteral("On"), 1);
    kerberos->setCurrentIndex(qMax(0, kerberos->findData(folder->data(0, FolderKerberosRole))));
    kerberosForm->addRow(QStringLiteral("Log on with Kerberos:"), kerberos);
    applyInheritance(kerberosPage);
    auto *kerberosNote = new QLabel(QStringLiteral("SSH sessions in this folder use your Kerberos "
                                                   "ticket (GSSAPI) unless they choose otherwise."),
                                    kerberosPage);
    kerberosNote->setWordWrap(true);
    kerberosForm->addRow(kerberosNote);
    kerberosForm->addRow(new AvailabilityNote(&kerberosProblem, [kerberos](bool available) {
        kerberos->setEnabled(available || kerberos->currentData().isValid());
    }, kerberosPage, AvailabilityNote::Run::Background));
    tabs->addTab(kerberosPage, QStringLiteral("Kerberos"));
    auto *automation = new AutomationEditor(
        automationFromText(folder->data(0, FolderAutomationRole).toString()),
        QStringLiteral("The parent folder's (%1)").arg(inheritedAutomation(folder->parent()).mode
                                                           == QStringLiteral("own")
                                                       ? QStringLiteral("set")
                                                       : QStringLiteral("none")),
        savedScriptNames(), tabs);
    tabs->addTab(automation, QStringLiteral("Automation"));
    layout->addWidget(tabs);
    auto *note = new QLabel(QStringLiteral("Sessions in this folder and its subfolders use these "
                                           "settings unless they choose their own."), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    // Save waits while the proxy lacks its host or port.
    auto *proxyGuard = new SaveGuard(buttons->button(QDialogButtonBox::Save), &dialog);
    proxyGuard->addCheck([proxy] { return proxy->problem(); });
    connect(proxy, &ProxyEditor::changed, proxyGuard, &SaveGuard::recheck);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    folder->setData(0, FolderJumpHostRole, jumpHost->value());
    folder->setData(0, FolderProxyRole, proxy->value());
    folder->setData(0, FolderKerberosRole, kerberos->currentData());
    folder->setData(0, FolderAutomationRole, automationToText(automation->value()));
    saveProxyPassword(proxy->value(), proxy->password());
    saveSessions();
}

// Del: the selected folder (with its contents) or session.
void MainWindow::deleteSelectedItem()
{
    auto *item = selectedItem();
    if (!item)
        return;
    if (isFolder(item)) {
        deleteSelectedFolder();
        return;
    }
    const auto id = item->data(0, SessionIdRole).toString();
    const auto session = m_sessions.constFind(id);
    if (session == m_sessions.cend())
        return;
    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("Delete Session"));
    confirmation.setText(QStringLiteral("Delete the session \"%1\"?").arg(session->name));
    auto *deleteButton = confirmation.addButton(QStringLiteral("Delete Session"),
                                                QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != deleteButton)
        return;

    const QString name = session->name;
    for (int index = m_sessionTabs->count() - 1; index >= 0; --index) {
        auto *page = m_sessionTabs->widget(index);
        if (page && page->property("sessionId").toString() == id)
            closeSessionTab(index);
    }
    m_sessions.remove(id);
    if (auto *parent = item->parent())
        delete parent->takeChild(parent->indexOfChild(item));
    else
        delete m_sessionTree->takeTopLevelItem(m_sessionTree->indexOfTopLevelItem(item));
    saveSessions();
}

void MainWindow::deleteSelectedFolder()
{
    auto *folder = selectedItem();
    if (!folder || !isFolder(folder)) {
        QMessageBox::information(this, QStringLiteral("Delete Folder"),
                                 QStringLiteral("Select a folder first."));
        return;
    }

    int folderCount = 0;
    int sessionCount = 0;
    QSet<QString> sessionIds;
    std::function<void(QTreeWidgetItem *)> inspect = [this, &folderCount, &sessionCount,
                                                       &sessionIds, &inspect](QTreeWidgetItem *item) {
        if (isFolder(item))
            ++folderCount;
        else {
            ++sessionCount;
            sessionIds.insert(item->data(0, SessionIdRole).toString());
        }
        for (int index = 0; index < item->childCount(); ++index)
            inspect(item->child(index));
    };
    inspect(folder);

    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("Delete Folder"));
    confirmation.setText(QStringLiteral("Delete \"%1\" and its contents?").arg(folder->text(0)));
    confirmation.setInformativeText(
        QStringLiteral("This removes %1 folder(s) and %2 saved session(s) from the session configuration.")
            .arg(folderCount).arg(sessionCount));
    auto *deleteButton = confirmation.addButton(QStringLiteral("Delete Folder"),
                                                QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != deleteButton)
        return;

    const auto folderName = folder->text(0);

    for (int index = m_sessionTabs->count() - 1; index >= 0; --index) {
        auto *page = m_sessionTabs->widget(index);
        if (page && sessionIds.contains(page->property("sessionId").toString()))
            closeSessionTab(index);
    }
    for (const auto &sessionId : sessionIds)
        m_sessions.remove(sessionId);

    auto *parent = folder->parent();
    if (parent)
        delete parent->takeChild(parent->indexOfChild(folder));
    else
        delete m_sessionTree->takeTopLevelItem(m_sessionTree->indexOfTopLevelItem(folder));
    saveSessions();
    saveFolderExpansion();
}

// Renames a session or folder (F2 in the session list).
void MainWindow::renameTreeItem(QTreeWidgetItem *item, const QString &newName)
{
    const QString name = newName.trimmed();
    if (name.isEmpty())
        return;
    if (isFolder(item)) {
        if (item->text(0) == name)
            return;
        item->setText(0, name);
        saveSessions();
        return;
    }
    const auto id = item->data(0, SessionIdRole).toString();
    auto session = m_sessions.find(id);
    if (session == m_sessions.end())
        return;
    if (session->name == name) {
        item->setText(0, sessionLabel(*session));
        return;
    }
    session->name = name;
    item->setText(0, sessionLabel(*session));
    // Open tabs titled by session name follow.
    if (m_startup.tabTitle != QStringLiteral("userhost")) {
        for (int index = 0; index < m_sessionTabs->count(); ++index) {
            if (m_sessionTabs->widget(index)->property("sessionId").toString() != id)
                continue;
            auto text = m_sessionTabs->tabText(index);
            // "Name (user@host)": only the name part changes.
            const auto open = text.lastIndexOf(QStringLiteral(" ("));
            m_sessionTabs->setTabText(index, m_startup.tabTitle == QStringLiteral("both") && open > 0
                                                 ? name + text.mid(open)
                                                 : name);
        }
    }
    saveSessions();
}

// A copy of the session right below it, named "Name (1)", "Name (2)", ...
void MainWindow::duplicateSession(QTreeWidgetItem *item)
{
    if (!item || isFolder(item))
        return;
    const auto original = m_sessions.constFind(item->data(0, SessionIdRole).toString());
    if (original == m_sessions.cend())
        return;
    QString base = original->name;
    static const QRegularExpression numbered(QStringLiteral("^(.*) \\((\\d+)\\)$"));
    if (const auto match = numbered.match(base); match.hasMatch())
        base = match.captured(1);
    QSet<QString> names;
    for (const auto &session : std::as_const(m_sessions))
        names.insert(session.name);
    int number = 1;
    while (names.contains(QStringLiteral("%1 (%2)").arg(base).arg(number)))
        ++number;

    SessionDefinition copy = *original;
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy.name = QStringLiteral("%1 (%2)").arg(base).arg(number);
    // The saved password comes along when the vault is open.
    if (m_vault.isUnlocked()) {
        QString password = m_vault.passwordForSession(original->id);
        if (!password.isEmpty() && !m_vault.setPassword(copy.id, password))
            notify(QStringLiteral("Duplicated as %1, but its password could not be copied.").arg(copy.name), true);
        password.fill(QChar(0));
    }
    auto *parent = item->parent();
    addSession(copy, parent);
    // addSession() appends; place the copy after the original and the
    // copies already below it, so they stay in order.
    auto *added = m_sessionTree->currentItem();
    const auto childAt = [this, parent](int index) {
        return parent ? parent->child(index) : m_sessionTree->topLevelItem(index);
    };
    const auto indexOf = [this, parent](QTreeWidgetItem *child) {
        return parent ? parent->indexOfChild(child) : m_sessionTree->indexOfTopLevelItem(child);
    };
    if (parent)
        parent->removeChild(added);
    else
        m_sessionTree->takeTopLevelItem(indexOf(added));
    int position = indexOf(item) + 1;
    const QString prefix = base + QStringLiteral(" (");
    while (auto *next = childAt(position)) {
        if (isFolder(next)
            || !m_sessions.value(next->data(0, SessionIdRole).toString()).name.startsWith(prefix))
            break;
        ++position;
    }
    if (parent)
        parent->insertChild(position, added);
    else
        m_sessionTree->insertTopLevelItem(position, added);
    m_sessionTree->setCurrentItem(added);
    saveSessions();
    if (m_vault.exists() && !m_vault.isUnlocked())
        notify(QStringLiteral("Duplicated as %1 (the vault is locked, so its password was not copied)").arg(copy.name));
}

void MainWindow::showTreeContextMenu(const QPoint &position)
{
    auto *clickedItem = m_sessionTree->itemAt(position);
    // Pinned and Recent have no menu; their copies act on the session.
    if (clickedItem && clickedItem->data(0, ItemKindRole).toString() == QStringLiteral("quick"))
        return;
    if (clickedItem && clickedItem->data(0, ItemKindRole).toString() == QStringLiteral("clone"))
        clickedItem = realSessionItem(clickedItem->data(0, SessionIdRole).toString());
    // An empty area adds at the top level.
    m_sessionTree->setCurrentItem(clickedItem);
    if (!clickedItem)
        m_sessionTree->clearSelection();

    QMenu menu(this);
    const auto addNew = [this, &menu] {
        menu.addAction(QStringLiteral("New Folder…"), this, &MainWindow::createFolder);
        menu.addAction(QStringLiteral("New Session…"), this, &MainWindow::createSession);
    };
    const auto addRename = [this, &menu, clickedItem] {
        auto *rename = menu.addAction(QStringLiteral("Rename"), this, [this, clickedItem] {
            m_sessionTree->editItem(clickedItem, 0);
        });
        rename->setShortcut(QKeySequence(Qt::Key_F2));
    };
    if (!clickedItem) {
        addNew();
    } else if (isFolder(clickedItem)) {
        addNew();
        menu.addSeparator();
        menu.addAction(QStringLiteral("Folder Settings…"), this, [this, clickedItem] {
            editFolderSettings(clickedItem);
        });
        menu.addAction(QStringLiteral("Run Command on Hosts…"), this, [this, clickedItem] {
            showRunCommand(clickedItem);
        });
        if (loadScriptPreferences().enabled) {
            auto *scripts = menu.addMenu(QStringLiteral("Run Script on Hosts"));
            const auto saved = loadSavedScripts();
            for (const auto &script : saved)
                scripts->addAction(script.group.isEmpty() ? script.name
                                                          : QStringLiteral("%1 › %2").arg(script.group, script.name),
                                   this, [this, clickedItem, name = script.name] {
                                       showRunCommand(clickedItem, scriptMarker(name));
                                   });
            if (saved.isEmpty())
                scripts->addAction(QStringLiteral("(none yet)"))->setEnabled(false);
            scripts->addSeparator();
            scripts->addAction(QStringLiteral("Edit Scripts…"), this, [this] { showScripts(); });
        }
        addRename();
        auto *remove = menu.addAction(QStringLiteral("Delete Folder…"), this,
                                      &MainWindow::deleteSelectedItem);
        remove->setShortcut(m_shortcuts.value(QStringLiteral("delete_folder")));
    } else {
        menu.addAction(QStringLiteral("Open"), this, &MainWindow::openSelectedSession);
        menu.addAction(QStringLiteral("Edit…"), this, &MainWindow::editSelectedSession);
        addRename();
        menu.addAction(QStringLiteral("Duplicate"), this, [this, clickedItem] {
            duplicateSession(clickedItem);
        });
        const auto clickedId = clickedItem->data(0, SessionIdRole).toString();
        if (m_sessions.value(clickedId).protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0
            && (loadContainerPreferences().docker.enabled || loadContainerPreferences().kubernetes.enabled))
            menu.addAction(QStringLiteral("Open a Container…"), this, [this, clickedId] { openContainerOn(clickedId); });
        const QString pinId = clickedItem->data(0, SessionIdRole).toString();
        menu.addAction(m_sessions.value(pinId).pinned ? QStringLiteral("Unpin") : QStringLiteral("Pin"),
                       this, [this, pinId] {
                           auto session = m_sessions.find(pinId);
                           if (session == m_sessions.end())
                               return;
                           session->pinned = !session->pinned;
                           saveSessions();
                       });
        const auto session = m_sessions.constFind(clickedItem->data(0, SessionIdRole).toString());
        if (session != m_sessions.cend() && session->wakeOnLan.isSet()) {
            const auto wake = session->wakeOnLan;
            const QString name = session->name;
            menu.addAction(QStringLiteral("Wake Up"), this, [this, wake, name] {
                QString error;
                if (!wake.send(&error))
                    QMessageBox::warning(this, QStringLiteral("Wake-on-LAN"), error);
            });
        }
        auto *remove = menu.addAction(QStringLiteral("Delete Session…"), this,
                                      &MainWindow::deleteSelectedItem);
        remove->setShortcut(m_shortcuts.value(QStringLiteral("delete_folder")));
    }
    menu.exec(m_sessionTree->viewport()->mapToGlobal(position));
}

QString MainWindow::folderStateKey(const QTreeWidgetItem *folder) const
{
    QStringList segments;
    for (auto *item = folder; item; item = item->parent()) {
        const auto *parent = item->parent();
        const int index = parent ? parent->indexOfChild(const_cast<QTreeWidgetItem *>(item))
                                 : m_sessionTree->indexOfTopLevelItem(
                                       const_cast<QTreeWidgetItem *>(item));
        segments.prepend(QString::number(index));
    }
    return segments.join(QLatin1Char('/'));
}

void MainWindow::restoreFolderExpansion()
{
    // A folder with no record (new, or settings from an older version) is
    // collapsed.
    const auto states = loadFolderExpansionStates();
    std::function<void(QTreeWidgetItem *)> restore = [this, &states, &restore](QTreeWidgetItem *item) {
        if (isFolder(item))
            item->setExpanded(states.value(folderStateKey(item), false));
        for (int index = 0; index < item->childCount(); ++index)
            restore(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        restore(m_sessionTree->topLevelItem(index));
}

void MainWindow::saveFolderExpansion() const
{
    QHash<QString, bool> states;
    std::function<void(const QTreeWidgetItem *)> collect = [this, &states, &collect](
                                                             const QTreeWidgetItem *item) {
        if (isFolder(item))
            states.insert(folderStateKey(item), item->isExpanded());
        for (int index = 0; index < item->childCount(); ++index)
            collect(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        collect(m_sessionTree->topLevelItem(index));
    saveFolderExpansionStates(states);
}

QTreeWidgetItem *MainWindow::addFolder(const QString &name, QTreeWidgetItem *parent)
{
    auto *folder = parent ? new QTreeWidgetItem(parent, {name})
                          : new QTreeWidgetItem(m_sessionTree, {name});
    setItemKind(folder, ItemKind::Folder);
    folder->setExpanded(true);
    m_sessionTree->setCurrentItem(folder);
    return folder;
}

void MainWindow::addSession(const SessionDefinition &session, QTreeWidgetItem *parent)
{
    auto *item = parent ? new QTreeWidgetItem(parent, {sessionLabel(session)})
                        : new QTreeWidgetItem(m_sessionTree, {sessionLabel(session)});
    setItemKind(item, ItemKind::Session);
    item->setData(0, SessionIdRole, session.id);
    item->setToolTip(0, QStringLiteral("%1 — %2:%3")
                            .arg(session.protocol, session.host,
                                 session.port == 0 ? QStringLiteral("default")
                                                   : QString::number(session.port)));
    m_sessions.insert(session.id, session);
    if (parent)
        parent->setExpanded(true);
    m_sessionTree->setCurrentItem(item);
}

void MainWindow::setItemKind(QTreeWidgetItem *item, ItemKind kind)
{
    item->setData(0, ItemKindRole, kind == ItemKind::Folder ? QStringLiteral("folder")
                                                           : QStringLiteral("session"));
    // Both can be renamed with F2.
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    if (kind == ItemKind::Folder) {
        item->setIcon(0, themedIcon(QStringLiteral("folder"), QStyle::SP_DirIcon));
    } else {
        item->setIcon(0, themedIcon(QStringLiteral("network-server"), QStyle::SP_ComputerIcon));
        // Sessions can be dragged, but nothing can be dropped onto them.
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
    }
}

bool MainWindow::isFolder(const QTreeWidgetItem *item) const
{
    return item && item->data(0, ItemKindRole).toString() == QStringLiteral("folder");
}

void MainWindow::recordRecentSession(const QString &id)
{
    if (!m_sessions.contains(id))
        return; // quick-connect targets are not saved sessions
    m_recentSessions.removeAll(id);
    m_recentSessions.prepend(id);
    m_recentSessions = m_recentSessions.mid(0, 20);
    saveRecentSessions(m_recentSessions);
    refreshSessionList();
}

void MainWindow::setSessionListPreferences(const SessionListPreferences &preferences)
{
    m_sessionList = preferences;
    saveSessionListPreferences(preferences);
    applySessionListPreferences();
}

void MainWindow::applySessionListPreferences()
{
    m_sessionTree->setIndentation(m_sessionList.indentation);
    m_sessionTree->setColumnHidden(HostColumn, !m_sessionList.showHost);
    m_sessionTree->setColumnHidden(ProtocolColumn, !m_sessionList.showProtocol);
    m_sessionTree->setColumnHidden(SummaryColumn, summaryColumn() != SummaryColumn);
    m_sessionSearch->setVisible(m_sessionList.showSearch || !m_sessionSearch->text().isEmpty());
    // The header names the columns only when there are some.
    m_sessionTree->setHeaderHidden(false);
    if (m_sessionList.checkStatus) {
        m_statusTimer->start(m_sessionList.checkSeconds * 1000);
        QTimer::singleShot(0, this, &MainWindow::checkHostStatus);
    } else {
        m_statusTimer->stop();
        m_hostStatus->clear();
    }
    refreshSessionList();
}

// Where folder summaries go: -1 nowhere, 0 after the name, otherwise the
// first column after the name (its own when no other column is shown).
int MainWindow::summaryColumn() const
{
    if (m_sessionList.folderSummary == QStringLiteral("off"))
        return -1;
    if (m_sessionList.folderSummary != QStringLiteral("column"))
        return 0;
    if (m_sessionList.showHost)
        return HostColumn;
    if (m_sessionList.showProtocol)
        return ProtocolColumn;
    return SummaryColumn;
}

// How many sessions shown under a folder answer and how many do not.
QPair<int, int> MainWindow::folderStatusCounts(const QTreeWidgetItem *folder) const
{
    QPair<int, int> counts{0, 0};
    for (int index = 0; index < folder->childCount(); ++index) {
        const auto *child = folder->child(index);
        if (child->isHidden())
            continue; // left out by a search
        if (isFolder(child)) {
            const auto inner = folderStatusCounts(child);
            counts.first += inner.first;
            counts.second += inner.second;
            continue;
        }
        const auto status = m_hostStatus->status(child->data(0, SessionIdRole).toString());
        if (status == HostStatusChecker::Status::Up)
            ++counts.first;
        else if (status == HostStatusChecker::Status::Down)
            ++counts.second;
    }
    return counts;
}

// Rebuilds Pinned and Recent at the top, fills the columns, and fits the panel.
void MainWindow::refreshSessionList()
{
    if (!m_sessionTree)
        return;
    // Pinned and Recent look like other folders, with a star or a clock.
    const auto rebuild = [this](QTreeWidgetItem *&folder, bool show, const QString &name,
                                bool star, const QStringList &ids, int position) {
        const bool expanded = !folder || folder->isExpanded();
        delete folder;
        folder = nullptr;
        if (!show || ids.isEmpty())
            return;
        folder = new QTreeWidgetItem({name});
        m_sessionTree->insertTopLevelItem(position, folder);
        folder->setData(0, ItemKindRole, QStringLiteral("quick"));
        folder->setIcon(0, badgedFolderIcon(themedIcon(QStringLiteral("folder"), QStyle::SP_DirIcon),
                                            star));
        folder->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        for (const auto &id : ids) {
            const auto session = m_sessions.constFind(id);
            if (session == m_sessions.cend())
                continue;
            auto *copy = new QTreeWidgetItem(folder, {sessionLabel(*session)});
            copy->setData(0, ItemKindRole, QStringLiteral("clone"));
            copy->setData(0, SessionIdRole, id);
            copy->setIcon(0, themedIcon(QStringLiteral("network-server"), QStyle::SP_ComputerIcon));
            copy->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        folder->setExpanded(expanded);
    };
    QStringList pinned;
    for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
        if ((*it)->data(0, ItemKindRole).toString() == QStringLiteral("session")) {
            const auto id = (*it)->data(0, SessionIdRole).toString();
            if (m_sessions.value(id).pinned)
                pinned.append(id);
        }
    }
    QStringList recent;
    for (const auto &id : std::as_const(m_recentSessions)) {
        if (m_sessions.contains(id) && recent.size() < m_sessionList.recentCount)
            recent.append(id);
    }
    rebuild(m_pinnedFolder, m_sessionList.showPinned, QStringLiteral("Pinned"), true, pinned, 0);
    rebuild(m_recentFolder, m_sessionList.showRecent, QStringLiteral("Recent"), false, recent,
            m_pinnedFolder ? 1 : 0);

    // Columns: host (with a port that is not the protocol's usual one) and protocol.
    for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
        auto *item = *it;
        if (isFolder(item) || item->data(0, ItemKindRole).toString() == QStringLiteral("quick"))
            continue;
        const auto session = m_sessions.constFind(item->data(0, SessionIdRole).toString());
        if (session == m_sessions.cend())
            continue;
        static const QHash<QString, int> usualPorts{{QStringLiteral("SSH"), 22},
                                                    {QStringLiteral("MOSH"), 22},
                                                    {QStringLiteral("TELNET"), 23},
                                                    {QStringLiteral("RDP"), 3389},
                                                    {QStringLiteral("VNC"), 5900},
                                                    {QStringLiteral("FTP"), 21},
                                                    {QStringLiteral("SFTP"), 22},
                                                    {QStringLiteral("SCP"), 22},
                                                    {QStringLiteral("RLOGIN"), 513},
                                                    {QStringLiteral("RSH"), 514},
                                                    {QStringLiteral("XDMCP"), 177}};
        const bool usual = session->port == 0
                           || session->port == usualPorts.value(session->protocol.toUpper(), -1);
        item->setText(HostColumn, usual ? session->host
                                        : QStringLiteral("%1:%2").arg(session->host).arg(session->port));
        item->setText(ProtocolColumn, session->protocol);
    }
    applySessionFilter();
    m_sessionTree->viewport()->update();
    fitSessionPanel();
}

// Search Sessions (Ctrl+Shift+S): the Sessions tab, with the search box ready.
void MainWindow::focusSessionSearch()
{
    if (!m_leftTabs->isVisible())
        m_leftTabs->show();
    m_leftTabs->setCurrentIndex(0);
    m_sessionSearch->show();
    m_sessionSearch->setFocus();
    m_sessionSearch->selectAll();
}

// The first session shown in the list (Pinned and Recent count), or nullptr.
QTreeWidgetItem *MainWindow::firstVisibleSession() const
{
    for (QTreeWidgetItemIterator it(m_sessionTree, QTreeWidgetItemIterator::NotHidden); *it; ++it) {
        const auto kind = (*it)->data(0, ItemKindRole).toString();
        if (kind != QStringLiteral("session") && kind != QStringLiteral("clone"))
            continue;
        // Not inside a folder that was closed by hand.
        bool open = true;
        for (auto *parent = (*it)->parent(); parent && open; parent = parent->parent())
            open = parent->isExpanded();
        if (open)
            return *it;
    }
    return nullptr;
}

// Shows only the sessions matching every word of the search (in their name,
// host, user, protocol, or folder path) and the folders leading to them.
void MainWindow::applySessionFilter()
{
    if (!m_sessionSearch || !m_sessionTree)
        return;
    const QStringList words = m_sessionSearch->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const bool filtering = !words.isEmpty();
    if (filtering && !m_filtering) {
        m_pinnedExpandedBeforeSearch = !m_pinnedFolder || m_pinnedFolder->isExpanded();
        m_recentExpandedBeforeSearch = !m_recentFolder || m_recentFolder->isExpanded();
    }
    const bool ending = !filtering && m_filtering;
    m_filtering = filtering || ending; // expansions below are not remembered
    int shown = 0;
    std::function<bool(QTreeWidgetItem *, const QString &)> apply =
        [&](QTreeWidgetItem *item, const QString &path) -> bool {
        const auto kind = item->data(0, ItemKindRole).toString();
        bool visible = true;
        if (kind == QStringLiteral("folder") || kind == QStringLiteral("quick")) {
            const QString inner = path + item->text(0) + QLatin1Char('/');
            bool any = false;
            for (int index = 0; index < item->childCount(); ++index)
                any = apply(item->child(index), inner) || any;
            visible = !filtering || any;
            if (filtering && any && !item->isExpanded())
                item->setExpanded(true);
        } else if (filtering) {
            const auto session = m_sessions.value(item->data(0, SessionIdRole).toString());
            const QString text = QStringList{path, session.name, session.host, session.username,
                                             session.protocol}
                                     .join(QLatin1Char(' '));
            for (const auto &word : words)
                visible = visible && text.contains(word, Qt::CaseInsensitive);
            shown += visible ? 1 : 0;
        }
        item->setHidden(!visible);
        return visible;
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        apply(m_sessionTree->topLevelItem(index), QString());
    if (ending) {
        // Back to the folders as they were before searching.
        restoreFolderExpansion();
        if (m_pinnedFolder)
            m_pinnedFolder->setExpanded(m_pinnedExpandedBeforeSearch);
        if (m_recentFolder)
            m_recentFolder->setExpanded(m_recentExpandedBeforeSearch);
        if (auto *current = m_sessionTree->currentItem())
            m_sessionTree->scrollToItem(current);
    }
    m_filtering = filtering;
    m_noSessionMatches->setVisible(filtering && shown == 0);
}

// The side panel's tabs and search box start where the session list's box
// does: inside its frame, as wide as the style draws it. Never further left.
void MainWindow::alignSidePanelRows()
{
    m_sidePanelInset = m_sessionTree->frameWidth();
    m_sessionSearchRow->setContentsMargins(m_sidePanelInset, 0, m_sidePanelInset, 0);
    // The tab widget places its tab bar again on its next layout.
    QEvent request(QEvent::LayoutRequest);
    QApplication::sendEvent(m_leftTabs, &request);
}

// Grows or shrinks the side panel to show the list without cutting it off.
void MainWindow::fitSessionPanel()
{
    if (!m_leftTabs || !m_leftTabs->isVisible())
        return;
    // The columns always fit their contents, so names are not cut off; the
    // panel follows only with "Always fit the panel to the list".
    const int columns = fitSessionColumns();
    if (!m_sessionList.fitPanel)
        return;
    setSidePanelWidth(sessionListWidth(columns));
}

// Fits the session list's columns to their contents; their width together.
// Measured with the last column not stretched: it fills the panel otherwise,
// and reading that back made every fit a little wider than the one before.
int MainWindow::fitSessionColumns()
{
    int width = 0;
    auto *header = m_sessionTree->header();
    header->setStretchLastSection(false);
    for (int column = 0; column < m_sessionTree->columnCount(); ++column) {
        if (m_sessionTree->isColumnHidden(column))
            continue;
        m_sessionTree->resizeColumnToContents(column);
        width += m_sessionTree->columnWidth(column);
    }
    header->setStretchLastSection(true);
    return width;
}

// The side panel's width for a list whose columns take columnsWidth.
int MainWindow::sessionListWidth(int columnsWidth) const
{
    int width = columnsWidth + 2 * m_sessionTree->frameWidth() + 8;
    if (m_sessionTree->verticalScrollBar()->isVisible())
        width += m_sessionTree->verticalScrollBar()->width();
    return width + m_leftTabs->width() - m_sessionTree->width();
}

// A double-click on the panel's edge: fitted to what it shows now, the
// session list or the Files panel's files. Nothing listed: left as it is.
void MainWindow::fitSidePanelToContents()
{
    if (!m_leftTabs || !m_leftTabs->isVisible())
        return;
    int width = 0;
    if (m_leftTabs->currentWidget() == m_sftpStack) {
        if (auto *panel = qobject_cast<SftpPanel *>(m_sftpStack->currentWidget()); panel && panel->contentsWidth() > 0)
            width = panel->contentsWidth() + m_leftTabs->width() - m_sftpStack->width();
    } else if (m_leftTabs->currentIndex() == 0 && m_sessionTree->topLevelItemCount() > 0) {
        width = sessionListWidth(fitSessionColumns());
    }
    if (width > 0)
        setSidePanelWidth(width);
}

// The side panel is at least as wide as all of Sessions | Files | Commands |
// Scripts, so its tab bar never needs scroll buttons: with "Always fit the
// panel to the list" off, a remembered or dragged width can be narrower than
// a look's tabs.
void MainWindow::updateSidePanelMinimum()
{
    const int minimum = qMax(220, m_sidePanelInset + m_leftTabs->tabBar()->sizeHint().width() + 4);
    if (m_leftTabs->minimumWidth() != minimum)
        m_leftTabs->setMinimumWidth(minimum);
}

void MainWindow::setSidePanelWidth(int width)
{
    auto *splitter = qobject_cast<QSplitter *>(m_leftTabs->parentWidget());
    if (!splitter)
        return;
    // Never narrower than its tabs (updateSidePanelMinimum).
    const int total = splitter->contentsRect().width();
    const int wanted = qBound(m_leftTabs->minimumWidth(), width, qMax(m_leftTabs->minimumWidth(), total / 2));
    auto sizes = splitter->sizes();
    if (sizes.size() == 2 && sizes[0] != wanted && total > 0) {
        sizes[1] = total - wanted - splitter->handleWidth();
        sizes[0] = wanted;
        splitter->setSizes(sizes);
    }
}

// Knocks on every saved session's port. Sessions through a jump host or a
// proxy are not checked: this computer may not reach them directly.
void MainWindow::checkHostStatus()
{
    if (!m_sessionList.checkStatus || m_hostStatus->isChecking())
        return;
    // Snapshot folder inheritance in one tree walk. Widgets stay on the GUI
    // thread; preferences and probes are handled by HostStatusChecker's worker.
    QHash<QString, QPair<QString, QString>> inherited;
    std::function<void(QTreeWidgetItem *, QString, QString)> visit =
        [&](QTreeWidgetItem *item, QString jump, QString proxy) {
            if (isQuickItem(item))
                return;
            if (isFolder(item)) {
                const auto ownJump = item->data(0, FolderJumpHostRole).toString();
                const auto ownProxy = item->data(0, FolderProxyRole).toString();
                if (!ownJump.isEmpty())
                    jump = ownJump;
                if (!ownProxy.isEmpty())
                    proxy = ownProxy;
            } else {
                inherited.insert(item->data(0, SessionIdRole).toString(), {jump, proxy});
            }
            for (int index = 0; index < item->childCount(); ++index)
                visit(item->child(index), jump, proxy);
        };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        visit(m_sessionTree->topLevelItem(index), JumpHost::Direct, QString());
    QList<HostStatusChecker::Target> targets;
    for (const auto &session : std::as_const(m_sessions)) {
        if (session.protocol.compare(QStringLiteral("Serial"), Qt::CaseInsensitive) == 0) {
            targets.append({session.id, session.host, 0, true});
            continue;
        }
        // XDMCP is UDP: a TCP check cannot tell.
        if (session.protocol.compare(QStringLiteral("XDMCP"), Qt::CaseInsensitive) == 0)
            continue;
        const auto folder = inherited.value(session.id, {JumpHost::Direct, QString()});
        auto jump = session.jumpHost.isEmpty() ? folder.first : session.jumpHost;
        if (session.jumpHost.isEmpty() && jump == JumpHost::SessionPrefix + session.id)
            jump = JumpHost::Direct;
        if (!jump.isEmpty() && jump != JumpHost::Direct)
            continue;
        const auto proxy = session.proxy.isEmpty() ? folder.second : session.proxy;
        QString host = session.host;
        quint16 port = session.port;
        const QString protocol = session.protocol.toUpper();
        if (port == 0) {
            if (protocol == QStringLiteral("RDP"))
                port = 3389;
            else if (protocol == QStringLiteral("VNC")) {
                port = 5900;
                // "host:1" is VNC display 1.
                if (host.count(QLatin1Char(':')) == 1) {
                    port = quint16(5900 + host.section(QLatin1Char(':'), 1).toInt());
                    host = host.section(QLatin1Char(':'), 0, 0);
                }
            } else if (protocol == QStringLiteral("TELNET"))
                port = 23;
            else if (protocol == QStringLiteral("RLOGIN"))
                port = 513;
            else if (protocol == QStringLiteral("RSH"))
                port = 514;
            else if (protocol == QStringLiteral("FTP"))
                port = session.ftp.security.isEmpty() ? 0
                       : session.ftp.security == QStringLiteral("implicit") ? 990 : 21;
            else
                port = 22; // SSH, Mosh
        }
        if (!host.isEmpty())
            targets.append({session.id, host, port, false, proxy});
    }
    m_hostStatus->check(targets);
}
