// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "newentry.h"
#include "sshsession.h"

#include <QDialog>
#include <QPointer>

#include <functional>

class JumpChain;
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

// Tools → SSH Keys: the key pairs in settings/keys/ (create, import,
// delete, copy the public key) and, below the list, installing the selected
// key on a server like ssh-copy-id.
class KeyManagerDialog final : public QDialog
{
    Q_OBJECT

public:
    // Saved SSH sessions: (id, label).
    using SessionChoices = std::function<QList<QPair<QString, QString>>()>;
    // A saved session's connection (with jump hosts, proxy and password);
    // false when it cannot be used (the reason has been shown).
    using SessionResolver = std::function<bool(const QString &id, SshConnectionSettings *)>;
    // Makes a session log on with a key (keyPath relative to the program's
    // folder).
    using UseKey = std::function<void(const QString &id, const QString &keyPath)>;
    // Keeps (or with an empty passphrase, forgets) a key's passphrase in the
    // credential vault; false if the vault could not be used.
    using RememberPassphrase = std::function<bool(const QString &keyPath, const QString &passphrase)>;

    // makeSession: "New…" in the list of servers (see NewEntry).
    KeyManagerDialog(SessionChoices sessions, SessionResolver resolver, UseKey useKey,
                     RememberPassphrase rememberPassphrase, NewEntry::SessionMaker makeSession,
                     QWidget *parent);

private:
    void refresh(const QString &select = QString());
    QString selectedName() const;
    void showSelected();
    void createKey();
    void importKey();
    void deleteKey();
    // A saved session is chosen (not "New…").
    bool serverChosen() const;
    void install();
    void installThrough(quint16 viaLocalPort);
    void finishInstall(const QString &message, bool success);

    SessionChoices m_sessions;
    SessionResolver m_resolver;
    UseKey m_useKey;
    RememberPassphrase m_rememberPassphrase;

    QTreeWidget *m_list = nullptr;
    QPlainTextEdit *m_publicKey = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_delete = nullptr;
    QComboBox *m_server = nullptr;
    QCheckBox *m_useForSession = nullptr;
    QPushButton *m_installButton = nullptr;
    QLabel *m_installStatus = nullptr;

    QPointer<SshSession> m_ssh;
    QPointer<JumpChain> m_chain;
    SshConnectionSettings m_target;
    QString m_installKey; // the public key line being installed
    QString m_installName;
    QByteArray m_output;
};
