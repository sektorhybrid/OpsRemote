// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sshsession.h"

#include "appdirs.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QDeadlineTimer>
#include <QMutex>
#include <QSet>
#include <QThread>
#include <QRegularExpression>
#include <QTimer>
#include <QTcpSocket>
#include <QTcpServer>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <fcntl.h>
#ifndef _WIN32
#include <sys/time.h>
#endif

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace {
constexpr int MaximumPasswordAttempts = 3;
constexpr long ConnectTimeoutSeconds = 15;
constexpr qsizetype MaximumReadBatch = 256 * 1024;
constexpr size_t DefaultSftpChunk = 32 * 1024;
constexpr size_t MaximumSftpChunk = 256 * 1024;
constexpr int MaximumTreeDepth = 64;
// Transfers plan every entry first; this bounds the memory a huge tree uses.
constexpr qsizetype MaximumPlannedEntries = 200000;
constexpr qint64 ProgressIntervalMs = 100;
constexpr qint64 SftpQuietPeriodMs = 250;
constexpr qint64 SftpMaximumDrainMs = 2000;

QString joinRemotePath(const QString &directory, const QString &name)
{
    return directory.endsWith(QLatin1Char('/')) ? directory + name
                                                : directory + QLatin1Char('/') + name;
}

// Whether a remote entry name can be used as one local file name without
// leaving the download folder: no separators, drive letters, or control
// characters, and on Windows none of its reserved device names.
bool isSafeLocalName(const QString &name)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return false;
    for (const QChar c : name) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') || c.unicode() < 0x20)
            return false;
    }
#ifdef Q_OS_WIN
    // Windows drops trailing dots and spaces, and maps these names to devices.
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        return false;
    static const QRegularExpression reserved(
        QStringLiteral("^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch())
        return false;
    for (const QChar c : name) {
        if (QStringLiteral("<>\"|?*").contains(c))
            return false;
    }
#endif
    return true;
}

// Whether path lies inside folder (after resolving "." and "..").
bool isInside(const QString &path, const QString &folder)
{
    const auto root = QDir::cleanPath(QDir(folder).absolutePath());
    const auto target = QDir::cleanPath(QDir(path).absolutePath());
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return target.startsWith(root + QLatin1Char('/'), sensitivity);
}

QString remoteFileName(const QString &path)
{
    auto trimmed = path;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    return trimmed.mid(trimmed.lastIndexOf(QLatin1Char('/')) + 1);
}

QDateTime fromUnixTime(quint32 seconds)
{
    return seconds == 0 ? QDateTime() : QDateTime::fromSecsSinceEpoch(seconds);
}

// Worker threads that have not finished yet; see SshSession::shutdownAll().
QMutex workerThreadsMutex;
QSet<QThread *> workerThreads;

void ensureLibsshInitialized()
{
    static std::once_flag once;
    std::call_once(once, [] { ssh_init(); });
}

void wipe(QByteArray &bytes)
{
    bytes.fill('\0');
    bytes.clear();
}

void wipe(QString &text)
{
    text.fill(QChar(0));
    text.clear();
}

// libssh writes plain (unhashed) host names, using "[host]:port" for
// non-default ports.
QByteArray knownHostsToken(const QString &host, quint16 port)
{
    return (port == 22 ? host : QStringLiteral("[%1]:%2").arg(host).arg(port)).toUtf8();
}

bool removeKnownHostEntries(const QString &path, const QByteArray &token, QString *error)
{
    QFile file(path);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }

    QByteArray kept;
    bool removed = false;
    while (!file.atEnd()) {
        const auto line = file.readLine();
        const auto trimmed = line.trimmed();
        bool matches = false;
        if (!trimmed.isEmpty() && !trimmed.startsWith('#')) {
            const auto hosts = trimmed.left(trimmed.indexOf(' ')).split(',');
            matches = hosts.contains(token);
        }
        if (matches)
            removed = true;
        else
            kept.append(line);
    }
    file.close();
    if (!removed)
        return true;

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(kept) != kept.size()
        || !output.commit()) {
        *error = output.errorString();
        return false;
    }
    restrictToOwner(path);
    return true;
}
}

class SshWorker final : public QObject
{
    Q_OBJECT

public:
    ~SshWorker() override { closeSession(); }

    void start(const SshConnectionSettings &settings);
    void write(const QByteArray &data);
    void resize(int columns, int rows);
    void answerHostKey(SshHostKeyDecision decision);
    void answerPrompts(QStringList answers);
    void cancelPrompts();

    void listDirectory(const QString &path);
    void makeDirectory(const QString &path);
    void renamePath(const QString &from, const QString &to);
    void removePaths(const QStringList &paths);
    void startTransfer(int jobId, bool upload, const QStringList &sources,
                       const QString &destination);
    void resolveConflict(SftpConflictResolution resolution, bool applyToAll);
    void cancelTransfers();

signals:
    void statusMessage(const QString &message);
    void hostKeyVerificationRequired(SshHostKeyStatus status, const QString &keyType,
                                     const QString &fingerprint);
    void inputRequired(const QString &instruction, const QStringList &prompts,
                       const QList<bool> &echo);
    void connected();
    void forwardReady(quint16 localPort);
    void dataReceived(const QByteArray &data);
    void disconnected(const QString &reason, bool error);

    void sftpReady(const QString &homePath);
    void sftpUnavailable(const QString &error);
    void directoryListed(const QString &path, const QList<SftpEntry> &entries,
                         const QString &error);
    void fileOperationFinished(const QString &error);
    void transferProgress(int jobId, const QString &currentFile, quint64 bytesDone,
                          quint64 bytesTotal, int filesDone, int filesTotal, int queuedJobs);
    void transferConflict(const QString &path, quint64 existingSize,
                          const QDateTime &existingModified, quint64 newSize,
                          const QDateTime &newModified, bool upload);
    void transferFinished(int jobId, bool upload, const QString &destination,
                          int filesTransferred, int skipped, const QStringList &errors,
                          bool cancelled);

private:
    enum class AuthStage { PublicKey, StoredPassword, KeyboardInteractive, PasswordPrompt, Failed };
    enum class PendingInput { None, KeyboardInteractive, Password };
    enum class TaskStart { Opened, Finished, Waiting };

    struct TransferTask
    {
        bool isDirectory = false;
        QString source;
        QString destination;
        quint64 size = 0;
        QDateTime modified;
        bool executable = false;
    };

    struct TransferJob
    {
        int id = 0;
        bool upload = true;
        QStringList sources;
        QString destination;
    };

    void initializeSftp();
    QString sftpError(const QString &action, const QString &path) const;
    bool removeRecursively(const QString &path, int depth, QString *error);
    bool planUpload(const TransferJob &job, QString *error);
    bool planDownload(const TransferJob &job, QString *error);
    bool planRemoteTree(const QString &remotePath, const QString &localPath, int depth,
                        QString *error);
    void startNextJob();
    void scheduleTransferStep();
    void transferStep();
    TaskStart beginTask();
    void completeFile();
    void failFile(const QString &error);
    bool closeTransferFiles(bool success);
    void finishJob(bool cancelled);
    void emitProgress(bool force);
    bool destinationExists(const QString &path, quint64 *size, QDateTime *modified);
    QString uniqueDestination(const QString &path);

    void verifyHostKey();
    void beginAuthentication();
    void continueAuthentication();
    bool handleAuthResult(int result);
    bool runKeyboardInteractive();
    void openShell();
    void startKeepalive();
    void openForward();
    void forwardFromSocket(QTcpSocket *socket);
    void pumpForwards();
    void closeForward(QTcpSocket *socket);
    void readAvailable();
    void finish(const QString &reason, bool error);
    void closeSession();
    QString lastError() const;

