// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "ftpclient.h"
#include "appdirs.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocale>
#include <QFile>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTcpServer>
#include <QThread>
#include <QTimeZone>
#include <QTimer>

#include <cctype>
#include <functional>
#include <memory>

namespace {

constexpr int MaximumReplyLines = 20000;
constexpr qint64 DataChunk = 64 * 1024;
// Limits on what a server may send: one reply line, a whole reply, a folder
// listing, and the time a listing may take.
constexpr qint64 MaximumReplyLine = 16 * 1024;
constexpr qint64 MaximumReplyBytes = 1024 * 1024;
constexpr qint64 MaximumListingBytes = 32 * 1024 * 1024;
constexpr qint64 MaximumListingMs = 10 * 60 * 1000;

// The worker's session was closed: stop waiting.
bool interrupted()
{
    return QThread::currentThread()->isInterruptionRequested();
}

QString hex(const QByteArray &digest)
{
    QStringList parts;
    for (const char byte : digest)
        parts << QStringLiteral("%1").arg(quint8(byte), 2, 16, QLatin1Char('0')).toUpper();
    return parts.join(QLatin1Char(':'));
}

// The accepted certificates: "host:port sha256-hex" lines in a file, and
// those accepted for this run only.
class CertificateTrust
{
public:
    explicit CertificateTrust(const QString &path) : m_path(path) {}

    static QString key(const QString &host, quint16 port)
    {
        return QStringLiteral("%1:%2").arg(host.toLower()).arg(port);
    }
    bool isTrusted(const QString &host, quint16 port, const QByteArray &digest) const
    {
        const QString line = key(host, port) + QLatin1Char(' ') + QString::fromLatin1(digest.toHex());
        return m_once.contains(line) || stored().contains(line);
    }
    // Whether another certificate was saved for this server.
    bool isChanged(const QString &host, quint16 port) const
    {
        const QString prefix = key(host, port) + QLatin1Char(' ');
        for (const auto &line : stored()) {
            if (line.startsWith(prefix) && !line.endsWith(QStringLiteral(" plain")))
                return true;
        }
        return false;
    }
    // Trusts the certificate for this run, and with save for good; false
    // (with error) when it could not be saved.
    bool accept(const QString &host, quint16 port, const QByteArray &digest, bool save,
                QString *error)
    {
        const QString line = key(host, port) + QLatin1Char(' ') + QString::fromLatin1(digest.toHex());
        m_once.insert(line);
        if (!save)
            return true;
        const QString prefix = key(host, port) + QLatin1Char(' ');
        return store(line, [&prefix](const QString &existing) {
            // One certificate per server; plain-text permissions stay.
            return existing.startsWith(prefix) && !existing.endsWith(QStringLiteral(" plain"));
        }, error);
    }
    // Sending the log-on unencrypted to a server without TLS, allowed.
    bool isPlainAllowed(const QString &host, quint16 port) const
    {
        const QString line = key(host, port) + QStringLiteral(" plain");
        return m_once.contains(line) || stored().contains(line);
    }
    bool allowPlain(const QString &host, quint16 port, bool save, QString *error)
    {
        const QString line = key(host, port) + QStringLiteral(" plain");
        m_once.insert(line);
        if (!save)
            return true;
        return store(line, [&line](const QString &existing) { return existing == line; }, error);
    }

private:
    // Adds line, dropping the lines replaced says, owner-only.
    bool store(const QString &line, const std::function<bool(const QString &)> &replaced,
               QString *error)
    {
        if (m_path.isEmpty()) {
            *error = QStringLiteral("there is no file to keep it in");
            return false;
        }
        QStringList lines;
        for (const auto &existing : stored()) {
            if (!replaced(existing))
                lines << existing;
        }
        lines << line;
        QDir().mkpath(QFileInfo(m_path).absolutePath());
        QSaveFile file(m_path);
        const QByteArray contents = (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
            *error = file.errorString();
            return false;
        }
        restrictToOwner(m_path);
        return true;
    }

