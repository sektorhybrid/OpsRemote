// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sshsession.h"
#include "sshfiles.h"

#include "appdirs.h"
#include "pageant.h"

#include <atomic>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QUuid>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QDeadlineTimer>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QRegularExpression>
#include <QTimer>
#include <QHash>
#include <QHostAddress>
#include <QLocalSocket>
#include <QProcess>
#include <QTcpSocket>
#include <QTcpServer>
#include <QtEndian>
#include <QUrl>
#include <QMap>

#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#include <libssh/sftp.h>

#include <fcntl.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

// A SOCKS client's request before its connection opens, and one forwarded
// SSH agent message: larger ones are not genuine and are refused.
constexpr qsizetype MaximumSocksRequest = 1024;
constexpr quint32 MaximumAgentMessage = 256 * 1024;
// An HTTP proxy client's request headers.
constexpr qsizetype MaximumHttpProxyRequest = 16 * 1024;

namespace {
constexpr int MaximumPasswordAttempts = 3;
constexpr long ConnectTimeoutSeconds = 15;
constexpr qsizetype MaximumReadBatch = 256 * 1024;
constexpr qint64 SftpQuietPeriodMs = 250;
constexpr qint64 SftpMaximumDrainMs = 2000;

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

// The request of a SOCKS 4, 4a or 5 client (CONNECT only, no
// authentication), read from buffer (what is used is removed). stage: 0
// before a SOCKS 5 greeting, 1 after it.
// Ends the sending side of a connection (TCP half close): the other side
// reads to its end and may still answer.
void shutdownSending(QTcpSocket *socket)
{
    const qintptr descriptor = socket->socketDescriptor();
    if (descriptor < 0)
        return;
#ifdef _WIN32
    ::shutdown(SOCKET(descriptor), SD_SEND);
#else
    ::shutdown(int(descriptor), SHUT_WR);
#endif
}

struct SocksRequest
{
    enum Result { More, Failed, Ready };
    Result result = More;
    QByteArray reply; // to send at once (greeting answer, or the refusal)
    QString host;
    quint16 port = 0;
    QByteArray success; // to send once connected
    QByteArray failure; // to send when the destination cannot be reached
};

SocksRequest parseSocksRequest(QByteArray &buffer, int *stage)
{
    SocksRequest request;
    const auto fail = [&request](const QByteArray &reply) {
        request.result = SocksRequest::Failed;
        request.reply += reply;
        return request;
    };
    // A SOCKS request is a few hundred bytes at most.
    if (buffer.size() > MaximumSocksRequest)
        return fail({});
    const auto byte = [&buffer](qsizetype at) { return static_cast<quint8>(buffer.at(at)); };
    if (buffer.isEmpty())
        return request;
    if (byte(0) == 5) {
        if (*stage == 0) {
            if (buffer.size() < 2 || buffer.size() < 2 + byte(1))
                return request;
            const QByteArray methods = buffer.mid(2, byte(1));
            buffer.remove(0, 2 + byte(1));
            if (!methods.contains('\0'))
                return fail(QByteArray("\x05\xff", 2));
            request.reply = QByteArray("\x05\x00", 2);
            *stage = 1;
        }
        // VER CMD RSV ATYP ADDR PORT
        if (buffer.size() < 5)
            return request;
        const auto reply = [](char code) {
            return QByteArray("\x05", 1) + code + QByteArray("\x00\x01\0\0\0\0\0\0", 8);
        };
        request.failure = reply(5);
        request.success = reply(0);
        qsizetype end = 0;
        switch (byte(3)) {
        case 1:
            if (buffer.size() < 10)
                return request;
            request.host = QHostAddress(qFromBigEndian<quint32>(buffer.constData() + 4)).toString();
            end = 8;
            break;
        case 3:
            if (buffer.size() < 7 + byte(4))
                return request;
            request.host = QString::fromLatin1(buffer.mid(5, byte(4)));
            end = 5 + byte(4);
            break;
        case 4:
            if (buffer.size() < 22)
                return request;
            request.host = QHostAddress(reinterpret_cast<const quint8 *>(buffer.constData() + 4)).toString();
            end = 20;
            break;
        default:
            return fail(reply(8));
        }
        if (byte(1) != 1)
            return fail(reply(7));
        request.port = qFromBigEndian<quint16>(buffer.constData() + end);
        buffer.remove(0, end + 2);
    } else if (byte(0) == 4) {
        // VER CMD PORT IP USERID\0 [DOMAIN\0] (4a when IP is 0.0.0.x)
        if (buffer.size() < 9)
            return request;
        const qsizetype userEnd = buffer.indexOf('\0', 8);
        if (userEnd < 0)
            return request;
        request.failure = QByteArray("\x00\x5b\0\0\0\0\0\0", 8);
        request.success = QByteArray("\x00\x5a\0\0\0\0\0\0", 8);
        request.port = qFromBigEndian<quint16>(buffer.constData() + 2);
        const quint32 ip = qFromBigEndian<quint32>(buffer.constData() + 4);
        qsizetype end = userEnd + 1;
        if ((ip & 0xffffff00u) == 0 && ip != 0) {
            const qsizetype domainEnd = buffer.indexOf('\0', end);
            if (domainEnd < 0)
                return request;
            request.host = QString::fromLatin1(buffer.mid(end, domainEnd - end));
            end = domainEnd + 1;
        } else {
            request.host = QHostAddress(ip).toString();
        }
        if (byte(1) != 1)
            return fail(request.failure);
        buffer.remove(0, end);
    } else {
        return fail({});
    }
    request.result = request.host.isEmpty() || request.port == 0 ? SocksRequest::Failed
                                                                    : SocksRequest::Ready;
    if (request.result == SocksRequest::Failed)
        request.reply += request.failure;
    return request;
}

class SshWorker final : public QObject
{
    Q_OBJECT

public:
    SshWorker() : m_files(new RemoteFileEngine(this))
    {
        m_files->setUnavailableMessage(QStringLiteral("The file connection is not available."));
    }
    ~SshWorker() override { closeSession(); }
    RemoteFileEngine *files() const { return m_files; }

    void start(const SshConnectionSettings &settings);
    void write(const QByteArray &data);
    void resize(int columns, int rows);
    void answerHostKey(SshHostKeyDecision decision);
    void answerPrompts(QStringList answers);
    void cancelPrompts();

    void queryShellFolder();
    void addForwards(const QList<PortForward> &forwards, int firstIndex);
    void removeForwards(int firstIndex, int count);
    void probe(int id, const QString &host, quint16 port);
    void runCommand(int id, const QByteArray &command, bool terminal, int timeoutMs, int outputLimit);
    // From any thread: ends a command under way (its result says so).
    void cancelCommands() { m_cancelCommands = true; }

signals:
    void statusMessage(const QString &message);
    void hostKeyVerificationRequired(SshHostKeyStatus status, const QString &keyType,
                                     const QString &fingerprint);
    void inputRequired(const QString &instruction, const QStringList &prompts,
                       const QList<bool> &echo);
    void connected();
    void forwardReady(quint16 localPort);
    void forwardStatus(int index, bool listening, const QString &message);
    void forwardConnectionFailed(int index, const QString &message);
    void forwardStats(int index, int connections, qint64 received, qint64 sent);
    void probeFinished(int id, bool reachable, const QString &message, int milliseconds);
    void commandFinished(int id, int exitStatus, const QByteArray &output, bool truncated,
                         const QString &error, int milliseconds);
    void dataReceived(const QByteArray &data);
    void disconnected(const QString &reason, bool error);

    // protocol: SFTP or SCP.
    void sftpReady(const QString &homePath, const QString &protocol);
    void sftpUnavailable(const QString &error);
    void shellFolder(const QString &path);

private:
    enum class AuthStage { PublicKey, AgentKeys, StoredPassword, KeyboardInteractive,
                           PasswordPrompt, Failed };
    enum class PendingInput { None, KeyboardInteractive, Password, KeyPassphrase };
    void initializeFiles();
    // Files over SFTP; false if the server does not offer it (reason in error).
    bool startSftp(QString *error);
    void schedulePump();
    void verifyHostKey();
    void beginAuthentication();
    void continueAuthentication();
    bool handleAuthResult(int result);
    bool runKeyboardInteractive();
    // Logs on with identityFile; false when that did not succeed (or a
    // passphrase prompt is pending, see m_pendingInput).
    bool tryIdentityFile(const QByteArray &passphrase);
    void openShell();
    void startKeepalive();
    void openForward();
    void startPortForwards();
    void startForward(int index);
    void stopForward(int index);
    void acceptForwardConnections(QTcpServer *server, int index);
    void continueHttpProxy(QTcpSocket *socket);
    void continueReverseSocks();
    void emitForwardStats();
    void acceptRemoteForwards();
    struct Forward;
    Forward *forwardFor(QTcpSocket *socket);
    bool openForwardChannel(Forward *forward, const QString &host, quint16 port);
    void continueSocks(QTcpSocket *socket);
    void forwardFromSocket(QTcpSocket *socket);
    void startAgentForwards();
    void pumpAgentForwards();
    void closeAgentForward(ssh_channel channel);
    QPair<QByteArray, int> localX11Cookie();
    void startX11Forwards();
    void pumpX11Forwards();
    void closeX11Forward(ssh_channel channel);
    void pumpForwards();
    void closeForward(QTcpSocket *socket);
    // The local end closed: what it sent last goes on, then the channel closes.
    void endForward(QTcpSocket *socket);
    void readAvailable();
    void finish(const QString &reason, bool error);
    void closeSession();
    QString lastError() const;

