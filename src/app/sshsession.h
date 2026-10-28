// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

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
    // An extra unencrypted private key, tried before the others.
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
};

enum class SshHostKeyStatus { Unknown, Changed, OtherType };
enum class SshHostKeyDecision { Reject, AcceptOnce, AcceptAndSave };

struct SftpEntry
{
    QString name;
    bool isDirectory = false;
    bool isSymlink = false;
    quint64 size = 0;
    QDateTime modified;
    quint32 permissions = 0;
};

enum class SftpConflictResolution { Overwrite, Skip, Rename, Cancel };

// GUI-thread handle for one interactive SSH shell. libssh runs on a dedicated
// worker thread so name resolution, key exchange, and authentication never
// block the UI. All signals are delivered on the thread that owns this object.
class SshSession final : public QObject
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
    // Answers the prompts from the last inputRequired() signal, in order.
    void answerPrompts(const QStringList &answers);
    void cancelPrompts();

    // SFTP runs as a second channel on the same connection once the shell is
    // open; sftpReady() or sftpUnavailable() reports the outcome. Remote paths
    // are absolute and use '/'.
    void listDirectory(const QString &path);
    void makeDirectory(const QString &path);
    void renamePath(const QString &from, const QString &to);
    // Deletes files, and folders with all of their contents.
    void removePaths(const QStringList &paths);
    // Transfers are queued and run one at a time; each returns its job ID.
    // Folders are copied recursively.
    int upload(const QStringList &localPaths, const QString &remoteDirectory);
    int download(const QStringList &remotePaths, const QString &localDirectory);
    // Answers the last transferConflict() signal.
    void resolveConflict(SftpConflictResolution resolution, bool applyToAll);
    // Cancels the running transfer and everything queued after it.
    void cancelTransfers();

signals:
    void statusMessage(const QString &message);
    void hostKeyVerificationRequired(SshHostKeyStatus status, const QString &keyType,
                                     const QString &fingerprint);
    void inputRequired(const QString &instruction, const QStringList &prompts,
                       const QList<bool> &echo);
    void connected();
    // Tunnel mode: the local port (on 127.0.0.1) to connect to.
    void forwardReady(quint16 localPort);
    void dataReceived(const QByteArray &data);
    void disconnected(const QString &reason, bool error);

    void sftpReady(const QString &homePath);
    void sftpUnavailable(const QString &error);
    // path is canonical; on error, entries is empty.
    void directoryListed(const QString &path, const QList<SftpEntry> &entries,
                         const QString &error);
    // Reports the end of makeDirectory(), renamePath(), or removePaths().
    void fileOperationFinished(const QString &error);
    void transferProgress(int jobId, const QString &currentFile, quint64 bytesDone,
                          quint64 bytesTotal, int filesDone, int filesTotal, int queuedJobs);
    void transferConflict(const QString &path, quint64 existingSize,
                          const QDateTime &existingModified, quint64 newSize,
                          const QDateTime &newModified, bool upload);
    // errors lists per-file failures; skipped counts files the user chose to skip.
    void transferFinished(int jobId, bool upload, const QString &destination,
                          int filesTransferred, int skipped, const QStringList &errors,
                          bool cancelled);

private:
    QThread *m_thread = nullptr;
    SshWorker *m_worker = nullptr;
    int m_nextJobId = 1;
};