    QStringList stored() const
    {
        QFile file(m_path);
        if (m_path.isEmpty() || !file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    QString m_path;
    QSet<QString> m_once;
};

// Active mode: the server's connection, taken as a descriptor so it can
// become a TLS socket.
class DataListener final : public QTcpServer
{
public:
    qintptr takePending() { return m_pending.isEmpty() ? -1 : m_pending.takeFirst(); }
    bool hasPending() const { return !m_pending.isEmpty(); }

protected:
    void incomingConnection(qintptr descriptor) override { m_pending.append(descriptor); }

private:
    QList<qintptr> m_pending;
};

QDateTime parseFtpTime(QString text)
{
    // YYYYMMDDHHMMSS[.sss], UTC
    text = text.trimmed().section(QLatin1Char('.'), 0, 0);
    auto time = QDateTime::fromString(text, QStringLiteral("yyyyMMddHHmmss"));
    if (!time.isValid())
        return {};
    time.setTimeZone(QTimeZone::UTC);
    return time;
}

// One MLSD/MLST line: "fact=value;fact=value; name".
bool parseMlsdLine(const QString &line, SftpEntry *entry)
{
    const int space = line.indexOf(QLatin1Char(' '));
    if (space < 0)
        return false;
    entry->name = line.mid(space + 1);
    QString type;
    for (const auto &fact : line.left(space).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString name = fact.section(QLatin1Char('='), 0, 0).toLower();
        const QString value = fact.section(QLatin1Char('='), 1);
        if (name == QStringLiteral("type"))
            type = value.toLower();
        else if (name == QStringLiteral("size") || name == QStringLiteral("sizd"))
            entry->size = value.toULongLong();
        else if (name == QStringLiteral("modify"))
            entry->modified = parseFtpTime(value);
        else if (name == QStringLiteral("unix.mode"))
            entry->permissions = value.toUInt(nullptr, 8);
    }
    if (type == QStringLiteral("cdir") || type == QStringLiteral("pdir"))
        return false;
    entry->isDirectory = type == QStringLiteral("dir");
    entry->isSymlink = type.startsWith(QStringLiteral("os.unix=slink"))
        || type.startsWith(QStringLiteral("os.unix=symlink"));
    entry->isOther = !entry->isDirectory && !entry->isSymlink && type != QStringLiteral("file");
    return !entry->name.isEmpty();
}

// Windows (IIS) style: "01-15-24  03:45PM  <DIR>  name" or "... 1234 name".
bool parseDosLine(const QString &line, SftpEntry *entry)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^(\\d{2})-(\\d{2})-(\\d{2,4})\\s+(\\d{1,2}):(\\d{2})(AM|PM)?\\s+(<DIR>|\\d+)\\s+(.+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(line);
    if (!match.hasMatch())
        return false;
    int year = match.captured(3).toInt();
    if (year < 100)
        year += year < 70 ? 2000 : 1900;
    int hour = match.captured(4).toInt();
    const QString half = match.captured(6).toUpper();
    if (half == QStringLiteral("PM") && hour < 12)
        hour += 12;
    if (half == QStringLiteral("AM") && hour == 12)
        hour = 0;
    entry->modified = QDateTime(QDate(year, match.captured(1).toInt(), match.captured(2).toInt()),
                                QTime(hour, match.captured(5).toInt()));
    entry->isDirectory = match.captured(7).compare(QStringLiteral("<DIR>"), Qt::CaseInsensitive) == 0;
    entry->size = entry->isDirectory ? 0 : match.captured(7).toULongLong();
    entry->name = match.captured(8);
    return true;
}

// The text of a reply without its codes, for messages.
QString replyMessage(const QString &reply)
{
    QString last = reply.section(QLatin1Char('\n'), -1).trimmed();
    if (last.size() >= 4 && last.at(0).isDigit())
        last = last.mid(4).trimmed();
    return last.isEmpty() ? QStringLiteral("no answer") : last;
}

// One control connection and its data connections.
class FtpControl
{
public:
    enum class Open { Ok, Failed, Certificate, Login, PlainText };

    FtpControl(FtpConnectionSettings &settings, CertificateTrust &trust,
               std::function<void(const QString &)> log, const QString &name)
        : m_settings(settings), m_trust(trust), m_log(std::move(log)), m_name(name)
    {
    }
    ~FtpControl() { close(); }

    bool isOpen() const
    {
        return m_socket && m_loggedIn && m_socket->state() == QAbstractSocket::ConnectedState;
    }
    bool isEncrypted() const { return m_socket && m_socket->isEncrypted(); }
    bool dataEncrypted() const { return m_dataEncrypted; }
    QString home() const { return m_home; }
    bool hasMlst() const { return m_mlst; }
    bool hasMfmt() const { return m_mfmt; }
    bool hasMdtm() const { return m_mdtm; }
    qint64 idleMs() const { return m_lastUse.isValid() ? m_lastUse.elapsed() : 0; }

    // The details of a certificate that was not trusted (after Open::Certificate).
    QString certificateProblems, certificateDetails;
    QByteArray certificateDigest;

    Open open(QString *error)
    {
        close();
        certificateDigest.clear();
        m_socket = std::make_unique<QSslSocket>();
        configure(m_socket.get(), true);
        const int timeout = qMax(1, m_settings.connectTimeout) * 1000;
        const bool implicit = m_settings.security == QStringLiteral("implicit");
        log(QStringLiteral("Connecting to %1:%2%3").arg(m_settings.host).arg(m_settings.port)
                .arg(implicit ? QStringLiteral(" (TLS)") : QString()));
        if (implicit) {
            m_socket->connectToHostEncrypted(m_settings.host, m_settings.port);
            if (!m_socket->waitForEncrypted(timeout))
                return tlsFailure(error);
        } else {
            m_socket->connectToHost(m_settings.host, m_settings.port);
            if (!m_socket->waitForConnected(timeout)) {
                *error = QStringLiteral("Could not connect to %1:%2: %3")
                             .arg(m_settings.host).arg(m_settings.port).arg(m_socket->errorString());
                close();
                return Open::Failed;
            }
        }
        QString reply;
        int code = readReply(&reply, timeout);
        while (code == 120)
            code = readReply(&reply, timeout * 4);
        if (code != 220) {
            *error = code ? QStringLiteral("The server refused the connection: %1").arg(replyMessage(reply))
                          : QStringLiteral("The server did not greet (is it an FTP server?).");
            close();
            return Open::Failed;
        }
        if (!implicit && m_settings.security != QStringLiteral("none")) {
            code = command(QStringLiteral("AUTH TLS"), &reply, false);
            if (code == 234) {
                m_socket->startClientEncryption();
                if (!m_socket->waitForEncrypted(timeout))
                    return tlsFailure(error);
            } else if (m_settings.security == QStringLiteral("explicit")) {
                *error = QStringLiteral("The server does not offer TLS (FTPS), which this session "
                                        "requires: %1").arg(replyMessage(reply));
                close();
                return Open::Failed;
            } else {
                log(QStringLiteral("The server does not offer TLS: the connection is not encrypted."));
                // A password is not sent in plain text without asking first
                // (an anonymous log-on has none).
                if (!m_settings.username.isEmpty()
                    && !m_trust.isPlainAllowed(m_settings.host, m_settings.port)) {
                    *error = QStringLiteral("%1 does not offer TLS: the password would be sent "
                                            "unencrypted.").arg(m_settings.host);
                    close();
                    return Open::PlainText;
                }
            }
        }
        if (m_socket->isEncrypted())
            m_controlDigest = m_socket->peerCertificate().digest(QCryptographicHash::Sha256);

        const QString user = m_settings.username.isEmpty() ? QStringLiteral("anonymous")
                                                           : m_settings.username;
        code = command(QStringLiteral("USER ") + user, &reply, false);
        if (code == 331 || code == 332) {
            const QString password = m_settings.password.isEmpty() && m_settings.username.isEmpty()
                ? QStringLiteral("anonymous@")
                : m_settings.password;
            code = command(QStringLiteral("PASS ") + password, &reply, false);
        }
        if (code == 332) {
            *error = QStringLiteral("The server asks for an account (ACCT), which is not supported.");
            close();
            return Open::Failed;
        }
        if (code != 230 && code != 202) {
            *error = code ? QStringLiteral("Log-on failed: %1").arg(replyMessage(reply))
                          : QStringLiteral("The connection closed during log-on.");
            close();
            return code / 100 == 5 ? Open::Login : Open::Failed;
        }

        m_dataEncrypted = false;
        if (m_socket->isEncrypted()) {
            command(QStringLiteral("PBSZ 0"), &reply, false);
            if (command(QStringLiteral("PROT P"), &reply, false) / 100 == 2) {
                m_dataEncrypted = true;
            } else if (m_settings.security != QStringLiteral("auto")) {
                *error = QStringLiteral("The server does not encrypt file transfers (PROT P): %1")
                             .arg(replyMessage(reply));
                close();
                return Open::Failed;
            }
        }
        m_mlst = m_mfmt = m_mdtm = m_utf8 = false;
        if (command(QStringLiteral("FEAT"), &reply, false) == 211) {
            for (const auto &line : reply.split(QLatin1Char('\n'))) {
                const QString feature = line.trimmed().section(QLatin1Char(' '), 0, 0).toUpper();
                if (!line.startsWith(QLatin1Char(' ')))
                    continue;
                if (feature == QStringLiteral("MLST"))
                    m_mlst = true;
                else if (feature == QStringLiteral("MFMT"))
                    m_mfmt = true;
                else if (feature == QStringLiteral("MDTM"))
                    m_mdtm = true;
                else if (feature == QStringLiteral("UTF8"))
                    m_utf8 = true;
            }
        }
        if (m_settings.listing == QStringLiteral("list"))
            m_mlst = false;
        if (m_utf8)
            command(QStringLiteral("OPTS UTF8 ON"), &reply, false);
        if (command(QStringLiteral("TYPE I"), &reply, false) / 100 != 2) {
            *error = QStringLiteral("The server refused binary transfers: %1").arg(replyMessage(reply));
            close();
            return Open::Failed;
        }
        m_home = QStringLiteral("/");
        if (command(QStringLiteral("PWD"), &reply, false) == 257)
            m_home = quotedPath(reply, m_home);
        m_loggedIn = true;
        return Open::Ok;
    }

    void close()
    {
        m_loggedIn = false;
        if (m_socket) {
            m_socket->abort();
            m_socket.reset();
        }
        m_data.reset();
    }

    // Sends a command and reads its final reply: the reply code, or 0 when
    // the connection failed. A dropped connection is opened again once when
    // retry is set.
    int command(const QString &line, QString *reply, bool retry = true)
    {
        if (line.contains(QLatin1Char('\r')) || line.contains(QLatin1Char('\n'))) {
            *reply = QStringLiteral("500 Names with line breaks cannot be sent over FTP.");
            return 500;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (attempt == 1 || (retry && !isOpen())) {
                if (!retry)
                    return 0;
                QString error;
                if (open(&error) != Open::Ok) {
                    *reply = QStringLiteral("421 ") + error;
                    return 0;
                }
            }
            if (!send(line))
                continue;
            const int code = readReply(reply, qMax(1, m_settings.connectTimeout) * 2000);
            // 421: the server is closing the connection (idle timeout).
            if (code != 0 && code != 421)
                return code;
            close();
            if (!retry)
                return code;
        }
        return 0;
    }

    // Opens a data connection for a command (LIST, RETR, STOR...): the
    // connected socket once the server accepted the command.
    QSslSocket *openData(const QString &line, QString *error)
    {
        m_data.reset();
        m_finalReceived = false;
        m_finalReply.clear();
        // The transfer connection opens on first use.
        if (!isOpen() && open(error) != Open::Ok)
            return nullptr;
        const int timeout = qMax(1, m_settings.connectTimeout) * 1000;
        QString reply;
        // A proxy can only reach the server, so it forces passive mode.
        const bool passive = m_settings.passive || m_settings.proxy.isSet();
        std::unique_ptr<DataListener> listener;
        if (passive) {
            quint16 port = 0;
            if (m_epsv) {
                if (command(QStringLiteral("EPSV"), &reply) == 229) {
                    static const QRegularExpression epsv(QStringLiteral("\\(([^0-9])\\1\\1(\\d{1,5})\\1\\)"));
                    const auto match = epsv.match(reply);
                    bool ok = false;
                    const uint value = match.hasMatch() ? match.captured(2).toUInt(&ok) : 0;
                    port = ok && value >= 1 && value <= 65535 ? quint16(value) : 0;
                } else {
                    m_epsv = false;
                }
            }
            if (!port) {
                if (command(QStringLiteral("PASV"), &reply) == 227) {
                    static const QRegularExpression pasv(
                        QStringLiteral("(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+)"));
                    const auto match = pasv.match(reply);
                    // The address in the reply is often a private one behind
                    // NAT; the control connection's server is used instead
                    // (which also refuses "FTP bounce" redirection).
                    if (match.hasMatch()) {
                        bool valid = true;
                        for (int index = 1; index <= 6; ++index)
                            valid = valid && match.captured(index).toUInt() <= 255;
                        const uint value = match.captured(5).toUInt() * 256 + match.captured(6).toUInt();
                        port = valid && value >= 1 ? quint16(value) : 0;
                    }
                }
            }
            if (!port) {
                *error = QStringLiteral("The server did not open a data connection: %1")
                             .arg(replyMessage(reply));
                return nullptr;
            }
            m_data = std::make_unique<QSslSocket>();
            configure(m_data.get(), false);
            const bool proxied = m_settings.proxy.isSet();
            if (proxied)
                m_data->connectToHost(m_settings.host, port);
            else
                m_data->connectToHost(m_socket->peerAddress(), port);
            if (!m_data->waitForConnected(timeout)) {
                *error = QStringLiteral("Could not open the data connection (port %1): %2")
                             .arg(port).arg(m_data->errorString());
                m_data.reset();
                return nullptr;
            }
            if (m_dataEncrypted)
                m_data->startClientEncryption();
        } else {
            listener = std::make_unique<DataListener>();
            const QHostAddress local = m_socket->localAddress();
            if (!listener->listen(local, 0)) {
                *error = QStringLiteral("Could not listen for the data connection: %1")
                             .arg(listener->errorString());
                return nullptr;
            }
            const quint16 port = listener->serverPort();
            bool ok = false;
            const quint32 ipv4 = local.toIPv4Address(&ok);
            const QString portCommand = ok
                ? QStringLiteral("PORT %1,%2,%3,%4,%5,%6").arg(ipv4 >> 24).arg((ipv4 >> 16) & 255)
                      .arg((ipv4 >> 8) & 255).arg(ipv4 & 255).arg(port >> 8).arg(port & 255)
                : QStringLiteral("EPRT |2|%1|%2|").arg(local.toString()).arg(port);
            if (command(portCommand, &reply) / 100 != 2) {
                *error = QStringLiteral("The server refused active mode: %1").arg(replyMessage(reply));
                return nullptr;
            }
        }

        if (!send(line)) {
            *error = QStringLiteral("The connection closed.");
            m_data.reset();
            return nullptr;
        }
        // While waiting for the reply, the data connection's TLS handshake
        // goes on (servers differ in which they do first).
        const auto progress = [&] {
            if (listener && !m_data) {
                listener->waitForNewConnection(0);
                if (!listener->hasPending())
                    return;
                m_data = std::make_unique<QSslSocket>();
                configure(m_data.get(), false);
                m_data->setSocketDescriptor(listener->takePending());
                if (m_dataEncrypted)
                    m_data->startClientEncryption();
            }
            if (m_data && m_dataEncrypted && !m_data->isEncrypted())
                m_data->waitForEncrypted(20);
        };
        const int code = readReply(&reply, timeout * 2, progress);
        if (code / 100 == 2) {
            // Finished without a transfer (some servers, for empty listings).
            m_finalReceived = true;
            m_finalCode = code;
            m_finalReply = reply;
        } else if (code / 100 != 1) {
            *error = code ? replyMessage(reply) : QStringLiteral("The connection closed.");
            m_data.reset();
            return nullptr;
        }
        if (!m_data && listener) {
            QDeadlineTimer deadline(timeout);
            while (!m_data && !deadline.hasExpired()) {
                listener->waitForNewConnection(100);
                progress();
            }
        }
        if (!m_data) {
            *error = QStringLiteral("The server did not connect back (active mode); passive mode "
                                    "may work.");
            abortData();
            return nullptr;
        }
        if (m_dataEncrypted && !m_data->isEncrypted() && !m_data->waitForEncrypted(timeout)) {
            *error = QStringLiteral("The encrypted data connection failed: %1")
                         .arg(m_data->errorString());
            abortData();
            return nullptr;
        }
        return m_data.get();
    }

    // After a transfer's data connection is done: the server's result.
    bool finishData(QString *error)
    {
        if (m_data) {
            if (m_data->state() != QAbstractSocket::UnconnectedState) {
                m_data->disconnectFromHost();
                if (m_data->state() != QAbstractSocket::UnconnectedState)
                    m_data->waitForDisconnected(qMax(1, m_settings.connectTimeout) * 1000);
            }
            m_data.reset();
        }
        QString reply = m_finalReply;
        const int code = m_finalReceived
            ? m_finalCode
            : readReply(&reply, qMax(1, m_settings.connectTimeout) * 4000);
        m_finalReceived = false;
        if (code / 100 == 2)
            return true;
        *error = code ? replyMessage(reply) : QStringLiteral("the connection closed");
        return false;
    }

    // Abandons a transfer: closes the data connection and reads the server's
    // answer; a server that does not answer is disconnected.
    void abortData()
    {
        if (m_data) {
            m_data->abort();
            m_data.reset();
        }
        if (std::exchange(m_finalReceived, false))
            return;
        QString reply;
        if (readReply(&reply, 3000) == 0)
            close();
    }

    QSslSocket *data() const { return m_data.get(); }

    // Waits for more data on the data connection: true when some can be
    // read, false at its end. Some servers (vsftpd over TLS) keep the data
    // connection open after the last byte; their final reply on the control
    // connection ends it then.
    bool waitForData(QString *error)
    {
        QSslSocket *socket = m_data.get();
        QDeadlineTimer deadline(30000);
        while (socket) {
            if (socket->bytesAvailable() > 0)
                return true;
            if (interrupted()) {
                *error = QStringLiteral("the session was closed");
                return false;
            }
            if (socket->state() != QAbstractSocket::ConnectedState
                || socket->property("peerClosed").toBool())
                return false;
            if (socket->waitForReadyRead(200)) {
                deadline.setRemainingTime(30000);
                continue;
            }
            if (!m_finalReceived && m_socket && (m_socket->canReadLine() || m_socket->waitForReadyRead(0))) {
                m_finalCode = readReply(&m_finalReply, 2000);
                m_finalReceived = true;
            }
            if (m_finalReceived) {
                // Anything still on its way.
                while (socket->waitForReadyRead(300)) {
                    if (socket->bytesAvailable() > 0)
                        return true;
                }
                return socket->bytesAvailable() > 0;
            }
            if (deadline.hasExpired()) {
                *error = QStringLiteral("the server stopped sending");
                return false;
            }
        }
        return false;
    }

private:
    void log(const QString &text) { m_log(m_name.isEmpty() ? text : m_name + text); }

    void configure(QSslSocket *socket, bool control)
    {
        if (m_settings.proxy.isSet()) {
            const auto &proxy = m_settings.proxy;
            socket->setProxy(QNetworkProxy(proxy.type == ProxySettings::Type::Http
                                               ? QNetworkProxy::HttpProxy
                                               : QNetworkProxy::Socks5Proxy,
                                           proxy.host, proxy.port, proxy.user, proxy.password));
        } else {
            socket->setProxy(QNetworkProxy::NoProxy);
        }
        socket->setPeerVerifyName(m_settings.host);
        if (control) {
            auto configuration = socket->sslConfiguration();
            // Data connections resume the control connection's TLS session;
            // many servers require that.
            configuration.setSslOption(QSsl::SslOptionDisableSessionPersistence, false);
            socket->setSslConfiguration(configuration);
            QObject::connect(socket, &QSslSocket::sslErrors, socket,
                             [this, socket](const QList<QSslError> &errors) {
                                 const auto certificate = socket->peerCertificate();
                                 const auto digest = certificate.digest(QCryptographicHash::Sha256);
                                 if (m_trust.isTrusted(m_settings.host, m_settings.port, digest)) {
                                     socket->ignoreSslErrors();
                                     return;
                                 }
                                 QStringList problems;
                                 for (const auto &error : errors)
                                     problems << error.errorString();
                                 certificateProblems = problems.join(QLatin1Char('\n'));
                                 certificateDigest = digest;
                                 certificateDetails =
                                     QStringLiteral("Issued to: %1\nIssued by: %2\nValid: %3 – %4")
                                         .arg(certificate.subjectDisplayName(),
                                              certificate.issuerDisplayName(),
                                              QLocale().toString(certificate.effectiveDate().toLocalTime(),
                                                                 QLocale::ShortFormat),
                                              QLocale().toString(certificate.expiryDate().toLocalTime(),
                                                                 QLocale::ShortFormat));
                             });
        } else if (m_socket) {
            // The end of the data over TLS: the server's close_notify, which
            // Qt reports this way while the connection stays open (vsftpd
            // waits for ours before it closes and replies).
            QObject::connect(socket, &QAbstractSocket::errorOccurred, socket,
                             [socket](QAbstractSocket::SocketError error) {
                                 if (error == QAbstractSocket::RemoteHostClosedError)
                                     socket->setProperty("peerClosed", true);
                             });
            socket->setSslConfiguration(m_socket->sslConfiguration());
            // The same certificate as the control connection.
            QObject::connect(socket, &QSslSocket::sslErrors, socket, [this, socket] {
                if (socket->peerCertificate().digest(QCryptographicHash::Sha256) == m_controlDigest)
                    socket->ignoreSslErrors();
            });
        }
    }

    Open tlsFailure(QString *error)
    {
        const bool question = !certificateDigest.isEmpty();
        *error = question ? QStringLiteral("The server's certificate is not trusted.")
                          : QStringLiteral("The encrypted connection failed: %1")
                                .arg(m_socket->errorString());
        close();
        return question ? Open::Certificate : Open::Failed;
    }

    bool send(const QString &line)
    {
        if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
            return false;
        // The log shows neither the password nor the account.
        log(line.startsWith(QStringLiteral("PASS ")) ? QStringLiteral("> PASS ********")
            : line.startsWith(QStringLiteral("USER ")) ? QStringLiteral("> USER ********")
                                                         : QStringLiteral("> ") + line);
        m_lastUse.start();
        m_socket->write(line.toUtf8() + "\r\n");
        while (m_socket->bytesToWrite() > 0 || m_socket->encryptedBytesToWrite() > 0) {
            if (!m_socket->waitForBytesWritten(qMax(1, m_settings.connectTimeout) * 1000))
                return false;
        }
        return true;
    }

    // The final reply's code, all of its lines in reply; 0 when the
    // connection closed or did not answer in time.
    int readReply(QString *reply, int timeoutMs, const std::function<void()> &progress = {})
    {
        reply->clear();
        if (!m_socket)
            return 0;
        QDeadlineTimer deadline(timeoutMs);
        QByteArray code;
        QStringList lines;
        qint64 total = 0;
        for (;;) {
            while (!m_socket->canReadLine()) {
                // A line without its end that has grown past any real reply.
                if (m_socket->bytesAvailable() > MaximumReplyLine || interrupted())
                    return 0;
                if (deadline.hasExpired() || m_socket->state() != QAbstractSocket::ConnectedState) {
                    if (!m_socket->canReadLine())
                        return 0;
                    break;
                }
                if (progress)
                    progress();
                m_socket->waitForReadyRead(progress ? 50 : int(qMin<qint64>(deadline.remainingTime(), 1000)));
            }
            QByteArray line = m_socket->readLine(MaximumReplyLine + 2);
            total += line.size();
            if (!line.endsWith('\n') || total > MaximumReplyBytes)
                return 0;
            while (line.endsWith('\n') || line.endsWith('\r'))
                line.chop(1);
            const QString text = QString::fromUtf8(line);
            log(QStringLiteral("< ") + text);
            lines << text;
            if (lines.size() > MaximumReplyLines)
                return 0;
            if (code.isEmpty()) {
                if (line.size() < 3 || !std::isdigit(uchar(line[0])) || !std::isdigit(uchar(line[1]))
                    || !std::isdigit(uchar(line[2])))
                    return 0;
                code = line.left(3);
                if (line.size() == 3 || line[3] != '-')
                    break;
            } else if (line.startsWith(code + ' ') || line == code) {
                break;
            }
        }
        m_lastUse.start();
        *reply = lines.join(QLatin1Char('\n'));
        return code.toInt();
    }

public:
    // The path in a 257 reply: "path" with "" for a quote.
    static QString quotedPath(const QString &reply, const QString &fallback)
    {
        const QString first = reply.section(QLatin1Char('\n'), 0, 0);
        const int start = first.indexOf(QLatin1Char('"'));
        if (start < 0)
            return fallback;
        QString path;
        for (int index = start + 1; index < first.size(); ++index) {
            if (first[index] == QLatin1Char('"')) {
                if (index + 1 < first.size() && first[index + 1] == QLatin1Char('"')) {
                    path += QLatin1Char('"');
                    ++index;
                    continue;
                }
                break;
            }
            path += first[index];
        }
        return path.isEmpty() ? fallback : path;
    }

private:

    FtpConnectionSettings &m_settings;
    CertificateTrust &m_trust;
    std::function<void(const QString &)> m_log;
    QString m_name;
    std::unique_ptr<QSslSocket> m_socket;
    std::unique_ptr<QSslSocket> m_data;
    QByteArray m_controlDigest;
    QString m_home;
    QElapsedTimer m_lastUse;
    bool m_loggedIn = false;
    bool m_dataEncrypted = false;
    bool m_epsv = true;
    bool m_mlst = false;
    bool m_mfmt = false;
    bool m_mdtm = false;
    bool m_utf8 = false;
    bool m_finalReceived = false;
    int m_finalCode = 0;
    QString m_finalReply;
};

// Files over FTP: listings and folder operations on one control connection,
// transfers on another (FTP runs one command at a time per connection).
class FtpBackend final : public RemoteFileBackend
{
public:
    FtpBackend(FtpConnectionSettings &settings, CertificateTrust &trust,
               const std::function<void(const QString &)> &log)
        : m_browse(settings, trust, log, QString()),
          m_transfer(settings, trust, log, QStringLiteral("[transfer] "))
    {
    }

    FtpControl &browse() { return m_browse; }
    FtpControl &transfer() { return m_transfer; }

    QString home() override { return m_browse.home(); }

    QString canonicalPath(const QString &path, QString *error) override
    {
        QString reply;
        const int code = m_browse.command(QStringLiteral("CWD ") + path, &reply);
        if (code / 100 != 2) {
            *error = QStringLiteral("Could not open %1: %2").arg(path, replyMessage(reply));
            return {};
        }
        if (m_browse.command(QStringLiteral("PWD"), &reply) != 257)
            return path;
        return FtpControl::quotedPath(reply, path);
    }

    bool list(const QString &path, bool resolveLinks, QList<SftpEntry> *entries,
              QString *error) override
    {
        QString reply;
        const bool mlsd = m_browse.hasMlst();
        if (!mlsd) {
            // LIST describes the current folder most reliably (names with
            // spaces, servers that take options).
            if (m_browse.command(QStringLiteral("CWD ") + path, &reply) / 100 != 2) {
                *error = QStringLiteral("Could not list %1: %2").arg(path, replyMessage(reply));
                return false;
            }
        }
        QByteArray listing;
        if (!readAll(m_browse, mlsd ? QStringLiteral("MLSD ") + path : QStringLiteral("LIST"),
                     &listing, error)) {
            *error = QStringLiteral("Could not list %1: %2").arg(path, *error);
            return false;
        }
        for (QString line : QString::fromUtf8(listing).split(QLatin1Char('\n'))) {
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            if (line.isEmpty())
                continue;
            SftpEntry entry;
            const bool parsed = mlsd ? parseMlsdLine(line, &entry)
                                     : RemotePaths::parseLsLine(line, &entry) || parseDosLine(line, &entry);
            if (!parsed || entry.name == QStringLiteral(".") || entry.name == QStringLiteral("..")
                || entry.name.contains(QLatin1Char('/')))
                continue;
            if (entry.isSymlink && resolveLinks) {
                // A link that can be entered is a folder.
                entry.isDirectory = m_browse.command(QStringLiteral("CWD ")
                                                         + RemotePaths::join(path, entry.name),
                                                     &reply) / 100 == 2;
            }
            entries->append(entry);
        }
        return true;
    }

    std::optional<Info> stat(const QString &path, bool, QString *error) override
    {
        QString reply;
        Info info;
        if (m_browse.hasMlst()) {
            const int code = m_browse.command(QStringLiteral("MLST ") + path, &reply);
            if (code / 100 != 2) {
                *error = QStringLiteral("Could not read %1: %2").arg(path, replyMessage(reply));
                return std::nullopt;
            }
            for (const auto &line : reply.split(QLatin1Char('\n'))) {
                SftpEntry entry;
                if (line.startsWith(QLatin1Char(' ')) && parseMlsdLine(line.mid(1), &entry)) {
                    info.isDirectory = entry.isDirectory;
                    info.isSymlink = entry.isSymlink;
                    info.isRegular = !entry.isDirectory && !entry.isSymlink && !entry.isOther;
                    info.size = entry.size;
                    info.modified = entry.modified;
                    info.permissions = entry.permissions;
                    return info;
                }
            }
            // "cdir" for a folder itself, on some servers.
            if (reply.contains(QStringLiteral("type=cdir"), Qt::CaseInsensitive)
                || reply.contains(QStringLiteral("type=dir"), Qt::CaseInsensitive)) {
                info.isDirectory = true;
                return info;
            }
        }
        const int size = m_browse.command(QStringLiteral("SIZE ") + path, &reply);
        if (size == 213) {
            info.isRegular = true;
            info.size = replyMessage(reply).toULongLong();
            if (m_browse.command(QStringLiteral("MDTM ") + path, &reply) == 213)
                info.modified = parseFtpTime(replyMessage(reply));
            return info;
        }
        const QString sizeReply = replyMessage(reply);
        if (m_browse.command(QStringLiteral("CWD ") + path, &reply) / 100 == 2) {
            info.isDirectory = true;
            return info;
        }
        *error = QStringLiteral("Could not read %1: %2").arg(path, sizeReply);
        return std::nullopt;
    }

    bool makeDirectory(const QString &path, QString *error) override
    {
        return simple(QStringLiteral("MKD ") + path, QStringLiteral("create folder"), path, error);
    }

    bool rename(const QString &from, const QString &to, QString *error) override
    {
        // Like SFTP, never replaces an existing entry (RNTO may).
        QString ignored;
        if (stat(to, true, &ignored)) {
            *error = QStringLiteral("Could not rename %1: %2 already exists.").arg(from, to);
            return false;
        }
        QString reply;
        if (m_browse.command(QStringLiteral("RNFR ") + from, &reply) != 350
            || m_browse.command(QStringLiteral("RNTO ") + to, &reply, false) / 100 != 2) {
            *error = QStringLiteral("Could not rename %1: %2").arg(from, replyMessage(reply));
            return false;
        }
        return true;
    }

    bool removeFile(const QString &path, QString *error) override
    {
        return simple(QStringLiteral("DELE ") + path, QStringLiteral("delete"), path, error);
    }

    bool replace(const QString &from, const QString &to, QString *error) override
    {
        // Most servers replace with RNTO; others need the old file gone.
        QString reply;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (m_transfer.command(QStringLiteral("RNFR ") + from, &reply) == 350
                && m_transfer.command(QStringLiteral("RNTO ") + to, &reply, false) / 100 == 2)
                return true;
            if (attempt == 0)
                m_transfer.command(QStringLiteral("DELE ") + to, &reply);
        }
        *error = QStringLiteral("Could not replace %1: %2").arg(to, replyMessage(reply));
        return false;
    }

    bool removeDirectory(const QString &path, QString *error) override
    {
        return simple(QStringLiteral("RMD ") + path, QStringLiteral("delete"), path, error);
    }

    bool openRead(const QString &path, QString *error) override
    {
        m_path = path;
        QString reason;
        if (!m_transfer.openData(QStringLiteral("RETR ") + path, &reason)) {
            *error = QStringLiteral("Could not read %1: %2").arg(path, reason);
            return false;
        }
        return true;
    }

    qint64 read(char *data, qint64 size, QString *error) override
    {
        QSslSocket *socket = m_transfer.data();
        if (!socket)
            return 0;
        QString reason;
        if (!m_transfer.waitForData(&reason)) {
            if (reason.isEmpty())
                return 0;
            *error = QStringLiteral("Could not read %1: %2").arg(m_path, reason);
            return -1;
        }
        return socket->read(data, size);
    }

    bool openWrite(const QString &path, quint64, const QDateTime &, bool, QString *error) override
    {
        m_path = path;
        QString reason;
        if (!m_transfer.openData(QStringLiteral("STOR ") + path, &reason)) {
            *error = QStringLiteral("Could not create %1: %2").arg(path, reason);
            return false;
        }
        return true;
    }

    bool write(const char *data, qint64 size, QString *error) override
    {
        QSslSocket *socket = m_transfer.data();
        if (!socket || socket->write(data, size) != size) {
            *error = QStringLiteral("Could not write %1: the data connection closed.").arg(m_path);
            return false;
        }
        while (socket->bytesToWrite() > 0 || socket->encryptedBytesToWrite() > 0) {
            if (!socket->waitForBytesWritten(30000)) {
                *error = QStringLiteral("Could not write %1: %2").arg(m_path, socket->errorString());
                return false;
            }
        }
        return true;
    }

    bool closeFile(bool success, QString *error) override
    {
        if (!success) {
            m_transfer.abortData();
            return false;
        }
        QString reason;
        if (m_transfer.finishData(&reason))
            return true;
        *error = QStringLiteral("Could not save %1: %2").arg(m_path, reason);
        return false;
    }

    void setModified(const QString &path, const QDateTime &modified) override
    {
        const QString time = modified.toUTC().toString(QStringLiteral("yyyyMMddHHmmss"));
        QString reply;
        if (m_transfer.hasMfmt())
            m_transfer.command(QStringLiteral("MFMT %1 %2").arg(time, path), &reply);
        else if (m_transfer.hasMdtm())
            // vsftpd's way (mdtm_write); elsewhere a harmless "no such file".
            m_transfer.command(QStringLiteral("MDTM %1 %2").arg(time, path), &reply);
    }

    bool exactListingTimes() const override { return m_browse.hasMlst(); }
    qint64 readChunk() const override { return DataChunk; }
    qint64 writeChunk() const override { return DataChunk; }

    // Keeps idle connections from timing out.
    void keepalive(int seconds)
    {
        QString reply;
        for (FtpControl *control : {&m_browse, &m_transfer}) {
            if (control->isOpen() && !control->data() && control->idleMs() >= seconds * 1000LL)
                control->command(QStringLiteral("NOOP"), &reply, false);
        }
    }

private:
    bool simple(const QString &line, const QString &action, const QString &path, QString *error)
    {
        QString reply;
        if (m_browse.command(line, &reply) / 100 == 2)
            return true;
        *error = QStringLiteral("Could not %1 %2: %3").arg(action, path, replyMessage(reply));
        return false;
    }

    bool readAll(FtpControl &control, const QString &line, QByteArray *output, QString *error)
    {
        QSslSocket *socket = control.openData(line, error);
        if (!socket)
            return false;
        QElapsedTimer started;
        started.start();
        while (control.waitForData(error)) {
            output->append(socket->readAll());
            if (output->size() > MaximumListingBytes || started.elapsed() > MaximumListingMs) {
                control.abortData();
                *error = QStringLiteral("the listing is too large or too slow");
                return false;
            }
        }
        if (!error->isEmpty()) {
            control.abortData();
            return false;
        }
        output->append(socket->readAll());
        return control.finishData(error);
    }

    FtpControl m_browse;
    FtpControl m_transfer;
    QString m_path;
};

}

class FtpWorker final : public QObject
{
    Q_OBJECT

public:
    FtpWorker() : m_files(new RemoteFileEngine(this))
    {
        m_files->setUnavailableMessage(QStringLiteral("Not connected."));
    }
    ~FtpWorker() override
    {
        m_files->reset();
        m_settings.password.fill(QChar(0));
    }