    SshConnectionSettings m_settings;
    ssh_session m_session = nullptr;
    ssh_channel m_channel = nullptr;
    // The LC_OPSREMOTE_TAG value given to the shell, if the server took it.
    QByteArray m_shellTag;
    QSocketNotifier *m_notifier = nullptr;
    // One SSH channel per forwarded connection. A SOCKS connection has no
    // channel until its request has been read.
    struct Forward
    {
        QTcpSocket *socket = nullptr;
        ssh_channel channel = nullptr;
        int index = -1; // in m_rules; -1 for the tunnel mode
        // While the request of a SOCKS or HTTP proxy client is read: 0 its
        // greeting, 1 its request; -1 once the connection carries data.
        int socksStage = -1;
        QByteArray pending; // request bytes not yet parsed
        bool http = false; // an HTTP proxy client (else SOCKS)
        // Reverse SOCKS: the request comes from the server over channel; the
        // socket connects here once it is read.
        bool reverse = false;
        // The channel's end was passed on to the socket (its sending side
        // shut down).
        bool eofSent = false;
        // The local side ended: its channel got EOF and waits for the
        // server to close it.
        bool localEnded = false;
    };
    // The forwards by index: the session's own (0, 1, …) and those added
    // later (addForwards), each with its listener here, if it has one.
    QMap<int, PortForward> m_rules;
    QHash<int, QTcpServer *> m_listeners;
    struct Counters
    {
        int connections = 0;
        qint64 received = 0; // from the tunnel's far end
        qint64 sent = 0;
        bool changed = true;
    };
    QHash<int, Counters> m_counters;
    QTimer *m_statsTimer = nullptr;
    // Tunnel mode (jump hosts): the local listener.
    QTcpServer *m_forwardServer = nullptr;
    QList<Forward> m_forwards;
    // Remote forwards: requested server port -> index in m_rules.
    QHash<int, int> m_remotePorts;
    // Agent forwarding: channels the server opened to reach the local agent
    // (new ones wait in m_newAgentChannels until the next read).
    ssh_callbacks_struct m_callbacks{};
    QList<ssh_channel> m_newAgentChannels;
    // To the agent's socket, or with no socket, message by message to Pageant.
    struct AgentForward
    {
        QLocalSocket *socket = nullptr;
        ssh_channel channel = nullptr;
        QByteArray pending; // Pageant: a request not yet complete
    };
    QList<AgentForward> m_agentForwards;
    // X11 forwarding: channels the server opened for X programs, each
    // connected to the local X server.
    QList<ssh_channel> m_newX11Channels;
    struct X11Forward
    {
        QIODevice *socket = nullptr;
        ssh_channel channel = nullptr;
    };
    QList<X11Forward> m_x11Forwards;
    SshHostKeyStatus m_hostKeyStatus = SshHostKeyStatus::Unknown;
    bool m_awaitingHostKey = false;
    AuthStage m_stage = AuthStage::PublicKey;
    PendingInput m_pendingInput = PendingInput::None;
    int m_authMethods = 0;
    int m_failedAttempts = 0;
    int m_passphraseAttempts = 0;
    bool m_storedPasswordTried = false;
    bool m_finished = false;
    std::atomic<bool> m_cancelCommands{false};

    RemoteFileEngine *m_files = nullptr;
    bool m_pumpScheduled = false;
};

// A connected socket to 127.0.0.1:port, or SSH_INVALID_SOCKET.
static socket_t connectToLocalPort(quint16 port)
{
    socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == SSH_INVALID_SOCKET)
        return SSH_INVALID_SOCKET;
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0)
        return socket;
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
    return SSH_INVALID_SOCKET;
}

static void closeSocket(socket_t socket)
{
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

static void setSocketTimeout(socket_t socket, int seconds)
{
#ifdef _WIN32
    const DWORD value = DWORD(seconds) * 1000;
#else
    const timeval value{seconds, 0};
#endif
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&value), sizeof value);
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&value), sizeof value);
}

static bool sendAll(socket_t socket, const QByteArray &data)
{
    for (qsizetype sent = 0; sent < data.size();) {
        const auto count = ::send(socket, data.constData() + sent, int(data.size() - sent), 0);
        if (count <= 0)
            return false;
        sent += count;
    }
    return true;
}

static bool receiveExactly(socket_t socket, char *buffer, int size)
{
    for (int received = 0; received < size;) {
        const auto count = ::recv(socket, buffer + received, size - received, 0);
        if (count <= 0)
            return false;
        received += int(count);
    }
    return true;
}

// A socket connected to host:port through an HTTP (CONNECT) or SOCKS5
// proxy, or SSH_INVALID_SOCKET with the reason in *error. Blocking; runs on
// the worker thread.
static socket_t connectThroughProxy(const ProxySettings &proxy, const QString &host, quint16 port,
                                    int timeoutSeconds, QString *error)
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    const auto proxyHost = proxy.host.toUtf8();
    const auto proxyPort = QByteArray::number(proxy.port);
    if (getaddrinfo(proxyHost.constData(), proxyPort.constData(), &hints, &addresses) != 0) {
        *error = QStringLiteral("Could not find the proxy %1.").arg(proxy.host);
        return SSH_INVALID_SOCKET;
    }
    socket_t socket = SSH_INVALID_SOCKET;
    for (auto *address = addresses; address && socket == SSH_INVALID_SOCKET; address = address->ai_next) {
        socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == SSH_INVALID_SOCKET)
            continue;
        setSocketTimeout(socket, timeoutSeconds);
        if (::connect(socket, address->ai_addr, int(address->ai_addrlen)) != 0) {
            closeSocket(socket);
            socket = SSH_INVALID_SOCKET;
        }
    }
    freeaddrinfo(addresses);
    if (socket == SSH_INVALID_SOCKET) {
        *error = QStringLiteral("Could not connect to the proxy %1:%2.").arg(proxy.host).arg(proxy.port);
        return SSH_INVALID_SOCKET;
    }
    const auto fail = [&](const QString &reason) {
        closeSocket(socket);
        *error = reason;
        return SSH_INVALID_SOCKET;
    };
    const auto target = host.toUtf8();

    if (proxy.type == ProxySettings::Type::Http) {
        const QByteArray authority = (host.contains(QLatin1Char(':')) ? "[" + target + "]" : target)
                                     + ':' + QByteArray::number(port);
        QByteArray request = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n";
        if (!proxy.user.isEmpty())
            request += "Proxy-Authorization: Basic "
                       + (proxy.user + QLatin1Char(':') + proxy.password).toUtf8().toBase64() + "\r\n";
        request += "\r\n";
        if (!sendAll(socket, request))
            return fail(QStringLiteral("The proxy closed the connection."));
        // Byte by byte, so nothing after the reply's headers is consumed.
        QByteArray reply;
        char byte = 0;
        while (!reply.endsWith("\r\n\r\n")) {
            if (reply.size() > 16384 || !receiveExactly(socket, &byte, 1))
                return fail(QStringLiteral("The proxy did not answer."));
            reply.append(byte);
        }
        const QByteArray status = reply.left(reply.indexOf("\r\n"));
        const int code = status.split(' ').value(1).toInt();
        if (code == 407)
            return fail(QStringLiteral("The proxy needs a user name and password (or rejected them)."));
        if (code != 200)
            return fail(QStringLiteral("The proxy refused the connection: %1")
                            .arg(QString::fromLatin1(status)));
    } else {
        const bool withPassword = !proxy.user.isEmpty();
        if (!sendAll(socket, withPassword ? QByteArray("\x05\x02\x00\x02", 4)
                                          : QByteArray("\x05\x01\x00", 3)))
            return fail(QStringLiteral("The proxy closed the connection."));
        char choice[2];
        if (!receiveExactly(socket, choice, 2) || choice[0] != 5)
            return fail(QStringLiteral("The proxy is not a SOCKS5 proxy."));
        if (choice[1] == 2) {
            const auto user = proxy.user.toUtf8().left(255);
            const auto password = proxy.password.toUtf8().left(255);
            QByteArray login("\x01", 1);
            login += char(user.size()) + user + char(password.size()) + password;
            char answer[2];
            if (!sendAll(socket, login) || !receiveExactly(socket, answer, 2) || answer[1] != 0)
                return fail(QStringLiteral("The proxy rejected the user name or password."));
        } else if (choice[1] != 0) {
            return fail(QStringLiteral("The proxy needs a log-on method OpsRemote does not support."));
        }
        QByteArray request("\x05\x01\x00\x03", 4);
        request += char(qMin<qsizetype>(target.size(), 255)) + target.left(255);
        request += char(port >> 8);
        request += char(port & 0xff);
        char head[4];
        if (!sendAll(socket, request) || !receiveExactly(socket, head, 4))
            return fail(QStringLiteral("The proxy closed the connection."));
        if (head[1] != 0)
            return fail(QStringLiteral("The proxy could not reach %1:%2 (SOCKS error %3).")
                            .arg(host).arg(port).arg(int(head[1])));
        // The bound address and port, which are not needed.
        int rest = head[3] == 1 ? 4 + 2 : head[3] == 4 ? 16 + 2 : -1;
        char skip[262];
        if (rest < 0) {
            char length = 0;
            if (!receiveExactly(socket, &length, 1))
                return fail(QStringLiteral("The proxy closed the connection."));
            rest = static_cast<unsigned char>(length) + 2;
        }
        if (!receiveExactly(socket, skip, rest))
            return fail(QStringLiteral("The proxy closed the connection."));
    }
    setSocketTimeout(socket, 0);
    return socket;
}