    SshConnectionSettings m_settings;
    ssh_session m_session = nullptr;
    ssh_channel m_channel = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    // Tunnel mode: the local listener and one SSH channel per connection.
    struct Forward
    {
        QTcpSocket *socket = nullptr;
        ssh_channel channel = nullptr;
    };
    QTcpServer *m_forwardServer = nullptr;
    QList<Forward> m_forwards;
    SshHostKeyStatus m_hostKeyStatus = SshHostKeyStatus::Unknown;
    bool m_awaitingHostKey = false;
    AuthStage m_stage = AuthStage::PublicKey;
    PendingInput m_pendingInput = PendingInput::None;
    int m_authMethods = 0;
    int m_failedAttempts = 0;
    bool m_storedPasswordTried = false;
    bool m_finished = false;

    sftp_session m_sftp = nullptr;
    size_t m_readChunk = DefaultSftpChunk;
    size_t m_writeChunk = DefaultSftpChunk;
    QList<TransferJob> m_jobQueue;
    std::optional<TransferJob> m_job;
    QList<TransferTask> m_tasks;
    qsizetype m_taskIndex = 0;
    quint64 m_jobBytesTotal = 0;
    quint64 m_jobBytesDone = 0;
    quint64 m_taskBytesDone = 0;
    int m_jobFiles = 0;
    int m_filesDone = 0;
    int m_filesTransferred = 0;
    int m_skipped = 0;
    QStringList m_transferErrors;
    std::optional<SftpConflictResolution> m_conflictPolicy;
    std::optional<SftpConflictResolution> m_taskResolution;
    bool m_awaitingConflict = false;
    bool m_stepScheduled = false;
    bool m_cancelRequested = false;
    sftp_file m_remoteFile = nullptr;
    // True once an upload has created or truncated its destination.
    bool m_remoteFileWritten = false;
    std::unique_ptr<QFileDevice> m_localFile;
    QElapsedTimer m_progressTimer;
};

void SshWorker::start(const SshConnectionSettings &settings)
{
    ensureLibsshInitialized();
    m_settings = settings;
    m_session = ssh_new();
    if (!m_session) {
        finish(QStringLiteral("Could not create an SSH session."), true);
        return;
    }

    // Saved sessions are self-contained: do not let ~/.ssh/config rewrite the
    // host, port, or user of a portable profile.
    const bool processConfig = false;
    const auto host = m_settings.host.toUtf8();
    const unsigned int port = m_settings.port;
    const auto user = m_settings.username.toUtf8();
    const long timeout = m_settings.connectTimeout > 0 ? m_settings.connectTimeout
                                                       : ConnectTimeoutSeconds;
    const auto knownHosts = QDir::toNativeSeparators(m_settings.knownHostsPath).toUtf8();
    if (ssh_options_set(m_session, SSH_OPTIONS_PROCESS_CONFIG, &processConfig) < 0
        || ssh_options_set(m_session, SSH_OPTIONS_HOST, host.constData()) < 0
        || ssh_options_set(m_session, SSH_OPTIONS_PORT, &port) < 0
        || ssh_options_set(m_session, SSH_OPTIONS_USER, user.constData()) < 0
        || ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeout) < 0
        || (!knownHosts.isEmpty()
            && ssh_options_set(m_session, SSH_OPTIONS_KNOWNHOSTS, knownHosts.constData()) < 0)) {
        finish(QStringLiteral("Invalid SSH connection settings: %1").arg(lastError()), true);
        return;
    }
    if (m_settings.compression) {
        ssh_options_set(m_session, SSH_OPTIONS_COMPRESSION, "yes");
    }

    emit statusMessage(QStringLiteral("Connecting to %1:%2…").arg(m_settings.host).arg(port));
    if (ssh_connect(m_session) != SSH_OK) {
        finish(QStringLiteral("Could not connect to %1:%2: %3")
                   .arg(m_settings.host).arg(port).arg(lastError()),
               true);
        return;
    }
    verifyHostKey();
}

void SshWorker::verifyHostKey()
{
    ssh_key key = nullptr;
    if (ssh_get_server_publickey(m_session, &key) != SSH_OK) {
        finish(QStringLiteral("Could not read the server host key: %1").arg(lastError()), true);
        return;
    }
    const auto keyType = QString::fromUtf8(ssh_key_type_to_char(ssh_key_type(key)));
    unsigned char *hash = nullptr;
    size_t hashLength = 0;
    const int hashResult = ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256,
                                                  &hash, &hashLength);
    ssh_key_free(key);
    if (hashResult < 0) {
        finish(QStringLiteral("Could not fingerprint the server host key."), true);
        return;
    }
    char *fingerprintText = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength);
    ssh_clean_pubkey_hash(&hash);
    const auto fingerprint = QString::fromUtf8(fingerprintText);
    ssh_string_free_char(fingerprintText);

    switch (ssh_session_is_known_server(m_session)) {
    case SSH_KNOWN_HOSTS_OK:
        beginAuthentication();
        return;
    case SSH_KNOWN_HOSTS_CHANGED:
        m_hostKeyStatus = SshHostKeyStatus::Changed;
        break;
    case SSH_KNOWN_HOSTS_OTHER:
        m_hostKeyStatus = SshHostKeyStatus::OtherType;
        break;
    case SSH_KNOWN_HOSTS_UNKNOWN:
    case SSH_KNOWN_HOSTS_NOT_FOUND:
        m_hostKeyStatus = SshHostKeyStatus::Unknown;
        break;
    case SSH_KNOWN_HOSTS_ERROR:
    default:
        finish(QStringLiteral("Could not check known hosts: %1").arg(lastError()), true);
        return;
    }
    m_awaitingHostKey = true;
    emit hostKeyVerificationRequired(m_hostKeyStatus, keyType, fingerprint);
}

void SshWorker::answerHostKey(SshHostKeyDecision decision)
{
    if (!std::exchange(m_awaitingHostKey, false) || m_finished)
        return;
    if (decision == SshHostKeyDecision::Reject) {
        finish(QStringLiteral("Host key was not accepted; disconnected."), false);
        return;
    }
    if (decision == SshHostKeyDecision::AcceptAndSave) {
        QString error;
        QDir().mkpath(QFileInfo(m_settings.knownHostsPath).absolutePath());
        if (m_hostKeyStatus == SshHostKeyStatus::Changed
            && !removeKnownHostEntries(m_settings.knownHostsPath,
                                       knownHostsToken(m_settings.host, m_settings.port), &error))
            emit statusMessage(QStringLiteral("Could not remove the old host key: %1").arg(error));
        if (ssh_session_update_known_hosts(m_session) != SSH_OK)
            emit statusMessage(QStringLiteral("Could not save the host key: %1").arg(lastError()));
        else
            restrictToOwner(m_settings.knownHostsPath);
    }
    beginAuthentication();
}

void SshWorker::beginAuthentication()
{
    const int result = ssh_userauth_none(m_session, nullptr);
    if (result == SSH_AUTH_SUCCESS) {
        openShell();
        return;
    }
    if (result == SSH_AUTH_ERROR) {
        finish(QStringLiteral("Authentication failed: %1").arg(lastError()), true);
        return;
    }
    if (char *banner = ssh_get_issue_banner(m_session)) {
        emit statusMessage(QString::fromUtf8(banner).trimmed());
        ssh_string_free_char(banner);
    }
    m_authMethods = ssh_userauth_list(m_session, nullptr);
    m_stage = AuthStage::PublicKey;
    continueAuthentication();
}

