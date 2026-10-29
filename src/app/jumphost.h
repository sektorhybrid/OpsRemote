// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "newentry.h"
#include "sshsession.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QWidget>

class QComboBox;
class QWidget;

// A session's or folder's jump host, as stored in sessions.yaml:
//   ""                  a session: its folder's; a folder: its parent's
//   "none"              connect directly
//   "session:<id>"      a saved SSH session (which may have a jump host of
//                       its own, so hops chain)
//   "user@host[:port]"  any SSH server
namespace JumpHost {
inline const QString Direct = QStringLiteral("none");
inline const QString SessionPrefix = QStringLiteral("session:");
// Splits "user@host[:port]"; user and port stay empty / 0 when not given.
bool parseAddress(const QString &spec, QString *user, QString *host, quint16 *port);
}

// Dialogs for an SSH connection without a terminal (jump hosts, tunnels):
// an unknown or changed host key, and log-on prompts. server names it, as in
// "jump host bastion".
void askSshHostKey(QWidget *parent, SshSession *session, const QString &server,
                   SshHostKeyStatus status, const QString &keyType, const QString &fingerprint);
void askSshLogOn(QWidget *parent, SshSession *session, const QString &server,
                 const QString &instruction, const QStringList &prompts, const QList<bool> &echo);

// Opens the jump hosts one after another, each through the one before it,
// and ends with a local port that reaches the target. The hops ask for host
// keys and passwords with dialogs over dialogParent.
class JumpChain final : public QObject
{
    Q_OBJECT

public:
    explicit JumpChain(QWidget *dialogParent);
    ~JumpChain() override;

    // Hops in order (the first is reached directly). Takes their passwords.
    // With singleConnection, the local port accepts one connection and then
    // closes (RDP may connect more than once, so it passes false).
    void open(std::vector<SshConnectionSettings> hops, const QString &targetHost, quint16 targetPort,
              bool singleConnection = true);

signals:
    void statusMessage(const QString &message);
    // Connections to 127.0.0.1:localPort reach the target.
    void ready(quint16 localPort);
    // A hop could not connect, or one dropped later.
    void failed(const QString &reason, bool error);

private:
    void openHop(size_t index, quint16 viaLocalPort);

    QPointer<QWidget> m_dialogParent;
    std::vector<SshConnectionSettings> m_hops;
    QString m_targetHost;
    quint16 m_targetPort = 0;
    bool m_singleConnection = true;
    QList<QPointer<SshSession>> m_sessions;
    bool m_failed = false;
};

// The "Jump host" tab of the session and folder windows. inherited names
// what an empty value means here ("the folder's: …" or "none").
// sshSessions lists the saved SSH sessions (id, label); exclude is left out
// (a session cannot be its own jump host).
class JumpHostEditor final : public QWidget
{
    Q_OBJECT

public:
    JumpHostEditor(const QString &value, const QString &inherited,
                   const QList<QPair<QString, QString>> &sshSessions,
                   const QString &exclude = QString(), QWidget *parent = nullptr);

    QString value() const;
    // Adds "New…" to the list (a new SSH session, chosen once saved).
    void offerNew(NewEntry::SessionMaker make, NewEntry::SessionChoices choices);

private:
    QComboBox *m_choice = nullptr;
};