void SshWorker::start(const SshConnectionSettings &settings)
{
    ensureLibsshInitialized();
    m_settings = settings;
    m_session = ssh_new();
    if (!m_session) {
        finish(QStringLiteral("Could not create an SSH session."), true);
        return;
    }
    if ((m_settings.forwardAgent || m_settings.forwardX11) && m_settings.openShell
        && m_settings.forwardPort == 0) {
        // Called inside libssh's packet handling: only note the channel.
        m_callbacks.userdata = this;
        if (m_settings.forwardAgent)
            m_callbacks.channel_open_request_auth_agent_function =
                [](ssh_session session, void *userdata) -> ssh_channel {
                ssh_channel channel = ssh_channel_new(session);
                if (channel)
                    static_cast<SshWorker *>(userdata)->m_newAgentChannels.append(channel);
                return channel;
            };
        if (m_settings.forwardX11)
            m_callbacks.channel_open_request_x11_function =
                [](ssh_session session, const char *, int, void *userdata) -> ssh_channel {
                ssh_channel channel = ssh_channel_new(session);
                if (channel)
                    static_cast<SshWorker *>(userdata)->m_newX11Channels.append(channel);
                return channel;
            };
        ssh_callbacks_init(&m_callbacks);
        ssh_set_callbacks(m_session, &m_callbacks);
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
    if (m_settings.viaLocalPort == 0 && m_settings.proxy.isSet()) {
        emit statusMessage(QStringLiteral("Connecting through the proxy %1:%2…")
                               .arg(m_settings.proxy.host).arg(m_settings.proxy.port));
        QString error;
        socket_t socket = connectThroughProxy(m_settings.proxy, m_settings.host, m_settings.port,
                                              int(timeout), &error);
        m_settings.proxy.password.fill(QChar(0));
        if (socket == SSH_INVALID_SOCKET || ssh_options_set(m_session, SSH_OPTIONS_FD, &socket) < 0) {
            finish(error.isEmpty() ? QStringLiteral("Could not use the proxy.") : error, true);
            return;
        }
    } else if (m_settings.viaLocalPort > 0) {
        // libssh takes over the socket and closes it with the session.
        socket_t socket = connectToLocalPort(m_settings.viaLocalPort);
        if (socket == SSH_INVALID_SOCKET
            || ssh_options_set(m_session, SSH_OPTIONS_FD, &socket) < 0) {
            finish(QStringLiteral("Could not reach %1 through the jump host.")
                       .arg(m_settings.host),
                   true);
            return;
        }
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
bool SshWorker::tryIdentityFile(const QByteArray &passphrase)
{
    ssh_key key = nullptr;
    const auto path = QDir::toNativeSeparators(m_settings.identityFile).toUtf8();
    const int imported = ssh_pki_import_privkey_file(
        path.constData(), passphrase.isEmpty() ? nullptr : passphrase.constData(), nullptr,
        nullptr, &key);
    if (imported == SSH_OK) {
        const int result = ssh_userauth_publickey(m_session, nullptr, key);
        ssh_key_free(key);
        return handleAuthResult(result);
    }
    if (imported == SSH_EOF) {
        emit statusMessage(QStringLiteral("Could not read the private key %1.")
                               .arg(m_settings.identityFile));
        return false;
    }
    // A key with a passphrase (or a wrong one): ask, a few times.
    if (m_passphraseAttempts >= MaximumPasswordAttempts) {
        emit statusMessage(QStringLiteral("Could not open the private key %1: wrong passphrase, "
                                          "or not a private key.").arg(m_settings.identityFile));
        return false;
    }
    ++m_passphraseAttempts;
    m_pendingInput = PendingInput::KeyPassphrase;
    emit inputRequired(QString(),
                       {QStringLiteral("Passphrase for key %1: ")
                            .arg(QFileInfo(m_settings.identityFile).fileName())},
                       {false});
    return false;
}

void SshWorker::continueAuthentication()
{
    for (;;) {
        switch (m_stage) {
        case AuthStage::PublicKey: {
            m_stage = AuthStage::AgentKeys;
            if (!(m_authMethods & SSH_AUTH_METHOD_PUBLICKEY) || m_settings.identityFile.isEmpty())
                break;
            auto passphrase = m_settings.keyPassphrase.toUtf8();
            m_settings.keyPassphrase.fill(QChar(0));
            m_settings.keyPassphrase.clear();
            const bool done = tryIdentityFile(passphrase);
            wipe(passphrase);
            if (done || m_pendingInput != PendingInput::None)
                return;
            break;
        }
        case AuthStage::AgentKeys:
            m_stage = AuthStage::StoredPassword;
            if ((m_authMethods & SSH_AUTH_METHOD_PUBLICKEY) && m_settings.tryPublicKeys
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
    } else if (pending == PendingInput::KeyPassphrase) {
        auto passphrase = answers.value(0).toUtf8();
        for (auto &answer : answers)
            wipe(answer);
        const bool done = tryIdentityFile(passphrase);
        wipe(passphrase);
        if (!done && m_pendingInput == PendingInput::None)
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
    const auto pending = std::exchange(m_pendingInput, PendingInput::None);
    // Without the key's passphrase, the other ways to log on are still tried.
    if (pending == PendingInput::KeyPassphrase)
        continueAuthentication();
    else if (pending != PendingInput::None)
        finish(QStringLiteral("Authentication cancelled."), false);
}

void SshWorker::openShell()
{
    if (m_settings.forwardPort > 0) {
        openForward();
        return;
    }
    if (!m_settings.openShell) {
        m_notifier = new QSocketNotifier(ssh_get_fd(m_session), QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &SshWorker::readAvailable);
        startKeepalive();
        emit connected();
        startPortForwards();
        if (m_settings.filesOnly && !m_finished)
            initializeFiles();
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
    if (m_settings.forwardAgent && ssh_channel_request_auth_agent(m_channel) != SSH_OK)
        emit statusMessage(QStringLiteral("The server refused SSH agent forwarding."));
    if (m_settings.forwardX11) {
        const auto [cookie, screen] = localX11Cookie();
        if (ssh_channel_request_x11(m_channel, 0, "MIT-MAGIC-COOKIE-1", cookie.constData(), screen)
            != SSH_OK)
            emit statusMessage(QStringLiteral("The server refused X11 forwarding (X11Forwarding "
                                              "in its sshd_config)."));
    }
    // Tag the shell so queryShellFolder() can find it among the user's
    // processes. Servers commonly accept LC_* variables (AcceptEnv); one
    // that does not simply has no folder following. Always, so following can
    // be switched on later from the tab's right-click menu.
    {
        const QByteArray tag = QUuid::createUuid().toByteArray(QUuid::Id128);
        if (ssh_channel_request_env(m_channel, "LC_OPSREMOTE_TAG", tag.constData()) == SSH_OK)
            m_shellTag = tag;
    }
    if (ssh_channel_request_shell(m_channel) != SSH_OK) {
        finish(QStringLiteral("The server refused a shell: %1").arg(lastError()), true);
        return;
    }

    m_notifier = new QSocketNotifier(ssh_get_fd(m_session), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &SshWorker::readAvailable);
    startKeepalive();
    emit connected();
    startPortForwards();
    readAvailable();
    if (!m_finished)
        initializeFiles();
}

void SshWorker::startKeepalive()
{
    if (m_settings.keepaliveSeconds <= 0)
        return;
    // Keeps NAT routers and firewalls from dropping an idle connection.
    auto *keepalive = new QTimer(this);
    connect(keepalive, &QTimer::timeout, this, [this] {
        if (m_session && !m_finished && ssh_send_ignore(m_session, "keepalive") != SSH_OK) {
            finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
            return;
        }
        schedulePump();
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
            if (m_settings.forwardOnce)
                m_forwardServer->close();
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
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] { endForward(socket); });
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
    auto *forward = forwardFor(socket);
    if (!forward)
        return;
    if (forward->socksStage >= 0 && !forward->reverse) {
        if (forward->http)
            continueHttpProxy(socket);
        else
            continueSocks(socket);
        return;
    }
    if (forward->reverse && forward->socksStage >= 0)
        return; // not connected yet
    const QByteArray data = socket->readAll();
    if (!data.isEmpty()
        && ssh_channel_write(forward->channel, data.constData(), uint32_t(data.size())) == SSH_ERROR) {
        finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
        return;
    }
    if (forward->index >= 0 && !data.isEmpty()) {
        auto &counters = m_counters[forward->index];
        counters.sent += data.size();
        counters.changed = true;
    }
    pumpForwards();
}

SshWorker::Forward *SshWorker::forwardFor(QTcpSocket *socket)
{
    for (auto &forward : m_forwards) {
        if (forward.socket == socket)
            return &forward;
    }
    return nullptr;
}

bool SshWorker::openForwardChannel(Forward *forward, const QString &host, quint16 port)
{
    // Opening waits for the server, and libssh keeps what else arrives
    // meanwhile (data, connections to remote forwards) in its own buffers,
    // where the socket notifier does not see it: look again from the event
    // loop.
    schedulePump();
    const auto target = host.toUtf8();
    ssh_channel channel = ssh_channel_new(m_session);
    // Port 0 and a path: a Unix socket on the server.
    const bool unixSocket = port == 0 && host.startsWith(QLatin1Char('/'));
    if (!channel
        || (unixSocket ? ssh_channel_open_forward_unix(channel, target.constData(), "127.0.0.1",
                                                       forward->socket->peerPort())
                       : ssh_channel_open_forward(channel, target.constData(), port, "127.0.0.1",
                                                  forward->socket->peerPort()))
               != SSH_OK) {
        if (channel)
            ssh_channel_free(channel);
        return false;
    }
    forward->channel = channel;
    return true;
}

// Connects channels the server opened for agent forwarding to the local
// SSH agent (SSH_AUTH_SOCK; on Windows, the OpenSSH agent's pipe or Pageant).
void SshWorker::startAgentForwards()
{
    QString agent = QString::fromLocal8Bit(qgetenv("SSH_AUTH_SOCK"));
#ifdef _WIN32
    if (agent.isEmpty())
        agent = QStringLiteral("openssh-ssh-agent");
#endif
    const QString choice = m_settings.agent;
    for (ssh_channel channel : std::exchange(m_newAgentChannels, {})) {
        QLocalSocket *socket = nullptr;
        if (choice != QStringLiteral("pageant") && !agent.isEmpty()) {
            socket = new QLocalSocket(this);
            socket->connectToServer(agent);
            if (!socket->waitForConnected(2000)) {
                delete socket;
                socket = nullptr;
            }
        }
        const bool pageant = !socket && choice != QStringLiteral("openssh") && Pageant::isRunning();
        if (!socket && !pageant) {
            emit statusMessage(QStringLiteral("Agent forwarding: no local SSH agent is running."));
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            continue;
        }
        m_agentForwards.append({socket, channel, {}});
        if (!socket)
            continue;
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, channel] {
            const QByteArray data = socket->readAll();
            if (!data.isEmpty()
                && ssh_channel_write(channel, data.constData(), uint32_t(data.size())) == SSH_ERROR)
                closeAgentForward(channel);
        });
        connect(socket, &QLocalSocket::disconnected, this,
                [this, channel] { closeAgentForward(channel); });
    }
}

void SshWorker::pumpAgentForwards()
{
    char buffer[16384];
    QList<ssh_channel> ended;
    for (auto &forward : m_agentForwards) {
        for (;;) {
            const int count = ssh_channel_read_nonblocking(forward.channel, buffer, sizeof buffer, 0);
            if (count <= 0) {
                if (count == SSH_ERROR)
                    ended.append(forward.channel);
                break;
            }
            if (forward.socket) {
                forward.socket->write(buffer, count);
                continue;
            }
            // Pageant answers whole messages: 4-byte length, then the body.
            forward.pending.append(buffer, count);
            while (forward.pending.size() >= 4) {
                const auto *bytes = reinterpret_cast<const uchar *>(forward.pending.constData());
                const quint32 length = qFromBigEndian<quint32>(bytes);
                if (length > MaximumAgentMessage) {
                    ended.append(forward.channel); // not an agent request
                    break;
                }
                if (forward.pending.size() < qsizetype(length) + 4)
                    break;
                const QByteArray request = forward.pending.left(qsizetype(length) + 4);
                forward.pending.remove(0, qsizetype(length) + 4);
                QByteArray reply = Pageant::query(request);
                if (reply.isEmpty())
                    reply = QByteArray("\0\0\0\x01\x05", 5); // SSH_AGENT_FAILURE
                if (ssh_channel_write(forward.channel, reply.constData(), uint32_t(reply.size()))
                    == SSH_ERROR) {
                    ended.append(forward.channel);
                    break;
                }
            }
        }
        if (ssh_channel_is_eof(forward.channel) || ssh_channel_is_closed(forward.channel))
            ended.append(forward.channel);
    }
    for (ssh_channel channel : std::as_const(ended))
        closeAgentForward(channel);
}

void SshWorker::closeAgentForward(ssh_channel channel)
{
    for (qsizetype i = 0; i < m_agentForwards.size(); ++i) {
        if (m_agentForwards[i].channel != channel)
            continue;
        const auto forward = m_agentForwards.takeAt(i);
        if (ssh_channel_is_open(forward.channel))
            ssh_channel_close(forward.channel);
        ssh_channel_free(forward.channel);
        if (forward.socket) {
            forward.socket->disconnect(this);
            forward.socket->abort();
            forward.socket->deleteLater();
        }
        return;
    }
}

// The local X server: DISPLAY, or :0 (on Windows, VcXsrv or Xming).
static QString localX11Display()
{
    const QString display = QString::fromLocal8Bit(qgetenv("DISPLAY")).trimmed();
    return display.isEmpty() ? QStringLiteral(":0") : display;
}

// The local X server's cookie and screen, sent to the server as they are
// (like ssh -Y). An X server without access control (VcXsrv -ac) has no
// cookie; any one will do then.
QPair<QByteArray, int> SshWorker::localX11Cookie()
{
    const QString display = localX11Display();
    const int screen = display.section(QLatin1Char('.'), 1).toInt();
    QProcess xauth;
    xauth.start(QStringLiteral("xauth"), {QStringLiteral("list"), display});
    if (xauth.waitForFinished(2000)) {
        for (const auto &line : QString::fromLocal8Bit(xauth.readAllStandardOutput()).split(QLatin1Char('\n'))) {
            const auto fields = line.simplified().split(QLatin1Char(' '));
            if (fields.size() == 3 && fields[1] == QStringLiteral("MIT-MAGIC-COOKIE-1"))
                return {fields[2].toLatin1(), screen};
        }
    } else {
        xauth.kill();
        xauth.waitForFinished(500);
    }
    return {QUuid::createUuid().toByteArray(QUuid::Id128), screen};
}

// Connects new X11 channels to the local X server: its socket in
// /tmp/.X11-unix for ":N", TCP port 6000+N otherwise (and on Windows).
void SshWorker::startX11Forwards()
{
    const QString display = localX11Display();
    const QString host = display.section(QLatin1Char(':'), 0, 0);
    const int number = display.section(QLatin1Char(':'), 1).section(QLatin1Char('.'), 0, 0).toInt();
    for (ssh_channel channel : std::exchange(m_newX11Channels, {})) {
        QIODevice *socket = nullptr;
#ifndef _WIN32
        if (host.isEmpty() || host == QStringLiteral("unix")) {
            auto *local = new QLocalSocket(this);
            local->connectToServer(QStringLiteral("/tmp/.X11-unix/X%1").arg(number));
            if (local->waitForConnected(2000))
                socket = local;
            else
                delete local;
        }
#endif
        if (!socket) {
            auto *tcp = new QTcpSocket(this);
            tcp->connectToHost(host.isEmpty() || host == QStringLiteral("unix")
                                   ? QStringLiteral("127.0.0.1")
                                   : host,
                               quint16(6000 + number));
            if (tcp->waitForConnected(2000))
                socket = tcp;
            else
                delete tcp;
        }
        if (!socket) {
            emit statusMessage(QStringLiteral("X11 forwarding: no X server at %1 on this computer.")
                                   .arg(display));
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            continue;
        }
        m_x11Forwards.append({socket, channel});
        connect(socket, &QIODevice::readyRead, this, [this, socket, channel] {
            const QByteArray data = socket->readAll();
            if (!data.isEmpty()
                && ssh_channel_write(channel, data.constData(), uint32_t(data.size())) == SSH_ERROR)
                closeX11Forward(channel);
        });
        connect(socket, &QIODevice::readChannelFinished, this,
                [this, channel] { closeX11Forward(channel); });
    }
}

void SshWorker::pumpX11Forwards()
{
    char buffer[65536];
    QList<ssh_channel> ended;
    for (const auto &forward : std::as_const(m_x11Forwards)) {
        for (;;) {
            const int count = ssh_channel_read_nonblocking(forward.channel, buffer, sizeof buffer, 0);
            if (count <= 0) {
                if (count == SSH_ERROR)
                    ended.append(forward.channel);
                break;
            }
            forward.socket->write(buffer, count);
        }
        if (ssh_channel_is_eof(forward.channel) || ssh_channel_is_closed(forward.channel))
            ended.append(forward.channel);
    }
    for (ssh_channel channel : std::as_const(ended))
        closeX11Forward(channel);
}

void SshWorker::closeX11Forward(ssh_channel channel)
{
    for (qsizetype i = 0; i < m_x11Forwards.size(); ++i) {
        if (m_x11Forwards[i].channel != channel)
            continue;
        const auto forward = m_x11Forwards.takeAt(i);
        if (ssh_channel_is_open(forward.channel))
            ssh_channel_close(forward.channel);
        ssh_channel_free(forward.channel);
        forward.socket->disconnect(this);
        forward.socket->close();
        forward.socket->deleteLater();
        return;
    }
}

// Saved and session port forwards (not the jump-host tunnel mode).
void SshWorker::startPortForwards()
{
    for (int index = 0; index < m_settings.forwards.size(); ++index) {
        m_rules.insert(index, m_settings.forwards[index]);
        startForward(index);
    }
}

// Opens one forward: a listener here, or (Remote, ReverseDynamic) a port
// the server listens on.
void SshWorker::startForward(int index)
{
    const auto rule = m_rules.value(index);
    m_counters.insert(index, Counters());
    if (!m_statsTimer) {
        m_statsTimer = new QTimer(this);
        connect(m_statsTimer, &QTimer::timeout, this, &SshWorker::emitForwardStats);
        m_statsTimer->start(1000);
    }
    if (rule.listensOnServer()) {
        // "localhost": only the server itself may connect, like ssh -R.
        const char *address = rule.allowLan ? nullptr : "localhost";
        int bound = 0;
        if (ssh_channel_listen_forward(m_session, address, rule.listenPort, &bound) != SSH_OK) {
            emit forwardStatus(index, false,
                               QStringLiteral("The server refused to listen on port %1: %2")
                                   .arg(rule.listenPort).arg(lastError()));
            return;
        }
        m_remotePorts.insert(rule.listenPort, index);
        emit forwardStatus(index, true, rule.description());
        return;
    }
    auto *server = new QTcpServer(this);
    if (!server->listen(rule.allowLan ? QHostAddress::Any : QHostAddress::LocalHost, rule.listenPort)) {
        emit forwardStatus(index, false, QStringLiteral("Could not listen on port %1: %2")
                                             .arg(rule.listenPort)
                                             .arg(server->errorString()));
        delete server;
        return;
    }
    m_listeners.insert(index, server);
    connect(server, &QTcpServer::newConnection, this,
            [this, server, index] { acceptForwardConnections(server, index); });
    emit forwardStatus(index, true, rule.description());
}

// Closes a forward and its connections.
void SshWorker::stopForward(int index)
{
    if (!m_rules.contains(index))
        return;
    const auto rule = m_rules.take(index);
    delete m_listeners.take(index);
    if (rule.listensOnServer() && m_remotePorts.value(rule.listenPort, -1) == index) {
        m_remotePorts.remove(rule.listenPort);
        if (m_session && !m_finished)
            ssh_channel_cancel_forward(m_session, rule.allowLan ? nullptr : "localhost", rule.listenPort);
    }
    QList<QTcpSocket *> sockets;
    for (const auto &forward : std::as_const(m_forwards)) {
        if (forward.index == index)
            sockets.append(forward.socket);
    }
    for (auto *socket : std::as_const(sockets))
        closeForward(socket);
    m_counters.remove(index);
}

void SshWorker::addForwards(const QList<PortForward> &forwards, int firstIndex)
{
    if (!m_session || m_finished || m_forwardServer) {
        for (int i = 0; i < forwards.size(); ++i)
            emit forwardStatus(firstIndex + i, false, QStringLiteral("The SSH connection is not open."));
        return;
    }
    for (int i = 0; i < forwards.size(); ++i) {
        stopForward(firstIndex + i);
        m_rules.insert(firstIndex + i, forwards[i]);
        startForward(firstIndex + i);
    }
}

void SshWorker::removeForwards(int firstIndex, int count)
{
    for (int i = 0; i < count; ++i)
        stopForward(firstIndex + i);
}

void SshWorker::emitForwardStats()
{
    for (auto it = m_counters.begin(); it != m_counters.end(); ++it) {
        if (!std::exchange(it->changed, false))
            continue;
        emit forwardStats(it.key(), it->connections, it->received, it->sent);
    }
}

// Whether the server reaches host:port (or the Unix socket host, with port
// 0): a channel is opened there and closed again.
void SshWorker::probe(int id, const QString &host, quint16 port)
{
    if (!m_session || m_finished) {
        emit probeFinished(id, false, QStringLiteral("not connected"), 0);
        return;
    }
    QElapsedTimer timer;
    timer.start();
    ssh_channel channel = ssh_channel_new(m_session);
    const auto target = host.toUtf8();
    int result = SSH_ERROR;
    if (channel) {
        result = port == 0 && host.startsWith(QLatin1Char('/'))
                     ? ssh_channel_open_forward_unix(channel, target.constData(), "127.0.0.1", 0)
                     : ssh_channel_open_forward(channel, target.constData(), port, "127.0.0.1", 0);
    }
    const int elapsed = int(timer.elapsed());
    const QString error = lastError();
    if (channel) {
        if (result == SSH_OK)
            ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    schedulePump();
    emit probeFinished(id, result == SSH_OK, result == SSH_OK ? QString() : error, elapsed);
}

// One command on its own channel, as `ssh host command` runs it: through
// the account's shell, stdout and stderr together in the order they came.
// The worker waits for it (a command connection has nothing else to do).
void SshWorker::runCommand(int id, const QByteArray &command, bool terminal, int timeoutMs, int outputLimit)
{
    QElapsedTimer timer;
    timer.start();
    if (!m_session || m_finished) {
        emit commandFinished(id, -1, {}, false, QStringLiteral("Not connected."), 0);
        return;
    }
    m_cancelCommands = false;
    ssh_channel channel = ssh_channel_new(m_session);
    if (!channel || ssh_channel_open_session(channel) != SSH_OK
        || (terminal && ssh_channel_request_pty_size(channel, "xterm", 200, 50) != SSH_OK)
        || ssh_channel_request_exec(channel, command.constData()) != SSH_OK) {
        const QString error = QStringLiteral("The server refused to run the command: %1").arg(lastError());
        if (channel)
            ssh_channel_free(channel);
        emit commandFinished(id, -1, {}, false, error, int(timer.elapsed()));
        return;
    }
    QByteArray output;
    bool truncated = false;
    QString error;
    char buffer[16384];
    for (;;) {
        if (m_cancelCommands) {
            error = QStringLiteral("Stopped.");
            break;
        }
        if (timer.elapsed() > timeoutMs) {
            error = QStringLiteral("No answer within %1 s; stopped waiting.").arg(timeoutMs / 1000);
            break;
        }
        bool open = false;
        bool read = false;
        for (const int stream : {0, 1}) {
            const int available = ssh_channel_poll_timeout(channel, stream == 0 ? 100 : 0, stream);
            if (available == SSH_ERROR) {
                error = lastError();
                break;
            }
            if (available == SSH_EOF)
                continue;
            open = true;
            if (available <= 0)
                continue;
            const int count = ssh_channel_read_nonblocking(
                channel, buffer, uint32_t(qMin<int>(available, int(sizeof buffer))), stream);
            if (count <= 0)
                continue;
            read = true;
            // Past the limit the rest is read and dropped, so the command
            // can finish.
            const int room = qMax(0, outputLimit - int(output.size()));
            output.append(buffer, qMin(count, room));
            truncated = truncated || count > room;
        }
        if (!error.isEmpty() || (!open && !read))
            break;
    }
    int status = -1;
    if (error.isEmpty()) {
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 11, 0)
        uint32_t code = 0;
        char *signal = nullptr;
        int core = 0;
        if (ssh_channel_get_exit_state(channel, &code, &signal, &core) == SSH_OK) {
            if (signal)
                error = QStringLiteral("Ended by signal %1.").arg(QString::fromUtf8(signal));
            else
                status = int(code);
        }
        ssh_string_free_char(signal);
#else
        status = ssh_channel_get_exit_status(channel);
#endif
    }
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    schedulePump();
    emit commandFinished(id, status, output, truncated, error, int(timer.elapsed()));
}

void SshWorker::acceptForwardConnections(QTcpServer *server, int index)
{
    const auto rule = m_rules.value(index);
    while (auto *socket = server->nextPendingConnection()) {
        // SOCKS and HTTP proxy clients name their destination first.
        const bool proxy = rule.type == PortForward::Type::Dynamic
                           || rule.type == PortForward::Type::HttpProxy;
        Forward entry{socket, nullptr, index, proxy ? 0 : -1, {}};
        entry.http = rule.type == PortForward::Type::HttpProxy;
        m_forwards.append(entry);
        ++m_counters[index].connections;
        m_counters[index].changed = true;
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { forwardFromSocket(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { endForward(socket); });
        if (!proxy && !openForwardChannel(&m_forwards.last(), rule.targetHost, rule.targetPort)) {
            emit forwardConnectionFailed(index, rule.type == PortForward::Type::Socket
                                                    ? QStringLiteral("The SSH server could not open %1: %2")
                                                          .arg(rule.targetHost, lastError())
                                                    : QStringLiteral("The SSH server could not reach %1:%2: %3")
                                                          .arg(rule.targetHost)
                                                          .arg(rule.targetPort)
                                                          .arg(lastError()));
            closeForward(socket);
            continue;
        }
        forwardFromSocket(socket);
    }
}

// Connections the server accepted on a remote forward's port.
void SshWorker::acceptRemoteForwards()
{
    for (;;) {
        int port = 0;
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 11, 0)
        char *originator = nullptr;
        int originatorPort = 0;
        ssh_channel channel = ssh_channel_open_forward_port(m_session, 0, &port, &originator,
                                                            &originatorPort);
        ssh_string_free_char(originator);
#else
        ssh_channel channel = ssh_channel_accept_forward(m_session, 0, &port);
#endif
        if (!channel)
            return;
        const int index = m_remotePorts.value(port, -1);
        if (index < 0) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            continue;
        }
        const auto rule = m_rules.value(index);
        auto *socket = new QTcpSocket(this);
        ++m_counters[index].connections;
        m_counters[index].changed = true;
        if (rule.type == PortForward::Type::ReverseDynamic) {
            // The server's client speaks SOCKS over the channel; the socket
            // connects once its request has been read (continueReverseSocks).
            Forward entry{socket, channel, index, 0, {}};
            entry.reverse = true;
            m_forwards.append(entry);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { forwardFromSocket(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] { endForward(socket); });
            continue;
        }
        m_forwards.append({socket, channel, index, -1, {}});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { forwardFromSocket(socket); });
        connect(socket, &QTcpSocket::connected, this, &SshWorker::pumpForwards);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { endForward(socket); });
        connect(socket, &QTcpSocket::errorOccurred, this, [this, socket, index, rule](QAbstractSocket::SocketError error) {
            // The far end closing after its answer is the normal end.
            if (error == QAbstractSocket::RemoteHostClosedError) {
                endForward(socket);
                return;
            }
            emit forwardConnectionFailed(index, QStringLiteral("Could not reach %1:%2: %3")
                                                .arg(rule.targetHost)
                                                .arg(rule.targetPort)
                                                .arg(socket->errorString()));
            closeForward(socket);
        });
        socket->connectToHost(rule.targetHost, rule.targetPort);
    }
}

// A SOCKS 4, 4a or 5 client (CONNECT only, no authentication), until its
// target is known and a channel to it is open.
void SshWorker::continueSocks(QTcpSocket *socket)
{
    auto *forward = forwardFor(socket);
    forward->pending += socket->readAll();
    const auto request = parseSocksRequest(forward->pending, &forward->socksStage);
    if (!request.reply.isEmpty())
        socket->write(request.reply);
    if (request.result == SocksRequest::More)
        return;
    if (request.result == SocksRequest::Failed) {
        socket->flush();
        closeForward(socket);
        return;
    }
    if (!openForwardChannel(forward, request.host, request.port)) {
        emit forwardConnectionFailed(forward->index,
                           QStringLiteral("The SSH server could not reach %1:%2: %3")
                               .arg(request.host).arg(request.port).arg(lastError()));
        socket->write(request.failure);
        socket->flush();
        closeForward(socket);
        return;
    }
    forward->socksStage = -1;
    socket->write(request.success);
    // Anything the client sent after its request goes to the server.
    const QByteArray rest = std::exchange(forward->pending, QByteArray());
    if (!rest.isEmpty()
        && ssh_channel_write(forward->channel, rest.constData(), uint32_t(rest.size())) == SSH_ERROR) {
        finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
        return;
    }
    pumpForwards();
}

// Reverse SOCKS: requests that clients on the server send over their
// channels; each connects from this computer once its request is complete.
void SshWorker::continueReverseSocks()
{
    QList<QTcpSocket *> failed;
    char buffer[4096];
    for (auto &forward : m_forwards) {
        if (!forward.reverse || forward.socksStage < 0)
            continue;
        for (;;) {
            const int count = ssh_channel_read_nonblocking(forward.channel, buffer, sizeof buffer, 0);
            if (count <= 0)
                break;
            forward.pending.append(buffer, count);
        }
        if (forward.pending.isEmpty()) {
            if (ssh_channel_is_eof(forward.channel) || ssh_channel_is_closed(forward.channel))
                failed.append(forward.socket);
            continue;
        }
        const auto request = parseSocksRequest(forward.pending, &forward.socksStage);
        if (!request.reply.isEmpty())
            ssh_channel_write(forward.channel, request.reply.constData(), uint32_t(request.reply.size()));
        if (request.result == SocksRequest::More)
            continue;
        if (request.result == SocksRequest::Failed) {
            failed.append(forward.socket);
            continue;
        }
        forward.socksStage = -1;
        QTcpSocket *socket = forward.socket;
        ssh_channel channel = forward.channel;
        const int index = forward.index;
        const QByteArray success = request.success;
        const QByteArray failure = request.failure;
        const QString target = QStringLiteral("%1:%2").arg(request.host).arg(request.port);
        connect(socket, &QTcpSocket::connected, this, [this, socket, channel, success] {
            ssh_channel_write(channel, success.constData(), uint32_t(success.size()));
            if (auto *entry = forwardFor(socket)) {
                const QByteArray rest = std::exchange(entry->pending, QByteArray());
                if (!rest.isEmpty())
                    socket->write(rest);
            }
            pumpForwards();
        });
        connect(socket, &QTcpSocket::errorOccurred, this,
                [this, socket, channel, failure, index, target](QAbstractSocket::SocketError error) {
                    if (error == QAbstractSocket::RemoteHostClosedError) {
                        endForward(socket);
                        return;
                    }
                    ssh_channel_write(channel, failure.constData(), uint32_t(failure.size()));
                    emit forwardConnectionFailed(index, QStringLiteral("Could not reach %1 from this "
                                                                       "computer: %2")
                                                            .arg(target, socket->errorString()));
                    closeForward(socket);
                });
        socket->connectToHost(request.host, request.port);
    }
    for (auto *socket : std::as_const(failed))
        closeForward(socket);
}

// An HTTP proxy client: "CONNECT host:port" (tunnelled as is) or a plain
// request for "http://host[:port]/path" (sent on as "/path", one request per
// connection).
void SshWorker::continueHttpProxy(QTcpSocket *socket)
{
    auto *forward = forwardFor(socket);
    forward->pending += socket->readAll();
    QByteArray &buffer = forward->pending;
    const qsizetype end = buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (buffer.size() > MaximumHttpProxyRequest)
            closeForward(socket);
        return;
    }
    const auto refuse = [this, socket](const QByteArray &status) {
        socket->write("HTTP/1.1 " + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        socket->flush();
        closeForward(socket);
    };
    QList<QByteArray> lines = buffer.left(end).split('\n');
    for (auto &line : lines)
        line = line.trimmed();
    const QList<QByteArray> request = lines.value(0).split(' ');
    if (request.size() != 3 || !request.at(2).startsWith("HTTP/1."))
        return refuse("400 Bad Request");
    const QByteArray method = request.at(0);
    QString host;
    quint16 port = 0;
    QByteArray first;
    if (method == "CONNECT") {
        const QUrl url = QUrl::fromUserInput(QString::fromLatin1(request.at(1)));
        host = url.host();
        port = quint16(url.port(443));
    } else {
        const QUrl url(QString::fromLatin1(request.at(1)));
        if (url.scheme() != QStringLiteral("http") || url.host().isEmpty())
            return refuse("400 Bad Request");
        host = url.host();
        port = quint16(url.port(80));
        QByteArray path = url.path(QUrl::FullyEncoded).toLatin1();
        if (path.isEmpty())
            path = "/";
        if (url.hasQuery())
            path += '?' + url.query(QUrl::FullyEncoded).toLatin1();
        // The request for the server: its own path, and closed after the
        // answer (the next request may be for another host).
        first = method + ' ' + path + ' ' + request.at(2) + "\r\n";
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray name = lines[i].left(lines[i].indexOf(':')).trimmed().toLower();
            if (lines[i].isEmpty() || name == "proxy-connection" || name == "connection"
                || name == "proxy-authorization" || name == "keep-alive")
                continue;
            first += lines[i] + "\r\n";
        }
        first += "Connection: close\r\n\r\n";
    }
    if (host.isEmpty() || port == 0)
        return refuse("400 Bad Request");
    buffer.remove(0, end + 4);
    if (!openForwardChannel(forward, host, port)) {
        emit forwardConnectionFailed(forward->index,
                                     QStringLiteral("The SSH server could not reach %1:%2: %3")
                                         .arg(host).arg(port).arg(lastError()));
        return refuse("502 Bad Gateway");
    }
    forward->socksStage = -1;
    if (method == "CONNECT")
        socket->write("HTTP/1.1 200 Connection established\r\n\r\n");
    const QByteArray data = first + std::exchange(buffer, QByteArray());
    if (!data.isEmpty()
        && ssh_channel_write(forward->channel, data.constData(), uint32_t(data.size())) == SSH_ERROR) {
        finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
        return;
    }
    pumpForwards();
}

void SshWorker::pumpForwards()
{
    if (std::any_of(m_forwards.cbegin(), m_forwards.cend(),
                    [](const Forward &forward) { return forward.reverse && forward.socksStage >= 0; }))
        continueReverseSocks();
    char buffer[65536];
    QList<QTcpSocket *> ended;
    QList<QTcpSocket *> halfClosed;
    bool moved = false;
    for (const auto &forward : std::as_const(m_forwards)) {
        if (forward.localEnded) {
            // Nothing more to deliver: drop what still comes, until it closes.
            while (forward.channel && ssh_channel_read_nonblocking(forward.channel, buffer, sizeof buffer, 0) > 0) {
            }
            if (!forward.channel || ssh_channel_is_closed(forward.channel) || ssh_channel_is_eof(forward.channel))
                ended.append(forward.socket);
            continue;
        }
        // Remote forwards: the server's data waits in the channel until
        // the local connection is up.
        if (!forward.channel || forward.socket->state() != QAbstractSocket::ConnectedState)
            continue;
        for (;;) {
            const int count = ssh_channel_read_nonblocking(forward.channel, buffer, sizeof buffer, 0);
            if (count == SSH_ERROR) {
                finish(QStringLiteral("Connection lost: %1").arg(lastError()), true);
                return;
            }
            if (count <= 0)
                break;
            forward.socket->write(buffer, count);
            moved = true;
            if (forward.index >= 0) {
                auto &counters = m_counters[forward.index];
                counters.received += count;
                counters.changed = true;
            }
        }
        if (ssh_channel_is_closed(forward.channel)) {
            ended.append(forward.socket);
        } else if (ssh_channel_is_eof(forward.channel) && !forward.eofSent) {
            // The far end has sent all it will (a half close): pass that on
            // once written, and keep carrying the other direction, which may
            // still bring its answer.
            halfClosed.append(forward.socket);
        }
    }
    for (auto *socket : std::as_const(halfClosed)) {
        if (auto *forward = forwardFor(socket)) {
            forward->eofSent = true;
            socket->flush();
            if (socket->bytesToWrite() == 0) {
                shutdownSending(socket);
            } else {
                connect(socket, &QTcpSocket::bytesWritten, this, [socket] {
                    if (socket->bytesToWrite() == 0)
                        shutdownSending(socket);
                });
            }
        }
    }
    for (auto *socket : std::as_const(ended)) {
        socket->flush();
        closeForward(socket);
    }
    // Reading one channel makes libssh process the packets of others too,
    // so a channel already passed may now hold data or its end, which the
    // socket notifier does not announce: look again soon.
    // Reading queues window adjustments (the server may send more), which
    // libssh's non-blocking reads can leave unsent: the server would wait.
    if (!m_forwards.isEmpty() && m_session && !m_finished)
        ssh_blocking_flush(m_session, 1000);
    bool pending = moved;
    for (const auto &forward : std::as_const(m_forwards)) {
        if (pending || !forward.channel || forward.reverse)
            continue;
        if (forward.localEnded) {
            pending = ssh_channel_is_eof(forward.channel) || ssh_channel_is_closed(forward.channel);
        } else if (forward.socket->state() == QAbstractSocket::ConnectedState) {
            pending = ssh_channel_is_closed(forward.channel)
                      || (ssh_channel_is_eof(forward.channel) && !forward.eofSent)
                      || ssh_channel_poll_timeout(forward.channel, 0, 0) > 0;
        }
    }
    if (pending)
        schedulePump();
}

void SshWorker::endForward(QTcpSocket *socket)
{
    auto *forward = forwardFor(socket);
    if (!forward)
        return;
    if (forward->socksStage < 0 && forward->channel && socket->bytesAvailable() > 0)
        forwardFromSocket(socket);
    forward = forwardFor(socket);
    if (!forward || !forward->channel || forward->socksStage >= 0 || forward->localEnded
        || ssh_channel_is_closed(forward->channel)) {
        if (forward && !forward->localEnded)
            closeForward(socket);
        return;
    }
    // End of data, not a close: the server passes on what it still holds
    // and closes the channel itself (closing at once could lose the end).
    forward->localEnded = true;
    ssh_channel_send_eof(forward->channel);
    socket->disconnect(this);
    // Guarded: a later connection may get the same address.
    QTimer::singleShot(30000, this, [this, guard = QPointer<QTcpSocket>(socket)] {
        if (guard && forwardFor(guard))
            closeForward(guard);
    });
    schedulePump();
}

void SshWorker::closeForward(QTcpSocket *socket)
{
    for (qsizetype i = 0; i < m_forwards.size(); ++i) {
        if (m_forwards[i].socket != socket)
            continue;
        const auto forward = m_forwards.takeAt(i);
        if (forward.index >= 0 && m_counters.contains(forward.index)) {
            auto &counters = m_counters[forward.index];
            counters.connections = qMax(0, counters.connections - 1);
            counters.changed = true;
        }
        if (forward.channel) {
            if (ssh_channel_is_open(forward.channel))
                ssh_channel_close(forward.channel);
            ssh_channel_free(forward.channel);
        }
        // Closing exchanges packets, which may bring others' data along.
        schedulePump();
        // What is still on its way out is written before the socket goes.
        QTcpSocket *socket = forward.socket;
        socket->disconnect(this);
        if (socket->state() == QAbstractSocket::ConnectedState && socket->bytesToWrite() > 0) {
            connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(30000, socket, &QObject::deleteLater);
            socket->disconnectFromHost();
        } else {
            socket->disconnectFromHost();
            socket->deleteLater();
        }
        return;
    }
}

void SshWorker::readAvailable()
{
    if (!m_newAgentChannels.isEmpty())
        startAgentForwards();
    if (!m_agentForwards.isEmpty())
        pumpAgentForwards();
    if (!m_newX11Channels.isEmpty())
        startX11Forwards();
    if (!m_x11Forwards.isEmpty())
        pumpX11Forwards();
    // Accepting processes incoming packets, so it comes before the pump,
    // which then sees what that brought.
    if (!m_finished && !m_remotePorts.isEmpty())
        acceptRemoteForwards();
    if (!m_forwards.isEmpty())
        pumpForwards();
    if (m_finished || m_forwardServer || !m_channel)
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
    while (!m_agentForwards.isEmpty())
        closeAgentForward(m_agentForwards.constFirst().channel);
    for (ssh_channel channel : std::exchange(m_newAgentChannels, {}))
        ssh_channel_free(channel);
    while (!m_x11Forwards.isEmpty())
        closeX11Forward(m_x11Forwards.constFirst().channel);
    for (ssh_channel channel : std::exchange(m_newX11Channels, {}))
        ssh_channel_free(channel);
    delete m_forwardServer;
    m_forwardServer = nullptr;
    // Before the session goes: the backend uses it.
    m_files->reset();
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

// Files over SFTP, or over SCP when the session asks for it or (in "auto")
// the server has no SFTP.
void SshWorker::initializeFiles()
{
    const QString mode = m_settings.fileTransfer;
    QString sftpError;
    if (mode != QStringLiteral("scp")) {
        if (startSftp(&sftpError) || m_finished)
            return;
        if (mode == QStringLiteral("sftp")) {
            emit sftpUnavailable(sftpError);
            readAvailable();
            return;
        }
    }
    auto scp = std::make_unique<ScpBackend>(m_session, [this] { schedulePump(); });
    QString scpError;
    if (!scp->start(&scpError)) {
        emit sftpUnavailable(sftpError.isEmpty()
                                 ? QStringLiteral("SCP is not available: %1").arg(scpError)
                                 : QStringLiteral("%1\nSCP is not available either: %2")
                                       .arg(sftpError, scpError));
        readAvailable();
        return;
    }
    if (!sftpError.isEmpty())
        emit statusMessage(QStringLiteral("The server has no SFTP; files go over SCP."));
    const QString home = scp->home();
    m_files->setBackend(std::move(scp));
    emit sftpReady(home, QStringLiteral("SCP"));
    readAvailable();
}

bool SshWorker::startSftp(QString *error)
{
    ssh_channel channel = ssh_channel_new(m_session);
    if (!channel || ssh_channel_open_session(channel) != SSH_OK
        || ssh_channel_request_sftp(channel) != SSH_OK) {
        *error = QStringLiteral("The server does not provide SFTP: %1").arg(lastError());
        if (channel)
            ssh_channel_free(channel);
        readAvailable();
        return false;
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
            return false;
        }
    }

    sftp_session sftp = sftp_new_channel(m_session, channel);
    if (!sftp) {
        ssh_channel_free(channel);
    } else if (sftp_init(sftp) != SSH_OK) {
        sftp_free(sftp);
        sftp = nullptr;
    }
    if (!sftp) {
        const auto reason = lastError();
        // Output that arrives after the quiet period still breaks the
        // protocol; libssh then reports an invalid packet size.
        const bool scriptOutput = !loginOutput.isEmpty()
            || reason.contains(QStringLiteral("packet size"), Qt::CaseInsensitive);
        *error = scriptOutput
            ? QStringLiteral("SFTP could not start because the server prints text in "
                             "non-interactive sessions%1. Make that output conditional on "
                             "an interactive shell, for example by wrapping it in "
                             "\"if [[ $- == *i* ]]; then … fi\" in ~/.bashrc.")
                  .arg(loginOutput.isEmpty()
                           ? QString()
                           : QStringLiteral(" (\"%1\")")
                                 .arg(QString::fromUtf8(loginOutput).trimmed()
                                          .section(QLatin1Char('\n'), 0, 0)))
            : QStringLiteral("The server does not provide SFTP: %1").arg(reason);
        readAvailable();
        return false;
    }

    auto backend = std::make_unique<SftpBackend>(m_session, sftp, [this] { schedulePump(); });
    const QString home = backend->home();
    m_files->setBackend(std::move(backend));
    emit sftpReady(home, QStringLiteral("SFTP"));
    readAvailable();
    return true;
}

// Replies read synchronously by file operations may leave other channels'
// data buffered inside libssh without the socket becoming readable again;
// drain it from the event loop (never from inside a file operation, which a
// dropped connection would pull the backend out from under).
void SshWorker::schedulePump()
{
    if (std::exchange(m_pumpScheduled, true))
        return;
    QTimer::singleShot(0, this, [this] {
        m_pumpScheduled = false;
        readAvailable();
    });
}

// The folder of the newest shell started under this session's shell (a
// nested bash, su to the same user...), from /proc on the server. Runs in
// its own channel, so nothing shows in the terminal. Servers without /proc
// or without the tag report nothing.
void SshWorker::queryShellFolder()
{
    if (m_shellTag.isEmpty() || !m_session)
        return;
    ssh_channel channel = ssh_channel_new(m_session);
    if (!channel)
        return;
    const QByteArray command =
        "for f in $(grep -lzx 'LC_OPSREMOTE_TAG=" + m_shellTag
        + "' /proc/[0-9]*/environ 2>/dev/null); do d=${f%/environ}; "
          "case $(cat \"$d/comm\" 2>/dev/null) in bash|zsh|sh|fish|dash|ksh|mksh|tcsh|csh|ash) "
          "echo \"${d#/proc/} $(readlink \"$d/cwd\")\";; esac; done | sort -n | tail -n 1";
    QByteArray output;
    if (ssh_channel_open_session(channel) == SSH_OK
        && ssh_channel_request_exec(channel, command.constData()) == SSH_OK) {
        char buffer[1024];
        QElapsedTimer timer;
        timer.start();
        while (output.size() < 8192 && timer.elapsed() < 3000) {
            const int read = ssh_channel_read_timeout(channel, buffer, sizeof buffer, 0, 500);
            if (read > 0)
                output.append(buffer, read);
            else if (read < 0 || ssh_channel_is_eof(channel))
                break;
        }
    }
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    readAvailable();
    // "<pid> <folder>"
    const QString line = QString::fromUtf8(output).trimmed();
    const QString folder = line.section(QLatin1Char(' '), 1);
    if (folder.startsWith(QLatin1Char('/')))
        emit shellFolder(folder);
}

SshSession::SshSession(QObject *parent)
    : RemoteFileSession(parent), m_thread(new QThread), m_worker(new SshWorker)
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
    connect(m_worker, &SshWorker::forwardStatus, this, &SshSession::forwardStatus);
    connect(m_worker, &SshWorker::forwardConnectionFailed, this, &SshSession::forwardConnectionFailed);
    connect(m_worker, &SshWorker::forwardStats, this, &SshSession::forwardStats);
    connect(m_worker, &SshWorker::probeFinished, this, &SshSession::probeFinished);
    connect(m_worker, &SshWorker::commandFinished, this, &SshSession::commandFinished);
    connect(m_worker, &SshWorker::dataReceived, this, &SshSession::dataReceived);
    connect(m_worker, &SshWorker::disconnected, this, &SshSession::disconnected);
    connect(m_worker, &SshWorker::sftpReady, this,
            [this](const QString &homePath, const QString &protocol) {
                m_fileProtocol = protocol;
                emit sftpReady(homePath);
            });
    connect(m_worker, &SshWorker::sftpUnavailable, this, &SshSession::sftpUnavailable);
    connect(m_worker, &SshWorker::shellFolder, this, &SshSession::shellFolder);
    relay(m_worker->files());
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
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path] { worker->files()->listDirectory(path); },
                              Qt::QueuedConnection);
}