// Tries each authentication method in turn. Returns early while waiting for
// the user to answer a prompt; answerPrompts() resumes from the same stage.
void SshWorker::continueAuthentication()
{
    for (;;) {
        switch (m_stage) {
        case AuthStage::PublicKey:
            m_stage = AuthStage::StoredPassword;
            if (!(m_authMethods & SSH_AUTH_METHOD_PUBLICKEY))
                break;
            if (!m_settings.identityFile.isEmpty()) {
                ssh_key key = nullptr;
                const auto path = QDir::toNativeSeparators(m_settings.identityFile).toUtf8();
                if (ssh_pki_import_privkey_file(path.constData(), nullptr, nullptr, nullptr, &key)
                    == SSH_OK) {
                    const int result = ssh_userauth_publickey(m_session, nullptr, key);
                    ssh_key_free(key);
                    if (handleAuthResult(result))
                        return;
                } else {
                    emit statusMessage(QStringLiteral("Could not read the private key %1 "
                                                      "(keys with a passphrase are not supported "
                                                      "yet).").arg(m_settings.identityFile));
                }
            }
            if (m_settings.tryPublicKeys
                && handleAuthResult(ssh_userauth_publickey_auto(m_session, nullptr, nullptr)))
                return;
            break;
        case AuthStage::StoredPassword: {
            m_stage = AuthStage::KeyboardInteractive;
            if (m_settings.password.isEmpty() || !(m_authMethods & SSH_AUTH_METHOD_PASSWORD))
                break;
            m_storedPasswordTried = true;
            auto secret = m_settings.password.toUtf8();
            const int result = ssh_userauth_password(m_session, nullptr, secret.constData());
            wipe(secret);
            if (handleAuthResult(result))
                return;
            if (result == SSH_AUTH_DENIED)
                emit statusMessage(QStringLiteral("The stored password was rejected."));
            break;
        }
        case AuthStage::KeyboardInteractive:
            if ((m_authMethods & SSH_AUTH_METHOD_INTERACTIVE) && runKeyboardInteractive())
                return;
            m_stage = AuthStage::PasswordPrompt;
            break;
        case AuthStage::PasswordPrompt:
            if (!(m_authMethods & SSH_AUTH_METHOD_PASSWORD)
                || m_failedAttempts >= MaximumPasswordAttempts) {
                m_stage = AuthStage::Failed;
                break;
            }
            m_pendingInput = PendingInput::Password;
            emit inputRequired(QString(),
                               {QStringLiteral("%1@%2's password: ")
                                    .arg(m_settings.username, m_settings.host)},
                               {false});
            return;
        case AuthStage::Failed:
            finish(QStringLiteral("Authentication failed: access denied."), true);
            return;
        }
    }
}

// Returns true when authentication succeeded or failed fatally; the caller
// stops trying further methods in either case.
bool SshWorker::handleAuthResult(int result)
{
    switch (result) {
    case SSH_AUTH_SUCCESS:
        wipe(m_settings.password);
        openShell();
        return true;
    case SSH_AUTH_PARTIAL:
        m_authMethods = ssh_userauth_list(m_session, nullptr);
        return false;
    case SSH_AUTH_ERROR:
        finish(QStringLiteral("Authentication failed: %1").arg(lastError()), true);
        return true;
    default:
        return false;
    }
}

// Returns true when the exchange is finished (success or fatal error) or is
// waiting for user input, false when this method was denied.
bool SshWorker::runKeyboardInteractive()
{
    bool answeredWithStoredPassword = false;
    for (;;) {
        const int result = ssh_userauth_kbdint(m_session, nullptr, nullptr);
        if (result == SSH_AUTH_INFO) {
            const int count = ssh_userauth_kbdint_getnprompts(m_session);
            QStringList prompts;
            QList<bool> echo;
            for (int index = 0; index < count; ++index) {
                char echoFlag = 0;
                prompts.append(QString::fromUtf8(ssh_userauth_kbdint_getprompt(
                    m_session, static_cast<unsigned int>(index), &echoFlag)));
                echo.append(echoFlag != 0);
            }
            if (count == 0)
                continue;
            if (count == 1 && !echo.first() && !m_settings.password.isEmpty()
                && !m_storedPasswordTried) {
                m_storedPasswordTried = true;
                answeredWithStoredPassword = true;
                auto secret = m_settings.password.toUtf8();
                ssh_userauth_kbdint_setanswer(m_session, 0, secret.constData());
                wipe(secret);
                continue;
            }
            QStringList header;
            for (const char *text : {ssh_userauth_kbdint_getname(m_session),
                                     ssh_userauth_kbdint_getinstruction(m_session)}) {
                const auto line = QString::fromUtf8(text).trimmed();
                if (!line.isEmpty())
                    header.append(line);
            }
            m_pendingInput = PendingInput::KeyboardInteractive;
            emit inputRequired(header.join(QLatin1Char('\n')), prompts, echo);
            return true;
        }
        if (result == SSH_AUTH_DENIED) {
            if (std::exchange(answeredWithStoredPassword, false)) {
                emit statusMessage(QStringLiteral("The stored password was rejected."));
                continue;
            }
            if (++m_failedAttempts < MaximumPasswordAttempts) {
                emit statusMessage(QStringLiteral("Access denied."));
                continue;
            }
            return false;
        }
        return handleAuthResult(result);
    }
}

void SshWorker::answerPrompts(QStringList answers)
{
    const auto pending = std::exchange(m_pendingInput, PendingInput::None);
    if (pending == PendingInput::KeyboardInteractive) {
        for (int index = 0; index < answers.size(); ++index) {
            auto answer = answers[index].toUtf8();
            ssh_userauth_kbdint_setanswer(m_session, static_cast<unsigned int>(index),
                                          answer.constData());
            wipe(answer);
        }
        for (auto &answer : answers)
            wipe(answer);
        if (runKeyboardInteractive())
            return;
        m_stage = AuthStage::PasswordPrompt;
        continueAuthentication();
    } else if (pending == PendingInput::Password) {
        auto secret = answers.value(0).toUtf8();
        for (auto &answer : answers)
            wipe(answer);
        const int result = ssh_userauth_password(m_session, nullptr, secret.constData());
        wipe(secret);
        if (handleAuthResult(result))
            return;
        if (result == SSH_AUTH_DENIED) {
            ++m_failedAttempts;
            emit statusMessage(QStringLiteral("Access denied."));
        }
        continueAuthentication();
    }
}

void SshWorker::cancelPrompts()
{
    if (std::exchange(m_pendingInput, PendingInput::None) != PendingInput::None)
        finish(QStringLiteral("Authentication cancelled."), false);
}

void SshWorker::openShell()
{
    if (m_settings.forwardPort > 0) {
        openForward();
        return;
    }
    m_channel = ssh_channel_new(m_session);
    if (!m_channel || ssh_channel_open_session(m_channel) != SSH_OK) {
        finish(QStringLiteral("Could not open an SSH channel: %1").arg(lastError()), true);
        return;
    }
    const auto terminalType = m_settings.terminalType.toUtf8();
    if (ssh_channel_request_pty_size(m_channel, terminalType.constData(),
                                     m_settings.columns, m_settings.rows) != SSH_OK) {
        finish(QStringLiteral("The server refused a terminal: %1").arg(lastError()), true);
        return;
    }
    if (ssh_channel_request_shell(m_channel) != SSH_OK) {
        finish(QStringLiteral("The server refused a shell: %1").arg(lastError()), true);
        return;
    }

    m_notifier = new QSocketNotifier(ssh_get_fd(m_session), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &SshWorker::readAvailable);
    startKeepalive();
    emit connected();
    readAvailable();
    if (!m_finished)
        initializeSftp();
}

void SshWorker::startKeepalive()
{
    if (m_settings.keepaliveSeconds <= 0)
        return;
    // Keeps NAT routers and firewalls from dropping an idle connection.
    auto *keepalive = new QTimer(this);
    connect(keepalive, &QTimer::timeout, this, [this] {
        if (m_session && !m_finished && ssh_send_ignore(m_session, "keepalive") != SSH_OK)
            finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
    });
    keepalive->start(m_settings.keepaliveSeconds * 1000);
}

