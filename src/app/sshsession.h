// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include <optional>
#include <QStringList>

#include "portforward.h"
#include "proxy.h"
#include "remotefiles.h"

#include <vector>

class QThread;
class SshWorker;

struct SshConnectionSettings
{
    QString host;
    quint16 port = 22;
    QString username;
    // Optional password from the credential vault. It is tried once, then the
    // user is prompted interactively.
    QString password;
    QString knownHostsPath;
    QString terminalType = QStringLiteral("xterm-256color");
    int connectTimeout = 15; // seconds
    int keepaliveSeconds = 60; // 0 turns keepalives off
    bool compression = false;
    // Keys from ~/.ssh and the SSH agent.
    bool tryPublicKeys = true;
    // The server may use the local SSH agent (ssh -A); shell sessions only.
    bool forwardAgent = false;
    // Tags the shell (LC_OPSREMOTE_TAG) so its folder can be found for the
    // SFTP browser (SshSession::queryShellFolder).
    bool followTerminalFolder = false;
    // How the file browser reaches files: "auto" (SFTP, else SCP), "sftp",
    // or "scp".
    QString fileTransfer = QStringLiteral("auto");
    // X11 programs on the server show on the local X server (DISPLAY, or
    // :0 on Windows); shell sessions only.
    bool forwardX11 = false;
    // Windows: which agent: "auto" (the OpenSSH agent, else Pageant),
    // "openssh" or "pageant".
    QString agent = QStringLiteral("auto");
    // The passphrase of identityFile, from the vault; empty asks when the
    // key has one.
    QString keyPassphrase;
    // An extra private key, tried before the others.
    QString identityFile;
    int columns = 80;
    int rows = 24;
    // What the terminal tab does after a dropped connection.
    bool autoReconnect = false;
    int reconnectDelay = 5; // seconds
    int reconnectAttempts = 5;
    // Tunnel mode (forwardPort > 0): instead of a shell, connections to a
    // local port are forwarded through the server to forwardHost:forwardPort
    // (like ssh -L); the local port is reported by forwardReady().
    QString forwardHost;
    quint16 forwardPort = 0;
    // Tunnel mode: stop listening after the first connection, so no other
    // program on this computer can use the tunnel.
    bool forwardOnce = false;
    // Through jump hosts: connect to this local port (the last jump host's
    // forward) instead of host:port. host and port stay the real server's,
    // for its host key and for messages.
    quint16 viaLocalPort = 0;
    // Reach host:port through this proxy (when not through viaLocalPort).
    ProxySettings proxy;
    // Port forwards opened once logged in, reported by forwardStatus().
    QList<PortForward> forwards;
    // False: only the forwards, no shell or SFTP (a saved tunnel).
    bool openShell = true;
    // With openShell false: the file browser's connection only (SFTP or SCP
    // sessions, which have no terminal).
    bool filesOnly = false;
    // SSH servers to pass through first, in order (see JumpChain).
    std::vector<SshConnectionSettings> jumps;
};

enum class SshHostKeyStatus { Unknown, Changed, OtherType };
enum class SshHostKeyDecision { Reject, AcceptOnce, AcceptAndSave };

// GUI-thread handle for one interactive SSH shell. libssh runs on a dedicated
// worker thread so name resolution, key exchange, and authentication never
// block the UI. All signals are delivered on the thread that owns this object.
class SshSession final : public RemoteFileSession
{
    Q_OBJECT

public:
    explicit SshSession(QObject *parent = nullptr);
    ~SshSession() override;

    // Disconnects every session and waits up to timeoutMs for their worker
    // threads to finish. Call once the event loop has ended: exiting while a
    // worker is still inside libssh or OpenSSL can hang the process on
    // Windows. Returns false if a worker is still running (for example, stuck
    // in a connection attempt).
    static bool shutdownAll(int timeoutMs);

    void connectToHost(const SshConnectionSettings &settings);
    void write(const QByteArray &data);
    void resizeTerminal(int columns, int rows);
    void answerHostKey(SshHostKeyDecision decision);
    // Opens more forwards on this connection, numbered from firstIndex (kept
    // apart from the session's own, which count from 0); their state comes
    // with forwardStatus() and forwardStats() under those numbers.
    void addForwards(const QList<PortForward> &forwards, int firstIndex);
    void removeForwards(int firstIndex, int count);
    // Whether the server reaches host:port (a Unix socket: its path and
    // port 0); answered by probeFinished().
    void probe(int id, const QString &host, quint16 port);
    // Runs a command on the server (without a shell tab: openShell off),
    // answered by commandFinished() with the id returned. terminal: with a
    // pseudo-terminal, for commands that need one. Output past outputLimit
    // bytes is dropped.
    int runCommand(const QString &command, bool terminal, int timeoutSeconds, int outputLimit);
    // Ends the command under way, if any.
    void cancelCommands();
    // Answers the prompts from the last inputRequired() signal, in order.
    void answerPrompts(const QStringList &answers);
    void cancelPrompts();

    // Files go over SFTP, a second channel on the same connection once the
    // shell is open, or over SCP (SshConnectionSettings::fileTransfer);
    // sftpReady() or sftpUnavailable() reports the outcome. Remote paths are
    // absolute and use '/'.
    QString fileProtocol() const override { return m_fileProtocol; }
    void listDirectory(const QString &path) override;
    void queryShellFolder() override;
    void makeDirectory(const QString &path) override;
    void renamePath(const QString &from, const QString &to) override;
    void removePaths(const QStringList &paths) override;
    int upload(const QStringList &localPaths, const QString &remoteDirectory,
               std::optional<SftpConflictResolution> policy = std::nullopt) override;
    int download(const QStringList &remotePaths, const QString &localDirectory,
                 std::optional<SftpConflictResolution> policy = std::nullopt) override;
    void scanTree(const QString &path) override;
    void resolveConflict(SftpConflictResolution resolution, bool applyToAll) override;
    void cancelTransfers() override;

signals:
    void statusMessage(const QString &message);
    void hostKeyVerificationRequired(SshHostKeyStatus status, const QString &keyType,
                                     const QString &fingerprint);
    void inputRequired(const QString &instruction, const QStringList &prompts,
                       const QList<bool> &echo);
    void connected();
    // Tunnel mode: the local port (on 127.0.0.1) to connect to.
    void forwardReady(quint16 localPort);
    // Whether forwards[index] could start listening, and what it does or
    // why it failed.
    void forwardStatus(int index, bool listening, const QString &message);
    // One connection through forwards[index] could not be made; the
    // forward keeps listening.
    void forwardConnectionFailed(int index, const QString &message);
    // Each second while it changes: open connections and bytes through a
    // forward, received from and sent to its far end.
    void forwardStats(int index, int connections, qint64 received, qint64 sent);
    void probeFinished(int id, bool reachable, const QString &message, int milliseconds);
    // exitStatus: -1 when unknown (error says why).
    void commandFinished(int id, int exitStatus, const QByteArray &output, bool truncated,
                         const QString &error, int milliseconds);
    void dataReceived(const QByteArray &data);
    void disconnected(const QString &reason, bool error);

private:
    QThread *m_thread = nullptr;
    SshWorker *m_worker = nullptr;
    int m_nextJobId = 1;
    QString m_fileProtocol = QStringLiteral("SFTP");
};
