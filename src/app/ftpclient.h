// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "proxy.h"
#include "remotefiles.h"

#include <QByteArray>
#include <QString>

class QThread;
class FtpWorker;

struct FtpConnectionSettings
{
    QString host;
    quint16 port = 21;
    // Empty: anonymous.
    QString username;
    QString password;
    // "auto" (TLS when the server offers it), "explicit" (AUTH TLS,
    // required), "implicit" (TLS from the start, usually port 990), "none".
    QString security = QStringLiteral("auto");
    // Passive (the client opens data connections) or active (the server
    // connects back to this computer).
    bool passive = true;
    // "auto" (MLSD when the server offers it) or "list".
    QString listing = QStringLiteral("auto");
    int connectTimeout = 15; // seconds
    int keepaliveSeconds = 60; // NOOP while idle; 0: off
    // Certificates the user accepted: "host:port sha256" lines.
    QString knownCertificatesPath;
    ProxySettings proxy;
};

enum class FtpCertificateDecision { Reject, AcceptOnce, AcceptAndSave };

// GUI-thread handle for one FTP or FTPS connection, for the file browser.
// The protocol runs on a worker thread with two control connections: one for
// listings and folder operations, one for transfers, so browsing goes on
// while a file is copied.
class FtpSession final : public RemoteFileSession
{
    Q_OBJECT

public:
    explicit FtpSession(QObject *parent = nullptr);
    ~FtpSession() override;

    void connectToHost(const FtpConnectionSettings &settings);
    void answerCertificate(FtpCertificateDecision decision);
    // After plainTextQuestion(): connects anyway (remember: for this server
    // from now on), or not.
    void answerPlainText(bool allow, bool remember);
    // After loginFailed(): tries again with this password.
    void retryLogin(const QString &password);

    QString fileProtocol() const override { return m_protocol; }
    void listDirectory(const QString &path) override;
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
    // The protocol conversation (passwords hidden); outgoing lines start
    // with "> ".
    void logLine(const QString &line);
    // The server's certificate is not trusted: answer with answerCertificate().
    // changed: another certificate was accepted for this server before.
    void certificateQuestion(const QString &problems, const QString &fingerprint,
                             const QString &details, bool changed);
    // The server offers no TLS and the log-on would send the password in
    // plain text: answer with answerPlainText().
    void plainTextQuestion(const QString &message);
    // The server refused the user name or password: answer with retryLogin().
    void loginFailed(const QString &message);
    void connected(const QString &protocol);
    void disconnected(const QString &reason, bool error);

private:
    QThread *m_thread = nullptr;
    FtpWorker *m_worker = nullptr;
    int m_nextJobId = 1;
    QString m_protocol = QStringLiteral("FTP");
};

// SHA-256 fingerprint as colon-separated hex.
QString ftpFingerprint(const QByteArray &digest);