// Tunnel mode: listen on a local port only this computer can reach, and
// carry each connection to it through its own SSH channel.
void SshWorker::openForward()
{
    m_forwardServer = new QTcpServer(this);
    if (!m_forwardServer->listen(QHostAddress::LocalHost, 0)) {
        finish(QStringLiteral("Could not open a local port for the SSH tunnel: %1")
                   .arg(m_forwardServer->errorString()),
               true);
        return;
    }
    connect(m_forwardServer, &QTcpServer::newConnection, this, [this] {
        while (auto *socket = m_forwardServer->nextPendingConnection()) {
            const auto host = m_settings.forwardHost.toUtf8();
            ssh_channel channel = ssh_channel_new(m_session);
            if (!channel
                || ssh_channel_open_forward(channel, host.constData(), m_settings.forwardPort,
                                            "127.0.0.1", socket->peerPort())
                       != SSH_OK) {
                emit statusMessage(QStringLiteral("The SSH server could not reach %1:%2: %3")
                                       .arg(m_settings.forwardHost)
                                       .arg(m_settings.forwardPort)
                                       .arg(lastError()));
                if (channel)
                    ssh_channel_free(channel);
                socket->abort();
                socket->deleteLater();
                continue;
            }
            m_forwards.append({socket, channel});
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { forwardFromSocket(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] { closeForward(socket); });
            forwardFromSocket(socket);
        }
    });
    m_notifier = new QSocketNotifier(ssh_get_fd(m_session), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &SshWorker::readAvailable);
    startKeepalive();
    emit forwardReady(m_forwardServer->serverPort());
}

void SshWorker::forwardFromSocket(QTcpSocket *socket)
{
    for (const auto &forward : std::as_const(m_forwards)) {
        if (forward.socket != socket)
            continue;
        const QByteArray data = socket->readAll();
        if (!data.isEmpty()
            && ssh_channel_write(forward.channel, data.constData(), uint32_t(data.size())) == SSH_ERROR) {
            finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
            return;
        }
        break;
    }
    pumpForwards();
}

void SshWorker::pumpForwards()
{
    char buffer[65536];
    QList<QTcpSocket *> ended;
    for (const auto &forward : std::as_const(m_forwards)) {
        for (;;) {
            const int count = ssh_channel_read_nonblocking(forward.channel, buffer, sizeof buffer, 0);
            if (count == SSH_ERROR) {
                finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
                return;
            }
            if (count <= 0)
                break;
            forward.socket->write(buffer, count);
        }
        if (ssh_channel_is_eof(forward.channel) || ssh_channel_is_closed(forward.channel))
            ended.append(forward.socket);
    }
    for (auto *socket : std::as_const(ended)) {
        socket->flush();
        closeForward(socket);
    }
}

void SshWorker::closeForward(QTcpSocket *socket)
{
    for (qsizetype i = 0; i < m_forwards.size(); ++i) {
        if (m_forwards[i].socket != socket)
            continue;
        const auto forward = m_forwards.takeAt(i);
        if (ssh_channel_is_open(forward.channel))
            ssh_channel_close(forward.channel);
        ssh_channel_free(forward.channel);
        forward.socket->disconnect(this);
        forward.socket->disconnectFromHost();
        forward.socket->deleteLater();
        return;
    }
}

void SshWorker::readAvailable()
{
    if (m_forwardServer) {
        pumpForwards();
        return;
    }
    if (!m_channel)
        return;

    QByteArray output;
    char buffer[16384];
    for (const int stream : {0, 1}) {
        for (;;) {
            const int count = ssh_channel_read_nonblocking(m_channel, buffer, sizeof buffer,
                                                           stream);
            if (count == SSH_ERROR) {
                if (!output.isEmpty())
                    emit dataReceived(output);
                finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
                return;
            }
            if (count <= 0)
                break;
            output.append(buffer, count);
            if (output.size() >= MaximumReadBatch)
                emit dataReceived(std::exchange(output, QByteArray()));
        }
    }
    if (!output.isEmpty())
        emit dataReceived(output);

    if (ssh_channel_is_eof(m_channel) || ssh_channel_is_closed(m_channel)) {
        uint32_t exitCode = 0;
        char *exitSignal = nullptr;
        int coreDumped = 0;
        QString reason = QStringLiteral("Session closed.");
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 11, 0)
        if (ssh_channel_get_exit_state(m_channel, &exitCode, &exitSignal, &coreDumped) == SSH_OK) {
            reason = exitSignal
                ? QStringLiteral("Session closed by signal %1.").arg(QString::fromUtf8(exitSignal))
                : QStringLiteral("Session closed (exit status %1).").arg(exitCode);
        }
        ssh_string_free_char(exitSignal);
#else
        // libssh 0.10 reports only the exit code, not a terminating signal.
        Q_UNUSED(exitSignal);
        Q_UNUSED(coreDumped);
        if (const int status = ssh_channel_get_exit_status(m_channel); status >= 0) {
            exitCode = static_cast<uint32_t>(status);
            reason = QStringLiteral("Session closed (exit status %1).").arg(exitCode);
        }
#endif
        finish(reason, false);
    }
}

void SshWorker::write(const QByteArray &data)
{
    if (!m_channel || m_finished)
        return;
    if (ssh_channel_write(m_channel, data.constData(), static_cast<uint32_t>(data.size()))
        == SSH_ERROR) {
        finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
        return;
    }
    // Writing can process incoming packets into libssh's buffers without the
    // socket becoming readable again, so drain them now.
    readAvailable();
}

void SshWorker::resize(int columns, int rows)
{
    m_settings.columns = columns;
    m_settings.rows = rows;
    if (m_channel && !m_finished)
        ssh_channel_change_pty_size(m_channel, columns, rows);
}

void SshWorker::finish(const QString &reason, bool error)
{
    if (std::exchange(m_finished, true))
        return;
    closeSession();
    emit disconnected(reason, error);
}

void SshWorker::closeSession()
{
    delete m_notifier;
    m_notifier = nullptr;
    while (!m_forwards.isEmpty())
        closeForward(m_forwards.constFirst().socket);
    delete m_forwardServer;
    m_forwardServer = nullptr;
    closeTransferFiles(false);
    m_job.reset();
    m_jobQueue.clear();
    if (m_sftp) {
        sftp_free(m_sftp);
        m_sftp = nullptr;
    }
    if (m_channel) {
        if (ssh_channel_is_open(m_channel))
            ssh_channel_close(m_channel);
        ssh_channel_free(m_channel);
        m_channel = nullptr;
    }
    if (m_session) {
        ssh_disconnect(m_session);
        ssh_free(m_session);
        m_session = nullptr;
    }
    wipe(m_settings.password);
}

QString SshWorker::lastError() const
{
    return m_session ? QString::fromUtf8(ssh_get_error(m_session)) : QString();
}