void SshSession::addForwards(const QList<PortForward> &forwards, int firstIndex)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, forwards, firstIndex] {
        worker->addForwards(forwards, firstIndex);
    }, Qt::QueuedConnection);
}

void SshSession::removeForwards(int firstIndex, int count)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, firstIndex, count] {
        worker->removeForwards(firstIndex, count);
    }, Qt::QueuedConnection);
}

int SshSession::runCommand(const QString &command, bool terminal, int timeoutSeconds, int outputLimit)
{
    const int id = m_nextJobId++;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, id, command = command.toUtf8(), terminal,
                                         timeoutMs = timeoutSeconds * 1000, outputLimit] {
        worker->runCommand(id, command, terminal, timeoutMs, outputLimit);
    }, Qt::QueuedConnection);
    return id;
}

void SshSession::cancelCommands()
{
    // Read by the worker while it waits for a command.
    m_worker->cancelCommands();
}

void SshSession::probe(int id, const QString &host, quint16 port)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, id, host, port] {
        worker->probe(id, host, port);
    }, Qt::QueuedConnection);
}

void SshSession::queryShellFolder()
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker] { worker->queryShellFolder(); },
                              Qt::QueuedConnection);
}

void SshSession::makeDirectory(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path] { worker->files()->makeDirectory(path); },
                              Qt::QueuedConnection);
}