    RemoteFileEngine *files() const { return m_files; }

    void start(const FtpConnectionSettings &settings)
    {
        m_files->reset();
        m_backend = nullptr;
        m_settings = settings;
        m_trust = std::make_unique<CertificateTrust>(settings.knownCertificatesPath);
        connectNow();
    }

    void answerCertificate(FtpCertificateDecision decision)
    {
        if (m_pendingDigest.isEmpty())
            return;
        const auto digest = std::exchange(m_pendingDigest, {});
        if (decision == FtpCertificateDecision::Reject) {
            emit disconnected(QStringLiteral("The server's certificate was not accepted."), true);
            return;
        }
        QString error;
        if (!m_trust->accept(m_settings.host, m_settings.port, digest,
                             decision == FtpCertificateDecision::AcceptAndSave, &error))
            emit statusMessage(QStringLiteral("The certificate could not be saved (%1): it is "
                                              "trusted for this session only.").arg(error));
        connectNow();
    }

    void answerPlainText(bool allow, bool remember)
    {
        if (!allow) {
            emit disconnected(QStringLiteral("Not connected: %1 offers no encryption.").arg(m_settings.host),
                              true);
            return;
        }
        QString error;
        if (!m_trust->allowPlain(m_settings.host, m_settings.port, remember, &error))
            emit statusMessage(QStringLiteral("The choice could not be saved (%1): it holds for this "
                                              "session only.").arg(error));
        connectNow();
    }