void SshWorker::initializeSftp()
{
    ssh_channel channel = ssh_channel_new(m_session);
    if (!channel || ssh_channel_open_session(channel) != SSH_OK
        || ssh_channel_request_sftp(channel) != SSH_OK) {
        const auto error = lastError();
        if (channel)
            ssh_channel_free(channel);
        emit sftpUnavailable(QStringLiteral("The server does not provide SFTP: %1").arg(error));
        readAvailable();
        return;
    }

    // sftp-server sends nothing before the client's first packet, so anything
    // arriving now was printed by the server's login scripts (for example an
    // echo or ssh-agent in ~/.bashrc). Left in the stream it corrupts the SFTP
    // protocol, so discard it once the channel has been quiet briefly.
    QByteArray loginOutput;
    QElapsedTimer total;
    QElapsedTimer quiet;
    total.start();
    quiet.start();
    while (quiet.elapsed() < SftpQuietPeriodMs && total.elapsed() < SftpMaximumDrainMs) {
        bool received = false;
        for (const int stream : {0, 1}) {
            const int available = ssh_channel_poll_timeout(channel, stream == 0 ? 50 : 0, stream);
            if (available <= 0)
                continue;
            QByteArray chunk(available, Qt::Uninitialized);
            const int count = ssh_channel_read_nonblocking(channel, chunk.data(),
                                                           static_cast<uint32_t>(available),
                                                           stream);
            if (count > 0) {
                if (stream == 0)
                    loginOutput.append(chunk.constData(), count);
                received = true;
            }
        }
        if (received)
            quiet.restart();
        readAvailable();
        if (m_finished) {
            ssh_channel_free(channel);
            return;
        }
    }

    m_sftp = sftp_new_channel(m_session, channel);
    if (!m_sftp) {
        ssh_channel_free(channel);
    } else if (sftp_init(m_sftp) != SSH_OK) {
        sftp_free(m_sftp);
        m_sftp = nullptr;
    }
    if (!m_sftp) {
        const auto error = lastError();
        // Output that arrives after the quiet period still breaks the
        // protocol; libssh then reports an invalid packet size.
        const bool scriptOutput = !loginOutput.isEmpty()
            || error.contains(QStringLiteral("packet size"), Qt::CaseInsensitive);
        emit sftpUnavailable(
            scriptOutput
                ? QStringLiteral("SFTP could not start because the server prints text in "
                                 "non-interactive sessions%1. Make that output conditional on "
                                 "an interactive shell, for example by wrapping it in "
                                 "\"if [[ $- == *i* ]]; then … fi\" in ~/.bashrc.")
                      .arg(loginOutput.isEmpty()
                               ? QString()
                               : QStringLiteral(" (\"%1\")")
                                     .arg(QString::fromUtf8(loginOutput).trimmed()
                                              .section(QLatin1Char('\n'), 0, 0)))
                : QStringLiteral("The server does not provide SFTP: %1").arg(error));
        readAvailable();
        return;
    }

#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 11, 0)
    // libssh 0.10 has no limits query; it keeps the conservative default.
    if (sftp_limits_t limits = sftp_limits(m_sftp)) {
        const auto clampChunk = [](uint64_t length) {
            return length == 0 ? DefaultSftpChunk
                               : static_cast<size_t>(std::clamp<uint64_t>(length, 4096,
                                                                          MaximumSftpChunk));
        };
        m_readChunk = clampChunk(limits->max_read_length);
        m_writeChunk = clampChunk(limits->max_write_length);
        sftp_limits_free(limits);
    }
#endif

    QString homePath = QStringLiteral("/");
    if (char *home = sftp_canonicalize_path(m_sftp, ".")) {
        homePath = QString::fromUtf8(home);
        ssh_string_free_char(home);
    }
    emit sftpReady(homePath);
    readAvailable();
}

QString SshWorker::sftpError(const QString &action, const QString &path) const
{
    QString reason;
    switch (m_sftp ? sftp_get_error(m_sftp) : SSH_FX_NO_CONNECTION) {
    case SSH_FX_NO_SUCH_FILE:
    case SSH_FX_NO_SUCH_PATH:
        reason = QStringLiteral("no such file or folder");
        break;
    case SSH_FX_PERMISSION_DENIED:
        reason = QStringLiteral("permission denied");
        break;
    case SSH_FX_FILE_ALREADY_EXISTS:
        reason = QStringLiteral("it already exists");
        break;
    case SSH_FX_WRITE_PROTECT:
        reason = QStringLiteral("the file system is read-only");
        break;
    case SSH_FX_NO_CONNECTION:
    case SSH_FX_CONNECTION_LOST:
        reason = QStringLiteral("the connection was lost");
        break;
    default:
        reason = lastError();
        if (reason.isEmpty())
            reason = QStringLiteral("the server reported a failure");
        break;
    }
    return QStringLiteral("Could not %1 %2: %3").arg(action, path, reason);
}

void SshWorker::listDirectory(const QString &path)
{
    if (!m_sftp) {
        emit directoryListed(path, {}, QStringLiteral("SFTP is not available."));
        return;
    }

    QString canonicalPath = path;
    if (char *canonical = sftp_canonicalize_path(m_sftp, path.toUtf8().constData())) {
        canonicalPath = QString::fromUtf8(canonical);
        ssh_string_free_char(canonical);
    } else {
        emit directoryListed(path, {}, sftpError(QStringLiteral("open"), path));
        readAvailable();
        return;
    }

    sftp_dir directory = sftp_opendir(m_sftp, canonicalPath.toUtf8().constData());
    if (!directory) {
        emit directoryListed(canonicalPath, {}, sftpError(QStringLiteral("open"), canonicalPath));
        readAvailable();
        return;
    }

    QList<SftpEntry> entries;
    while (sftp_attributes attributes = sftp_readdir(m_sftp, directory)) {
        SftpEntry entry;
        entry.name = QString::fromUtf8(attributes->name);
        if (entry.name != QStringLiteral(".") && entry.name != QStringLiteral("..")) {
            entry.isDirectory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
            entry.isSymlink = attributes->type == SSH_FILEXFER_TYPE_SYMLINK;
            entry.size = attributes->size;
            entry.modified = fromUnixTime(attributes->mtime);
            entry.permissions = attributes->permissions;
            if (entry.isSymlink) {
                // Show links to folders as folders so they can be opened.
                const auto target = joinRemotePath(canonicalPath, entry.name).toUtf8();
                if (sftp_attributes resolved = sftp_stat(m_sftp, target.constData())) {
                    entry.isDirectory = resolved->type == SSH_FILEXFER_TYPE_DIRECTORY;
                    entry.size = resolved->size;
                    sftp_attributes_free(resolved);
                }
            }
            entries.append(entry);
        }
        sftp_attributes_free(attributes);
    }
    const bool complete = sftp_dir_eof(directory);
    sftp_closedir(directory);
    emit directoryListed(canonicalPath, entries,
                         complete ? QString() : sftpError(QStringLiteral("list"), canonicalPath));
    readAvailable();
}

void SshWorker::makeDirectory(const QString &path)
{
    QString error;
    if (!m_sftp)
        error = QStringLiteral("SFTP is not available.");
    else if (sftp_mkdir(m_sftp, path.toUtf8().constData(), 0755) < 0)
        error = sftpError(QStringLiteral("create folder"), path);
    emit fileOperationFinished(error);
    readAvailable();
}

void SshWorker::renamePath(const QString &from, const QString &to)
{
    QString error;
    if (!m_sftp)
        error = QStringLiteral("SFTP is not available.");
    else if (sftp_rename(m_sftp, from.toUtf8().constData(), to.toUtf8().constData()) < 0)
        error = sftpError(QStringLiteral("rename"), from);
    emit fileOperationFinished(error);
    readAvailable();
}

void SshWorker::removePaths(const QStringList &paths)
{
    QString error;
    if (!m_sftp)
        error = QStringLiteral("SFTP is not available.");
    for (const auto &path : paths) {
        if (!error.isEmpty() || !removeRecursively(path, 0, &error))
            break;
    }
    emit fileOperationFinished(error);
    readAvailable();
}

bool SshWorker::removeRecursively(const QString &path, int depth, QString *error)
{
    const auto pathBytes = path.toUtf8();
    sftp_attributes attributes = sftp_lstat(m_sftp, pathBytes.constData());
    if (!attributes) {
        *error = sftpError(QStringLiteral("delete"), path);
        return false;
    }
    const bool isDirectory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
    sftp_attributes_free(attributes);

    if (!isDirectory) {
        if (sftp_unlink(m_sftp, pathBytes.constData()) < 0) {
            *error = sftpError(QStringLiteral("delete"), path);
            return false;
        }
        return true;
    }
    if (depth > MaximumTreeDepth) {
        *error = QStringLiteral("Could not delete %1: folders are nested too deeply.").arg(path);
        return false;
    }

    // Collect names first so only one directory handle is open at a time.
    QStringList children;
    sftp_dir directory = sftp_opendir(m_sftp, pathBytes.constData());
    if (!directory) {
        *error = sftpError(QStringLiteral("open"), path);
        return false;
    }
    while (sftp_attributes child = sftp_readdir(m_sftp, directory)) {
        const auto name = QString::fromUtf8(child->name);
        if (name != QStringLiteral(".") && name != QStringLiteral(".."))
            children.append(joinRemotePath(path, name));
        sftp_attributes_free(child);
    }
    sftp_closedir(directory);

    for (const auto &child : children) {
        if (!removeRecursively(child, depth + 1, error))
            return false;
    }
    if (sftp_rmdir(m_sftp, pathBytes.constData()) < 0) {
        *error = sftpError(QStringLiteral("delete"), path);
        return false;
    }
    return true;
}