void SshSession::renamePath(const QString &from, const QString &to)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, from, to] {
        worker->files()->renamePath(from, to);
    }, Qt::QueuedConnection);
}

void SshSession::removePaths(const QStringList &paths)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, paths] { worker->files()->removePaths(paths); },
                              Qt::QueuedConnection);
}

int SshSession::upload(const QStringList &localPaths, const QString &remoteDirectory,
                       std::optional<SftpConflictResolution> policy)
{
    const int jobId = m_nextJobId++;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, jobId, localPaths, remoteDirectory, policy] {
        worker->files()->startTransfer(jobId, true, localPaths, remoteDirectory, policy);
    }, Qt::QueuedConnection);
    return jobId;
}

void SshSession::scanTree(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path] { worker->files()->scanTree(path); },
                              Qt::QueuedConnection);
}

int SshSession::download(const QStringList &remotePaths, const QString &localDirectory,
                         std::optional<SftpConflictResolution> policy)
{
    const int jobId = m_nextJobId++;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, jobId, remotePaths, localDirectory, policy] {
        worker->files()->startTransfer(jobId, false, remotePaths, localDirectory, policy);
    }, Qt::QueuedConnection);
    return jobId;
}

void SshSession::resolveConflict(SftpConflictResolution resolution, bool applyToAll)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, resolution, applyToAll] {
        worker->files()->resolveConflict(resolution, applyToAll);
    }, Qt::QueuedConnection);
}

void SshSession::cancelTransfers()
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker] { worker->files()->cancelTransfers(); },
                              Qt::QueuedConnection);
}

#include "sshsession.moc"