    void retryLogin(const QString &password)
    {
        m_settings.password = password;
        connectNow();
    }

signals:
    void statusMessage(const QString &message);
    void logLine(const QString &line);
    void certificateQuestion(const QString &problems, const QString &fingerprint,
                             const QString &details, bool changed);
    void loginFailed(const QString &message);
    void plainTextQuestion(const QString &message);
    void connected(const QString &protocol);
    void sftpReady(const QString &homePath);
    void disconnected(const QString &reason, bool error);

private:
    void connectNow()
    {
        m_files->reset();
        m_backend = nullptr;
        auto backend = std::make_unique<FtpBackend>(
            m_settings, *m_trust, [this](const QString &line) { emit logLine(line); });
        QString error;
        auto &control = backend->browse();
        switch (control.open(&error)) {
        case FtpControl::Open::Ok:
            break;
        case FtpControl::Open::Certificate:
            m_pendingDigest = control.certificateDigest;
            emit certificateQuestion(control.certificateProblems, ftpFingerprint(control.certificateDigest),
                                     control.certificateDetails,
                                     m_trust->isChanged(m_settings.host, m_settings.port));
            return;
        case FtpControl::Open::PlainText:
            emit plainTextQuestion(error);
            return;
        case FtpControl::Open::Login:
            emit loginFailed(error);
            return;
        case FtpControl::Open::Failed:
            emit disconnected(error, true);
            return;
        }
        const QString protocol = control.isEncrypted() ? QStringLiteral("FTPS") : QStringLiteral("FTP");
        if (!control.isEncrypted())
            emit statusMessage(QStringLiteral("Connected without encryption: passwords and files "
                                              "travel in plain text."));
        else if (!control.dataEncrypted())
            emit statusMessage(QStringLiteral("The server encrypts the log-on but not file "
                                              "transfers."));
        const QString home = control.home();
        m_backend = backend.get();
        m_files->setBackend(std::move(backend));
        emit connected(protocol);
        emit sftpReady(home);
        if (m_settings.keepaliveSeconds > 0) {
            if (!m_keepalive) {
                m_keepalive = new QTimer(this);
                connect(m_keepalive, &QTimer::timeout, this, [this] {
                    if (m_files->backend() == m_backend && m_backend)
                        m_backend->keepalive(m_settings.keepaliveSeconds);
                });
            }
            m_keepalive->start(m_settings.keepaliveSeconds * 1000);
        }
    }