void SshWorker::startTransfer(int jobId, bool upload, const QStringList &sources,
                              const QString &destination)
{
    if (!m_sftp) {
        emit transferFinished(jobId, upload, destination, 0, 0,
                              {QStringLiteral("SFTP is not available.")}, false);
        return;
    }
    m_jobQueue.append({jobId, upload, sources, destination});
    startNextJob();
}

void SshWorker::startNextJob()
{
    if (m_job || m_jobQueue.isEmpty() || m_finished)
        return;

    m_job = m_jobQueue.takeFirst();
    m_tasks.clear();
    m_taskIndex = 0;
    m_jobBytesTotal = 0;
    m_jobBytesDone = 0;
    m_jobFiles = 0;
    m_filesDone = 0;
    m_filesTransferred = 0;
    m_skipped = 0;
    m_transferErrors.clear();
    m_conflictPolicy.reset();
    m_taskResolution.reset();
    m_cancelRequested = false;

    QString error;
    const bool planned = m_job->upload ? planUpload(*m_job, &error) : planDownload(*m_job, &error);
    if (!planned)
        m_transferErrors.append(error);
    for (const auto &task : std::as_const(m_tasks)) {
        if (!task.isDirectory) {
            ++m_jobFiles;
            m_jobBytesTotal += task.size;
        }
    }
    m_progressTimer.invalidate();
    emitProgress(true);
    scheduleTransferStep();
}

bool SshWorker::planUpload(const TransferJob &job, QString *error)
{
    for (const auto &source : job.sources) {
        const QFileInfo info(source);
        if (!info.exists()) {
            *error = QStringLiteral("%1 no longer exists.").arg(source);
            return false;
        }
        const auto destination = joinRemotePath(job.destination, info.fileName());
        if (info.isFile()) {
            m_tasks.append({false, info.absoluteFilePath(), destination,
                            static_cast<quint64>(info.size()), info.lastModified(),
                            info.isExecutable()});
            continue;
        }
        if (!info.isDir())
            continue;

        m_tasks.append({true, info.absoluteFilePath(), destination});
        const QDir root(info.absoluteFilePath());
        QDirIterator iterator(info.absoluteFilePath(),
                              QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                  | QDir::System,
                              QDirIterator::Subdirectories);
        // A folder is always listed before its contents, so remote parents
        // are created first.
        while (iterator.hasNext()) {
            iterator.next();
            const auto child = iterator.fileInfo();
            const auto remote = joinRemotePath(destination,
                                               root.relativeFilePath(child.absoluteFilePath()));
            if (child.isSymLink() && child.isDir())
                continue;
            if (m_tasks.size() >= MaximumPlannedEntries) {
                *error = QStringLiteral("%1 has more than %2 entries; upload smaller parts of it.")
                             .arg(source).arg(MaximumPlannedEntries);
                return false;
            }
            if (child.isDir())
                m_tasks.append({true, child.absoluteFilePath(), remote});
            else if (child.isFile())
                m_tasks.append({false, child.absoluteFilePath(), remote,
                                static_cast<quint64>(child.size()), child.lastModified(),
                                child.isExecutable()});
        }
    }
    return true;
}

bool SshWorker::planDownload(const TransferJob &job, QString *error)
{
    for (const auto &source : job.sources) {
        const auto name = remoteFileName(source);
        const auto target = QDir(job.destination).filePath(name);
        if (!isSafeLocalName(name) || !isInside(target, job.destination)) {
            *error = QStringLiteral("%1 cannot be saved here: its name is not valid on this "
                                    "computer.").arg(source);
            return false;
        }
        if (!planRemoteTree(source, target, 0, error))
            return false;
    }
    return true;
}

bool SshWorker::planRemoteTree(const QString &remotePath, const QString &localPath, int depth,
                               QString *error)
{
    const auto pathBytes = remotePath.toUtf8();
    sftp_attributes attributes = sftp_stat(m_sftp, pathBytes.constData());
    if (!attributes) {
        *error = sftpError(QStringLiteral("read"), remotePath);
        return false;
    }
    const auto type = attributes->type;
    TransferTask task{type == SSH_FILEXFER_TYPE_DIRECTORY, remotePath, localPath,
                      attributes->size, fromUnixTime(attributes->mtime),
                      (attributes->permissions & 0111) != 0};
    sftp_attributes_free(attributes);

    if (type == SSH_FILEXFER_TYPE_REGULAR) {
        m_tasks.append(task);
        return true;
    }
    if (type != SSH_FILEXFER_TYPE_DIRECTORY)
        return true;
    if (depth > MaximumTreeDepth) {
        *error = QStringLiteral("%1 is nested too deeply.").arg(remotePath);
        return false;
    }

    m_tasks.append(task);
    QStringList children;
    sftp_dir directory = sftp_opendir(m_sftp, pathBytes.constData());
    if (!directory) {
        *error = sftpError(QStringLiteral("open"), remotePath);
        return false;
    }
    while (sftp_attributes child = sftp_readdir(m_sftp, directory)) {
        const auto name = QString::fromUtf8(child->name);
        if (name != QStringLiteral(".") && name != QStringLiteral(".."))
            children.append(name);
        sftp_attributes_free(child);
    }
    sftp_closedir(directory);
    for (const auto &name : children) {
        // A server can return any entry name; one that would leave the
        // download folder (for example "..\\x" on Windows) is skipped.
        const auto target = QDir(localPath).filePath(name);
        if (!isSafeLocalName(name) || !isInside(target, localPath)) {
            m_transferErrors.append(QStringLiteral("Skipped %1: its name is not valid on this "
                                                   "computer.").arg(joinRemotePath(remotePath, name)));
            continue;
        }
        if (m_tasks.size() >= MaximumPlannedEntries) {
            *error = QStringLiteral("%1 has more than %2 entries; download smaller parts of it.")
                         .arg(remotePath).arg(MaximumPlannedEntries);
            return false;
        }
        if (!planRemoteTree(joinRemotePath(remotePath, name), target, depth + 1, error))
            return false;
    }
    return true;
}

void SshWorker::scheduleTransferStep()
{
    if (std::exchange(m_stepScheduled, true))
        return;
    // Each chunk returns to the event loop so terminal I/O keeps flowing
    // while a transfer runs on the same connection.
    QTimer::singleShot(0, this, [this] {
        m_stepScheduled = false;
        transferStep();
    });
}

