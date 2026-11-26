// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// MainWindow: the Tools menu windows: Network Tools, Run Command, scripts, Kubernetes credentials, keys, tunnels.

#include "mainwindow_p.h"

using namespace mainwindow_p;

// Ping, traceroute and port scan, starting with the selected session's host.
void MainWindow::showNetworkTools()
{
    if (!m_networkTools)
        m_networkTools = new NetworkToolsWindow(this);
    if (auto *item = selectedItem(); item && !isFolder(item)) {
        const auto session = m_sessions.constFind(item->data(0, SessionIdRole).toString());
        if (session != m_sessions.cend())
            m_networkTools->setHost(session->host);
    }
    m_networkTools->show();
    m_networkTools->raise();
    m_networkTools->activateWindow();
}

void MainWindow::showRunCommand(QTreeWidgetItem *folder, const QString &command)
{
    QStringList ids;
    if (folder) {
        std::function<void(QTreeWidgetItem *)> collect = [&](QTreeWidgetItem *parent) {
            for (int i = 0; i < parent->childCount(); ++i) {
                auto *child = parent->child(i);
                if (isFolder(child))
                    collect(child);
                else if (const auto id = child->data(0, SessionIdRole).toString(); m_sessions.contains(id))
                    ids << id;
            }
        };
        collect(folder);
    } else {
        ids = m_sessions.keys();
    }
    QList<RunCommandWindow::Host> hosts;
    int skipped = 0;
    for (const auto &id : std::as_const(ids)) {
        const auto &session = m_sessions[id];
        if (session.protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            ++skipped;
            continue;
        }
        QString address = session.username.isEmpty() ? session.host
                                                     : QStringLiteral("%1@%2").arg(session.username, session.host);
        if (session.port != 0 && session.port != 22)
            address += QStringLiteral(":%1").arg(session.port);
        hosts.append({id, session.name, address});
    }
    const QString name = folder ? folder->text(0) : QStringLiteral("All sessions");
    if (hosts.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Run Command on Hosts"),
                                 QStringLiteral("%1 has no SSH sessions.").arg(name));
        return;
    }
    auto *window = new RunCommandWindow(
        name, hosts, skipped, [this](const QStringList &chosen) { return resolveCommandHosts(chosen); },
        [this] { return m_snippets; }, this);
    if (!command.isEmpty())
        window->setCommand(command);
    window->show();
}

void MainWindow::showScripts(const QString &select)
{
    if (!m_scriptsWindow) {
        m_scriptsWindow = new ScriptsWindow(this);
        m_scriptsWindow->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_scriptsWindow, &ScriptsWindow::runInTabRequested, this, [this](const QString &name) {
            auto *page = activeTerminal(m_sessionTabs->currentWidget());
            if (!page) {
                QMessageBox::information(m_scriptsWindow, QStringLiteral("Scripts"),
                                         QStringLiteral("Open an SSH tab (or a local terminal) first."));
                return;
            }
            runSavedScriptInTab(page, name);
        });
        connect(m_scriptsWindow, &ScriptsWindow::scriptsChanged, m_scriptsPanel, &ScriptsPanel::refresh);
        connect(m_scriptsWindow, &ScriptsWindow::runOnHostsRequested, this, [this](const QString &name) {
            auto *item = selectedItem();
            showRunCommand(item && isFolder(item) ? item : nullptr, scriptMarker(name));
        });
    }
    if (!select.isEmpty())
        m_scriptsWindow->select(select);
    m_scriptsWindow->show();
    m_scriptsWindow->raise();
    m_scriptsWindow->activateWindow();
}

void MainWindow::showKubeCredentials(QWidget *over)
{
    KubeCredentialsWindow window(
        [this](const QString &key) {
            if (m_vault.needsUnlock(key) && !ensureVaultUnlocked())
                return QString();
            return m_vault.passwordForSession(key);
        },
        [this](const QString &key, const QString &value, QString *error) {
            if (m_vault.needsVault(key) && !ensureVaultUnlocked())
                return false;
            return m_vault.setPassword(key, value, error);
        },
        [this](const QString &key) { m_vault.removePassword(key); }, over);
    window.exec();
}

QString MainWindow::kubeCredentialContents(const QString &name)
{
    for (const auto &credential : loadKubeCredentials()) {
        if (credential.name == name)
            return KubeCredentialsWindow::contents(credential, [this](const QString &key) {
                if (m_vault.needsUnlock(key) && !ensureVaultUnlocked())
                    return QString();
                return m_vault.passwordForSession(key);
            });
    }
    return {};
}

QStringList MainWindow::savedScriptNames() const
{
    QStringList names;
    for (const auto &script : loadSavedScripts())
        names << script.name;
    return names;
}

void MainWindow::addScriptMenu(QMenu *menu, TerminalPage *page)
{
    if (!loadScriptPreferences().enabled)
        return;
    auto *scripts = menu->addMenu(QStringLiteral("Run Script"));
    const auto saved = loadSavedScripts();
    for (const auto &script : saved) {
        auto *action = scripts->addAction(script.group.isEmpty() ? script.name
                                                                 : QStringLiteral("%1 › %2").arg(script.group, script.name),
                                          this, [this, page, name = script.name] {
                                              if (auto *target = page ? page : activeTerminal(m_sessionTabs->currentWidget()))
                                                  runSavedScriptInTab(target, name);
                                          });
        action->setToolTip(script.description);
    }
    if (saved.isEmpty())
        scripts->addAction(QStringLiteral("(none yet)"))->setEnabled(false);
    scripts->addSeparator();
    scripts->addAction(QStringLiteral("Edit Scripts…"), this, [this] { showScripts(); });
}