    FtpConnectionSettings m_settings;
    std::unique_ptr<CertificateTrust> m_trust;
    RemoteFileEngine *m_files = nullptr;
    FtpBackend *m_backend = nullptr;
    QTimer *m_keepalive = nullptr;
    QByteArray m_pendingDigest;
};

QString ftpFingerprint(const QByteArray &digest)
{
    return hex(digest);
}

FtpSession::FtpSession(QObject *parent)
    : RemoteFileSession(parent), m_thread(new QThread), m_worker(new FtpWorker)
{
    m_thread->setObjectName(QStringLiteral("ftp-session"));
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    connect(m_worker, &FtpWorker::statusMessage, this, &FtpSession::statusMessage);
    connect(m_worker, &FtpWorker::logLine, this, &FtpSession::logLine);
    connect(m_worker, &FtpWorker::certificateQuestion, this, &FtpSession::certificateQuestion);
    connect(m_worker, &FtpWorker::loginFailed, this, &FtpSession::loginFailed);
    connect(m_worker, &FtpWorker::plainTextQuestion, this, &FtpSession::plainTextQuestion);
    connect(m_worker, &FtpWorker::connected, this, [this](const QString &protocol) {
        m_protocol = protocol;
        emit connected(protocol);
    });
    connect(m_worker, &FtpWorker::sftpReady, this, &FtpSession::sftpReady);
    connect(m_worker, &FtpWorker::disconnected, this, &FtpSession::disconnected);
    relay(m_worker->files());
    m_thread->start();
}

FtpSession::~FtpSession()
{
    // As with SSH: the worker may be inside a blocking call; it is deleted in
    // its own thread when that returns.
    disconnect(m_worker, nullptr, this, nullptr);
    // Waits in the worker check this and give up.
    m_thread->requestInterruption();
    m_thread->quit();
}

void FtpSession::connectToHost(const FtpConnectionSettings &settings)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, settings] { worker->start(settings); },
                              Qt::QueuedConnection);
}