void SshWorker::transferStep()
{
    if (!m_job || m_awaitingConflict || !m_sftp)
        return;
    if (m_cancelRequested) {
        finishJob(true);
        return;
    }

    if (!m_remoteFile) {
        if (m_taskIndex >= m_tasks.size()) {
            finishJob(false);
            return;
        }
        switch (beginTask()) {
        case TaskStart::Waiting:
            return;
        case TaskStart::Finished:
            ++m_taskIndex;
            scheduleTransferStep();
            return;
        case TaskStart::Opened:
            break;
        }
    }

    qint64 transferred = 0;
    if (m_job->upload) {
        const auto chunk = m_localFile->read(static_cast<qint64>(m_writeChunk));
        if (chunk.isEmpty()) {
            if (m_localFile->atEnd())
                completeFile();
            else
                failFile(QStringLiteral("Could not read %1: %2")
                             .arg(m_tasks[m_taskIndex].source, m_localFile->errorString()));
            return;
        }
        for (qsizetype offset = 0; offset < chunk.size();) {
            const auto written = sftp_write(m_remoteFile, chunk.constData() + offset,
                                            static_cast<size_t>(chunk.size() - offset));
            if (written <= 0) {
                failFile(sftpError(QStringLiteral("write"), m_tasks[m_taskIndex].destination));
                return;
            }
            offset += written;
        }
        transferred = chunk.size();
    } else {
        QByteArray chunk(static_cast<qsizetype>(m_readChunk), Qt::Uninitialized);
        const auto count = sftp_read(m_remoteFile, chunk.data(), m_readChunk);
        if (count < 0) {
            failFile(sftpError(QStringLiteral("read"), m_tasks[m_taskIndex].source));
            return;
        }
        if (count == 0) {
            completeFile();
            return;
        }
        if (m_localFile->write(chunk.constData(), count) != count) {
            failFile(QStringLiteral("Could not write %1: %2")
                         .arg(m_tasks[m_taskIndex].destination, m_localFile->errorString()));
            return;
        }
        transferred = count;
    }

    m_taskBytesDone += static_cast<quint64>(transferred);
    m_jobBytesDone += static_cast<quint64>(transferred);
    emitProgress(false);
    // SFTP replies are read synchronously and may leave shell output
    // buffered inside libssh without the socket becoming readable again.
    readAvailable();
    scheduleTransferStep();
}

SshWorker::TaskStart SshWorker::beginTask()
{
    auto &task = m_tasks[m_taskIndex];
    if (task.isDirectory) {
        if (m_job->upload) {
            const auto path = task.destination.toUtf8();
            if (sftp_mkdir(m_sftp, path.constData(), 0755) < 0) {
                const auto error = sftpError(QStringLiteral("create folder"), task.destination);
                sftp_attributes existing = sftp_stat(m_sftp, path.constData());
                // An existing folder is merged into.
                if (!existing || existing->type != SSH_FILEXFER_TYPE_DIRECTORY)
                    m_transferErrors.append(error);
                sftp_attributes_free(existing);
            }
        } else if (!QDir().mkpath(task.destination)) {
            m_transferErrors.append(QStringLiteral("Could not create folder %1.")
                                        .arg(task.destination));
        }
        return TaskStart::Finished;
    }

    quint64 existingSize = 0;
    QDateTime existingModified;
    if (!m_taskResolution && destinationExists(task.destination, &existingSize, &existingModified)) {
        if (!m_conflictPolicy) {
            m_awaitingConflict = true;
            emit transferConflict(task.destination, existingSize, existingModified, task.size,
                                  task.modified, m_job->upload);
            return TaskStart::Waiting;
        }
        m_taskResolution = m_conflictPolicy;
    }
    if (const auto resolution = std::exchange(m_taskResolution, std::nullopt)) {
        if (*resolution == SftpConflictResolution::Skip) {
            ++m_skipped;
            ++m_filesDone;
            m_jobBytesDone += task.size;
            emitProgress(true);
            return TaskStart::Finished;
        }
        if (*resolution == SftpConflictResolution::Rename)
            task.destination = uniqueDestination(task.destination);
    }

    m_taskBytesDone = 0;
    emitProgress(true);
    if (m_job->upload) {
        auto file = std::make_unique<QFile>(task.source);
        if (!file->open(QIODevice::ReadOnly)) {
            failFile(QStringLiteral("Could not read %1: %2").arg(task.source, file->errorString()));
            return TaskStart::Waiting;
        }
        m_localFile = std::move(file);
        m_remoteFile = sftp_open(m_sftp, task.destination.toUtf8().constData(),
                                 O_WRONLY | O_CREAT | O_TRUNC, task.executable ? 0755 : 0644);
        if (!m_remoteFile) {
            failFile(sftpError(QStringLiteral("create"), task.destination));
            return TaskStart::Waiting;
        }
        m_remoteFileWritten = true;
    } else {
        m_remoteFile = sftp_open(m_sftp, task.source.toUtf8().constData(), O_RDONLY, 0);
        if (!m_remoteFile) {
            failFile(sftpError(QStringLiteral("open"), task.source));
            return TaskStart::Waiting;
        }
        // QSaveFile keeps an existing local file intact until the download
        // completes.
        auto file = std::make_unique<QSaveFile>(task.destination);
        if (!file->open(QIODevice::WriteOnly)) {
            failFile(QStringLiteral("Could not write %1: %2")
                         .arg(task.destination, file->errorString()));
            return TaskStart::Waiting;
        }
        m_localFile = std::move(file);
    }
    return TaskStart::Opened;
}

bool SshWorker::destinationExists(const QString &path, quint64 *size, QDateTime *modified)
{
    if (!m_job->upload) {
        const QFileInfo info(path);
        if (!info.exists())
            return false;
        *size = static_cast<quint64>(info.size());
        *modified = info.lastModified();
        return true;
    }
    sftp_attributes attributes = sftp_stat(m_sftp, path.toUtf8().constData());
    if (!attributes)
        return false;
    *size = attributes->size;
    *modified = fromUnixTime(attributes->mtime);
    sftp_attributes_free(attributes);
    return true;
}

// Returns "name (n).ext" for the first n that does not exist yet.
QString SshWorker::uniqueDestination(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const auto directory = path.left(slash + 1);
    const auto name = path.mid(slash + 1);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    const auto stem = dot > 0 ? name.left(dot) : name;
    const auto suffix = dot > 0 ? name.mid(dot) : QString();
    quint64 size = 0;
    QDateTime modified;
    for (int counter = 1;; ++counter) {
        const auto candidate = QStringLiteral("%1%2 (%3)%4").arg(directory, stem).arg(counter)
                                   .arg(suffix);
        if (!destinationExists(candidate, &size, &modified))
            return candidate;
    }
}

void SshWorker::completeFile()
{
    auto &task = m_tasks[m_taskIndex];
    if (!closeTransferFiles(true)) {
        failFile(QStringLiteral("Could not save %1.").arg(task.destination));
        return;
    }
    if (task.modified.isValid()) {
        if (m_job->upload) {
            const auto seconds = task.modified.toSecsSinceEpoch();
            const struct timeval times[2] = {{static_cast<decltype(timeval::tv_sec)>(seconds), 0},
                                             {static_cast<decltype(timeval::tv_sec)>(seconds), 0}};
            sftp_utimes(m_sftp, task.destination.toUtf8().constData(), times);
        } else {
            QFile file(task.destination);
            if (file.open(QIODevice::Append))
                file.setFileTime(task.modified, QFileDevice::FileModificationTime);
            if (task.executable)
                file.setPermissions(file.permissions() | QFileDevice::ExeOwner
                                    | QFileDevice::ExeGroup | QFileDevice::ExeOther);
        }
    }
    ++m_filesTransferred;
    ++m_filesDone;
    ++m_taskIndex;
    emitProgress(true);
    readAvailable();
    scheduleTransferStep();
}

void SshWorker::failFile(const QString &error)
{
    closeTransferFiles(false);
    const auto &task = m_tasks[m_taskIndex];
    m_jobBytesDone += task.size > m_taskBytesDone ? task.size - m_taskBytesDone : 0;
    m_transferErrors.append(error);
    ++m_filesDone;
    ++m_taskIndex;
    emitProgress(true);
    scheduleTransferStep();
}