// Runs a saved script in a tab: over SSH it is stored on the server through a
// channel of its own and run by one short line typed into the shell; in a
// local terminal it is stored here. Its output shows in the terminal.
void MainWindow::runSavedScriptInTab(TerminalPage *page, const QString &name)
{
    if (!loadScriptPreferences().enabled)
        return;
    const auto script = findSavedScript(name);
    if (!script) {
        tabMessage(page, QStringLiteral("There is no saved script called %1.").arg(name));
        return;
    }
    QHash<QString, QString> values{{QStringLiteral("user"), page->sshSettings().username},
                                   {QStringLiteral("host"), page->sshSettings().host}};
    if (!askScriptParameters(*script, &values, this))
        return;
    const auto body = filledScript(*script, values);
    // In a container the file would be outside it: the script goes into the
    // container's shell as one here-document instead (typed, so it shows).
    if (page->property("container").toBool()) {
        page->sendInput((typedScript(*script, body) + QLatin1Char('\r')).toUtf8());
        return;
    }
    if (page->isLocal()) {
        if (const auto problem = Platform::problem(Platform::Feature::LocalScripts); !problem.isEmpty()) {
            tabMessage(page, problem);
            return;
        }
        QTemporaryFile file(QDir::temp().filePath(QStringLiteral("opsremote-XXXXXX")));
        file.setAutoRemove(false);
        if (!file.open() || file.write(body.toUtf8() + '\n') < 0) {
            tabMessage(page, QStringLiteral("Could not store the script."), true);
            return;
        }
        file.close();
        page->sendInput((runStoredScriptLine(*script, file.fileName()) + QLatin1Char('\r')).toUtf8());
        return;
    }
    auto *ssh = page->sshSession();
    if (!ssh || !page->isConnected()) {
        tabMessage(page, Platform::has(Platform::Feature::LocalScripts)
                             ? QStringLiteral("Scripts run in connected SSH tabs (and local terminals).")
                             : QStringLiteral("Scripts run in connected SSH tabs."));
        return;
    }
    // Stored through the channel's input and kept for the line typed below,
    // which runs and removes it.
    const int id = ssh->runCommand(QStringLiteral("echo OPSREMOTE-SCRIPT:") + RemoteFilePlaceholder, false, 30, 4096,
                                   scriptFile(body), true);
    auto *connection = new QMetaObject::Connection;
    *connection = connect(ssh, &SshSession::commandFinished, page,
                          [this, page, id, connection, script = *script](int finished, int exitStatus,
                                                                         const QByteArray &output, bool,
                                                                         const QString &error, int) {
                              if (finished != id)
                                  return;
                              disconnect(*connection);
                              delete connection;
                              const auto marker = QByteArrayLiteral("OPSREMOTE-SCRIPT:");
                              const int at = output.indexOf(marker);
                              if (exitStatus != 0 || at < 0) {
                                  tabMessage(page,
                                             QStringLiteral("Could not send the script %1 to the server%2.")
                                                 .arg(script.name, error.isEmpty() ? QString() : QStringLiteral(": ") + error),
                                             true);
                                  return;
                              }
                              const auto path = QString::fromUtf8(output.mid(at + marker.size())).section(QLatin1Char('\n'), 0, 0).trimmed();
                              page->sendInput((runStoredScriptLine(script, path) + QLatin1Char('\r')).toUtf8());
                          });
}

QList<RunCommandWindow::Resolved> MainWindow::resolveCommandHosts(const QStringList &ids)
{
    QList<RunCommandWindow::Resolved> results;
    bool askedVault = false;
    for (const auto &id : ids) {
        RunCommandWindow::Resolved result;
        const auto session = m_sessions.constFind(id);
        if (session == m_sessions.cend()
            || session->protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            result.problem = QStringLiteral("No longer a saved SSH session.");
            results.append(result);
            continue;
        }
        result.settings = sshSettingsFor(session->host, session->port, session->username, sessionSshOptions(*session));
        if (!resolveJumpHosts(*session, &result.settings.jumps)) {
            result.problem = QStringLiteral("Its jump host cannot be used.");
            results.append(result);
            continue;
        }
        applyProxy(*session, &result.settings, result.settings.jumps);
        // Once per run: without the vault, keys and the log-on prompt still work.
        if (!askedVault && m_vault.needsUnlock(id)) {
            askedVault = true;
            ensureVaultUnlocked();
        }
        result.settings.password = m_vault.passwordForSession(id);
        results.append(result);
    }
    return results;
}

void MainWindow::showKeyManager()
{
    KeyManagerDialog dialog(
        [this] { return jumpHostChoices(); },
        [this](const QString &id, SshConnectionSettings *settings) {
            return resolveTunnelServer(JumpHost::SessionPrefix + id, settings);
        },
        [this](const QString &id, const QString &keyPath) {
            auto session = m_sessions.find(id);
            if (session == m_sessions.end())
                return;
            session->ssh.identityFile = keyPath;
            saveSessions();
        },
        [this](const QString &keyPath, const QString &passphrase) {
            if (passphrase.isEmpty() && (!m_vault.exists() || !m_vault.isUnlocked()))
                return true; // nothing could be stored
            if (m_vault.needsVault(keyPassphraseVaultKey(keyPath)) && !ensureVaultUnlocked())
                return false;
            QString error;
            const auto key = keyPassphraseVaultKey(keyPath);
            return passphrase.isEmpty() ? m_vault.removePassword(key, &error)
                                        : m_vault.setPassword(key, passphrase, &error);
        },
        [this](QWidget *over) { return createSshSession(over); }, this);
    dialog.exec();
}

void MainWindow::showTunnels()
{
    m_tunnelsWindow->show();
    m_tunnelsWindow->raise();
    m_tunnelsWindow->activateWindow();
}