void FtpSession::answerCertificate(FtpCertificateDecision decision)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, decision] {
        worker->answerCertificate(decision);
    }, Qt::QueuedConnection);
}

void FtpSession::answerPlainText(bool allow, bool remember)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, allow, remember] {
        worker->answerPlainText(allow, remember);
    }, Qt::QueuedConnection);
}

void FtpSession::retryLogin(const QString &password)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, password] { worker->retryLogin(password); },
                              Qt::QueuedConnection);
}

void FtpSession::listDirectory(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path] {
        worker->files()->listDirectory(path);
    }, Qt::QueuedConnection);
}

void FtpSession::makeDirectory(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path] {
        worker->files()->makeDirectory(path);
    }, Qt::QueuedConnection);
}

void FtpSession::renamePath(const QString &from, const QString &to)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, from, to] {
        worker->files()->renamePath(from, to);
    }, Qt::QueuedConnection);
}

void FtpSession::removePaths(const QStringList &paths)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, paths] {
        worker->files()->removePaths(paths);
    }, Qt::QueuedConnection);
}

int FtpSession::upload(const QStringList &localPaths, const QString &remoteDirectory,
                       std::optional<SftpConflictResolution> policy)
{
    const int jobId = m_nextJobId++;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, jobId, localPaths, remoteDirectory, policy] {
        worker->files()->startTransfer(jobId, true, localPaths, remoteDirectory, policy);
    }, Qt::QueuedConnection);
    return jobId;
}

int FtpSession::download(const QStringList &remotePaths, const QString &localDirectory,
                         std::optional<SftpConflictResolution> policy)
{
    const int jobId = m_nextJobId++;
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, jobId, remotePaths, localDirectory, policy] {
        worker->files()->startTransfer(jobId, false, remotePaths, localDirectory, policy);
    }, Qt::QueuedConnection);
    return jobId;
}

void FtpSession::scanTree(const QString &path)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, path] { worker->files()->scanTree(path); },
                              Qt::QueuedConnection);
}

void FtpSession::resolveConflict(SftpConflictResolution resolution, bool applyToAll)
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, resolution, applyToAll] {
        worker->files()->resolveConflict(resolution, applyToAll);
    }, Qt::QueuedConnection);
}

void FtpSession::cancelTransfers()
{
    QMetaObject::invokeMethod(m_worker, [worker = m_worker] { worker->files()->cancelTransfers(); },
                              Qt::QueuedConnection);
}

#include "ftpclient.moc"