// Closes the current file pair. On failure or cancellation the partial
// destination file is removed.
bool SshWorker::closeTransferFiles(bool success)
{
    if (!m_remoteFile && !m_localFile)
        return true;
    bool closed = true;
    if (m_remoteFile) {
        closed = sftp_close(m_remoteFile) == SSH_OK;
        m_remoteFile = nullptr;
    }
    if (m_localFile) {
        if (auto *saveFile = qobject_cast<QSaveFile *>(m_localFile.get())) {
            if (success && closed)
                closed = saveFile->commit();
            else
                saveFile->cancelWriting();
        }
        m_localFile.reset();
    }
    if ((!success || !closed) && std::exchange(m_remoteFileWritten, false) && m_sftp
        && m_taskIndex < m_tasks.size())
        sftp_unlink(m_sftp, m_tasks[m_taskIndex].destination.toUtf8().constData());
    m_remoteFileWritten = false;
    return closed;
}

void SshWorker::finishJob(bool cancelled)
{
    closeTransferFiles(false);
    const auto job = *std::exchange(m_job, std::nullopt);
    m_tasks.clear();
    m_awaitingConflict = false;
    emit transferFinished(job.id, job.upload, job.destination, m_filesTransferred, m_skipped,
                          m_transferErrors, cancelled);
    if (cancelled) {
        for (const auto &queued : std::as_const(m_jobQueue))
            emit transferFinished(queued.id, queued.upload, queued.destination, 0, 0, {}, true);
        m_jobQueue.clear();
    }
    m_cancelRequested = false;
    readAvailable();
    startNextJob();
}

void SshWorker::emitProgress(bool force)
{
    if (!m_job)
        return;
    if (!force && m_progressTimer.isValid() && m_progressTimer.elapsed() < ProgressIntervalMs)
        return;
    m_progressTimer.start();
    const auto current = m_taskIndex < m_tasks.size()
        ? QFileInfo(m_tasks[m_taskIndex].destination).fileName()
        : QString();
    emit transferProgress(m_job->id, current, m_jobBytesDone, m_jobBytesTotal, m_filesDone,
                          m_jobFiles, static_cast<int>(m_jobQueue.size()));
}

void SshWorker::resolveConflict(SftpConflictResolution resolution, bool applyToAll)
{
    if (!std::exchange(m_awaitingConflict, false))
        return;
    if (resolution == SftpConflictResolution::Cancel) {
        m_cancelRequested = true;
    } else {
        if (applyToAll)
            m_conflictPolicy = resolution;
        m_taskResolution = resolution;
    }
    scheduleTransferStep();
}

void SshWorker::cancelTransfers()
{
    if (!m_job)
        return;
    m_cancelRequested = true;
    m_awaitingConflict = false;
    scheduleTransferStep();
}

SshSession::SshSession(QObject *parent)
    : QObject(parent), m_thread(new QThread), m_worker(new SshWorker)
{
    m_thread->setObjectName(QStringLiteral("ssh-session"));
    m_worker->moveToThread(m_thread);
    // The thread and worker outlive this handle until a blocking libssh call
    // (for example a connection attempt) returns; they then delete themselves.
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    {
        QMutexLocker locker(&workerThreadsMutex);
        workerThreads.insert(m_thread);
    }
    // Runs in the worker thread as it ends, after the worker has been
    // deleted (which disconnects the session).
    connect(m_thread, &QThread::finished, m_thread, [thread = m_thread] {
        QMutexLocker locker(&workerThreadsMutex);
        workerThreads.remove(thread);
    }, Qt::DirectConnection);

    connect(m_worker, &SshWorker::statusMessage, this, &SshSession::statusMessage);
    connect(m_worker, &SshWorker::hostKeyVerificationRequired,
            this, &SshSession::hostKeyVerificationRequired);
    connect(m_worker, &SshWorker::inputRequired, this, &SshSession::inputRequired);
    connect(m_worker, &SshWorker::connected, this, &SshSession::connected);
    connect(m_worker, &SshWorker::forwardReady, this, &SshSession::forwardReady);
    connect(m_worker, &SshWorker::dataReceived, this, &SshSession::dataReceived);
    connect(m_worker, &SshWorker::disconnected, this, &SshSession::disconnected);
    connect(m_worker, &SshWorker::sftpReady, this, &SshSession::sftpReady);
    connect(m_worker, &SshWorker::sftpUnavailable, this, &SshSession::sftpUnavailable);
    connect(m_worker, &SshWorker::directoryListed, this, &SshSession::directoryListed);
    connect(m_worker, &SshWorker::fileOperationFinished,
            this, &SshSession::fileOperationFinished);
    connect(m_worker, &SshWorker::transferProgress, this, &SshSession::transferProgress);
    connect(m_worker, &SshWorker::transferConflict, this, &SshSession::transferConflict);
    connect(m_worker, &SshWorker::transferFinished, this, &SshSession::transferFinished);
    m_thread->start();
}

SshSession::~SshSession()
{
    // The worker may still be inside a blocking libssh call. It is not waited
    // for here: stop listening to it, then let its event loop end, which
    // deletes the worker and then the thread in that thread (see the
    // constructor). Calls queued to it after quit() are dropped, never run.
    // shutdownAll() waits for every such thread before the process exits.
    disconnect(m_worker, nullptr, this, nullptr);
    m_thread->quit();
    m_worker = nullptr;
    m_thread = nullptr;
}

bool SshSession::shutdownAll(int timeoutMs)
{
    QList<QThread *> threads;
    {
        QMutexLocker locker(&workerThreadsMutex);
        threads = workerThreads.values();
    }
    // Quitting a worker's event loop deletes the worker in its thread, which
    // closes the channel and disconnects.
    for (auto *thread : std::as_const(threads))
        thread->quit();
    const QDeadlineTimer deadline(timeoutMs);
    bool finished = true;
    for (auto *thread : std::as_const(threads))
        finished = thread->wait(deadline) && finished;
    return finished;
}

void SshSession::connectToHost(const SshConnectionSettings &settings)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, settings] { worker->start(settings); },
                              Qt::QueuedConnection);
}

void SshSession::write(const QByteArray &data)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, data] { worker->write(data); },
                              Qt::QueuedConnection);
}

void SshSession::resizeTerminal(int columns, int rows)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, columns, rows] {
        worker->resize(columns, rows);
    }, Qt::QueuedConnection);
}

void SshSession::answerHostKey(SshHostKeyDecision decision)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, decision] {
        worker->answerHostKey(decision);
    }, Qt::QueuedConnection);
}

void SshSession::answerPrompts(const QStringList &answers)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, answers] {
        worker->answerPrompts(answers);
    }, Qt::QueuedConnection);
}

void SshSession::cancelPrompts()
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker] { worker->cancelPrompts(); },
                              Qt::QueuedConnection);
}

void SshSession::listDirectory(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path] { worker->listDirectory(path); },
                              Qt::QueuedConnection);
}

void SshSession::makeDirectory(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path] { worker->makeDirectory(path); },
                              Qt::QueuedConnection);
}

void SshSession::renamePath(const QString &from, const QString &to)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, from, to] {
        worker->renamePath(from, to);
    }, Qt::QueuedConnection);
}

void SshSession::removePaths(const QStringList &paths)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, paths] { worker->removePaths(paths); },
                              Qt::QueuedConnection);
}

int SshSession::upload(const QStringList &localPaths, const QString &remoteDirectory)
{
    const int jobId = m_nextJobId++;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, jobId, localPaths, remoteDirectory] {
        worker->startTransfer(jobId, true, localPaths, remoteDirectory);
    }, Qt::QueuedConnection);
    return jobId;
}

int SshSession::download(const QStringList &remotePaths, const QString &localDirectory)
{
    const int jobId = m_nextJobId++;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, jobId, remotePaths, localDirectory] {
        worker->startTransfer(jobId, false, remotePaths, localDirectory);
    }, Qt::QueuedConnection);
    return jobId;
}

void SshSession::resolveConflict(SftpConflictResolution resolution, bool applyToAll)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, resolution, applyToAll] {
        worker->resolveConflict(resolution, applyToAll);
    }, Qt::QueuedConnection);
}

void SshSession::cancelTransfers()
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker] { worker->cancelTransfers(); },
                              Qt::QueuedConnection);
}

#include "sshsession.moc"
