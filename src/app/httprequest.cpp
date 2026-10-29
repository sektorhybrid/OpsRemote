// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "httprequest.h"

#include <QHostInfo>
#include <QSslCipher>
#include <QSslSocket>
#include <QTimer>

#include <zlib.h>

namespace {

bool isIpAddress(const QString &host)
{
    return !QHostAddress(host).isNull();
}

// "host[:port]", "[v6]:port"; port 0 when not given.
bool splitHostPort(const QString &text, QString *host, quint16 *port)
{
    QString value = text.trimmed();
    *port = 0;
    if (value.startsWith(QLatin1Char('['))) {
        const int end = value.indexOf(QLatin1Char(']'));
        if (end < 0)
            return false;
        *host = value.mid(1, end - 1);
        const QString rest = value.mid(end + 1);
        if (rest.startsWith(QLatin1Char(':')))
            *port = quint16(rest.mid(1).toUInt());
        return !host->isEmpty();
    }
    // More than one colon: an IPv6 address without a port.
    if (value.count(QLatin1Char(':')) == 1) {
        bool ok = false;
        const uint number = value.section(QLatin1Char(':'), 1).toUInt(&ok);
        if (!ok || number == 0 || number > 65535)
            return false;
        *port = quint16(number);
        value = value.section(QLatin1Char(':'), 0, 0);
    }
    *host = value;
    return !host->isEmpty();
}

bool hasHeader(const QList<HttpRequestSpec::Header> &headers, const QString &name)
{
    for (const auto &header : headers) {
        if (header.enabled && header.name.trimmed().compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

}

QByteArray HttpResponse::header(const QByteArray &name) const
{
    for (const auto &[key, value] : headers) {
        if (key.compare(name, Qt::CaseInsensitive) == 0)
            return value;
    }
    return {};
}

HttpRequester::HttpRequester(QObject *parent) : QObject(parent)
{
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        complete(QStringLiteral("No complete answer within %1 s.").arg(m_spec.timeoutSeconds));
    });
}

HttpRequester::~HttpRequester()
{
    cleanup();
}

QByteArray HttpRequester::decompress(const QByteArray &data, const QByteArray &encoding, bool *ok)
{
    *ok = false;
    z_stream stream{};
    // 15 + 32: zlib or gzip, found from the header; -15: raw deflate.
    const bool deflate = encoding.trimmed().compare("deflate", Qt::CaseInsensitive) == 0;
    if (inflateInit2(&stream, deflate ? 15 : 15 + 32) != Z_OK)
        return {};
    QByteArray output;
    char buffer[65536];
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof buffer;
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END)
            break;
        output.append(buffer, int(sizeof buffer - stream.avail_out));
        if (output.size() > 512LL * 1024 * 1024)
            break; // a decompression bomb
    }
    inflateEnd(&stream);
    if (result != Z_STREAM_END && deflate && output.isEmpty()) {
        // Some servers send raw deflate under "deflate".
        z_stream raw{};
        if (inflateInit2(&raw, -15) != Z_OK)
            return {};
        raw.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
        raw.avail_in = uInt(data.size());
        result = Z_OK;
        while (result == Z_OK) {
            raw.next_out = reinterpret_cast<Bytef *>(buffer);
            raw.avail_out = sizeof buffer;
            result = inflate(&raw, Z_NO_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END)
                break;
            output.append(buffer, int(sizeof buffer - raw.avail_out));
        }
        inflateEnd(&raw);
    }
    *ok = result == Z_STREAM_END;
    return output;
}

void HttpRequester::send(const HttpRequestSpec &spec)
{
    cancel();
    m_spec = spec;
    m_method = spec.method.trimmed().toUpper();
    m_body = spec.body;
    m_redirects.clear();
    m_overridesApply = true;
    m_busy = true;
    m_clock.start();
    m_timeout->start(qMax(1, spec.timeoutSeconds) * 1000);
    QUrl url = QUrl::fromUserInput(spec.url.trimmed());
    start(url);
}

void HttpRequester::cancel()
{
    if (!m_busy)
        return;
    complete(QStringLiteral("Stopped."));
}

void HttpRequester::start(const QUrl &url)
{
    cleanup();
    const int generation = ++m_generation;
    m_url = url;
    m_response = HttpResponse();
    m_response.url = url.toString();
    m_response.redirects = m_redirects;
    m_buffer.clear();
    m_rawBody.clear();
    m_headDone = false;
    m_chunked = false;
    m_contentLength = -1;
    m_chunkLeft = -1;
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
        || url.host().isEmpty()) {
        complete(QStringLiteral("Not an http:// or https:// address: %1").arg(url.toString()));
        return;
    }
    m_response.tls = scheme == QStringLiteral("https");
    m_connectHost = url.host();
    m_connectPort = quint16(url.port(m_response.tls ? 443 : 80));
    if (m_overridesApply && !m_spec.connectTo.trimmed().isEmpty()) {
        QString host;
        quint16 port = 0;
        if (!splitHostPort(m_spec.connectTo, &host, &port)) {
            complete(QStringLiteral("\"Connect to\" should be host or host:port, not %1.").arg(m_spec.connectTo));
            return;
        }
        m_connectHost = host;
        if (port != 0)
            m_connectPort = port;
    }
    m_response.port = m_connectPort;
    if (isIpAddress(m_connectHost)) {
        connectTo(QHostAddress(m_connectHost));
        return;
    }
    // The name is looked up here, so the time it takes is known.
    QElapsedTimer lookup;
    lookup.start();
    m_lookupId = QHostInfo::lookupHost(m_connectHost, this, [this, generation, lookup](const QHostInfo &info) {
        if (generation != m_generation)
            return;
        m_lookupId = -1;
        m_response.dnsMs = lookup.elapsed();
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            complete(QStringLiteral("%1 could not be found: %2").arg(m_connectHost, info.errorString()));
            return;
        }
        // IPv4 first: the one most networks reach.
        QHostAddress chosen = info.addresses().constFirst();
        for (const auto &address : info.addresses()) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                chosen = address;
                break;
            }
        }
        connectTo(chosen);
    });
}

void HttpRequester::connectTo(const QHostAddress &address)
{
    m_response.address = address;
    m_socket = new QSslSocket(this);
    QElapsedTimer step;
    step.start();
    const int generation = m_generation;
    connect(m_socket, &QSslSocket::connected, this, [this, step]() mutable {
        m_response.connectMs = step.elapsed();
        step.restart();
        if (!m_response.tls)
            sendRequest();
    });
    connect(m_socket, &QSslSocket::encrypted, this, [this, step] {
        m_response.tlsMs = step.elapsed() - qMax<qint64>(0, m_response.connectMs);
        m_response.tlsProtocol = [](QSsl::SslProtocol protocol) {
            switch (protocol) {
            case QSsl::TlsV1_2: return QStringLiteral("TLS 1.2");
            case QSsl::TlsV1_3: return QStringLiteral("TLS 1.3");
            default: return QStringLiteral("TLS");
            }
        }(m_socket->sessionProtocol());
        m_response.cipher = m_socket->sessionCipher().name();
        m_response.certificates = m_socket->peerCertificateChain();
        sendRequest();
    });
    connect(m_socket, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
        for (const auto &error : errors)
            m_response.certificateProblems << error.errorString();
        m_response.certificates = m_socket->peerCertificateChain();
        // Shown either way; only checked requests stop here.
        if (!m_spec.verifyCertificates)
            m_socket->ignoreSslErrors();
    });
    connect(m_socket, &QSslSocket::readyRead, this, &HttpRequester::readSocket);
    connect(m_socket, &QSslSocket::disconnected, this, [this, generation] {
        if (generation != m_generation)
            return;
        m_buffer += m_socket->readAll();
        if (!m_headDone) {
            if (!parseHead()) {
                complete(m_buffer.isEmpty() ? QStringLiteral("The server closed the connection without answering.")
                                            : QStringLiteral("The server's answer is not HTTP."));
                return;
            }
        }
        if (m_headDone && !parseBody(true))
            complete(QStringLiteral("The connection ended before the whole body came."));
    });
    connect(m_socket, &QSslSocket::errorOccurred, this, [this, generation](QAbstractSocket::SocketError error) {
        if (generation != m_generation || error == QAbstractSocket::RemoteHostClosedError)
            return;
        if (!m_response.certificateProblems.isEmpty() && m_spec.verifyCertificates
            && error == QAbstractSocket::SslHandshakeFailedError) {
            complete(QStringLiteral("The certificate was not accepted: %1")
                         .arg(m_response.certificateProblems.join(QStringLiteral("; "))));
            return;
        }
        complete(m_socket->errorString());
    });
    if (m_response.tls) {
        // The name sent in the TLS handshake (SNI) and checked on the
        // certificate: the one asked for, or the URL's host. An address
        // sends none.
        QString name = m_overridesApply && !m_spec.sni.trimmed().isEmpty() ? m_spec.sni.trimmed() : m_url.host();
        m_response.sniSent = isIpAddress(name) ? QString() : name;
        m_socket->setPeerVerifyName(name);
        m_socket->connectToHostEncrypted(address.toString(), m_connectPort, name);
    } else {
        m_socket->connectToHost(address, m_connectPort);
    }
}

void HttpRequester::sendRequest()
{
    QByteArray target = m_url.path(QUrl::FullyEncoded).toUtf8();
    if (target.isEmpty())
        target = "/";
    if (m_url.hasQuery())
        target += '?' + m_url.query(QUrl::FullyEncoded).toUtf8();
    const bool defaultPort = m_url.port(-1) == -1
                             || m_url.port() == (m_response.tls ? 443 : 80);
    QString host = m_url.host();
    if (host.contains(QLatin1Char(':')))
        host = QStringLiteral("[%1]").arg(host);
    if (!defaultPort)
        host += QStringLiteral(":%1").arg(m_url.port());
    if (m_overridesApply && !m_spec.hostHeader.trimmed().isEmpty())
        host = m_spec.hostHeader.trimmed();
    QByteArray head = m_method.toLatin1() + ' ' + target + " HTTP/1.1\r\n";
    if (!hasHeader(m_spec.headers, QStringLiteral("Host")) || !m_overridesApply)
        head += "Host: " + host.toUtf8() + "\r\n";
    for (const auto &header : std::as_const(m_spec.headers)) {
        const QString name = header.name.trimmed();
        if (!header.enabled || name.isEmpty())
            continue;
        // Framing is ours: one request per connection, our length.
        if (name.compare(QStringLiteral("Content-Length"), Qt::CaseInsensitive) == 0
            || name.compare(QStringLiteral("Connection"), Qt::CaseInsensitive) == 0
            || name.compare(QStringLiteral("Transfer-Encoding"), Qt::CaseInsensitive) == 0)
            continue;
        if (name.compare(QStringLiteral("Host"), Qt::CaseInsensitive) == 0 && !m_overridesApply)
            continue;
        // No line breaks smuggled into the request.
        QString value = header.value;
        value.replace(QLatin1Char('\r'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
        head += name.toUtf8() + ": " + value.toUtf8() + "\r\n";
    }
    if (!hasHeader(m_spec.headers, QStringLiteral("User-Agent")) && !m_spec.userAgent.isEmpty())
        head += "User-Agent: " + m_spec.userAgent.toUtf8() + "\r\n";
    if (!hasHeader(m_spec.headers, QStringLiteral("Accept")))
        head += "Accept: */*\r\n";
    if (!hasHeader(m_spec.headers, QStringLiteral("Accept-Encoding")))
        head += "Accept-Encoding: gzip, deflate\r\n";
    const bool bodyMethod = m_method == QStringLiteral("POST") || m_method == QStringLiteral("PUT")
                            || m_method == QStringLiteral("PATCH");
    if (!m_body.isEmpty() || bodyMethod) {
        if (!m_body.isEmpty() && !m_spec.contentType.isEmpty() && !hasHeader(m_spec.headers, QStringLiteral("Content-Type")))
            head += "Content-Type: " + m_spec.contentType.toUtf8() + "\r\n";
        head += "Content-Length: " + QByteArray::number(m_body.size()) + "\r\n";
    }
    head += "Connection: close\r\n\r\n";
    m_response.requestHead = head;
    m_socket->write(head);
    if (!m_body.isEmpty())
        m_socket->write(m_body);
}

void HttpRequester::readSocket()
{
    if (!m_socket)
        return;
    if (m_response.firstByteMs < 0)
        m_response.firstByteMs = m_clock.elapsed();
    m_buffer += m_socket->readAll();
    if (!m_headDone && !parseHead())
        return;
    parseBody(false);
}

bool HttpRequester::parseHead()
{
    for (;;) {
        const qsizetype end = m_buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_buffer.size() > 256 * 1024)
                complete(QStringLiteral("The answer's headers are too long."));
            return false;
        }
        const QByteArray head = m_buffer.left(end);
        m_buffer.remove(0, end + 4);
        const QList<QByteArray> lines = head.split('\n');
        const QByteArray statusLine = lines.value(0).trimmed();
        if (!statusLine.startsWith("HTTP/")) {
            complete(QStringLiteral("The server's answer is not HTTP: %1").arg(QString::fromLatin1(statusLine.left(80))));
            return false;
        }
        const int status = statusLine.split(' ').value(1).toInt();
        // 100 Continue and other interim answers come before the real one.
        if (status >= 100 && status < 200 && status != 101) {
            m_response.responseHead += head + "\r\n\r\n";
            continue;
        }
        m_response.responseHead += head;
        const QString statusText = QString::fromUtf8(statusLine);
        m_response.httpVersion = statusText.section(QLatin1Char(' '), 0, 0);
        m_response.status = status;
        m_response.reason = statusText.section(QLatin1Char(' '), 2);
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            m_response.headers.append({line.left(colon).trimmed(), line.mid(colon + 1).trimmed()});
        }
        m_chunked = m_response.header("Transfer-Encoding").toLower().contains("chunked");
        bool ok = false;
        const qint64 length = m_response.header("Content-Length").toLongLong(&ok);
        m_contentLength = ok ? length : -1;
        // No body: HEAD, 1xx, 204, 304.
        if (m_method == QStringLiteral("HEAD") || status == 204 || status == 304)
            m_contentLength = 0, m_chunked = false;
        m_headDone = true;
        return true;
    }
}

bool HttpRequester::parseBody(bool closed)
{
    const auto take = [this](const QByteArray &bytes) {
        m_response.bodyBytesOnWire += bytes.size();
        const qint64 room = m_spec.maxBodyBytes - m_rawBody.size();
        if (room > 0)
            m_rawBody += bytes.left(room);
        if (bytes.size() > room)
            m_response.bodyTruncated = true;
    };
    if (m_chunked) {
        for (;;) {
            if (m_chunkLeft < 0) {
                const qsizetype lineEnd = m_buffer.indexOf("\r\n");
                if (lineEnd < 0)
                    break;
                bool ok = false;
                const qint64 size = m_buffer.left(lineEnd).split(';').value(0).trimmed().toLongLong(&ok, 16);
                if (!ok || size < 0) {
                    complete(QStringLiteral("The chunked body is malformed."));
                    return true;
                }
                m_buffer.remove(0, lineEnd + 2);
                if (size == 0) {
                    complete(QString());
                    return true;
                }
                m_chunkLeft = size;
            }
            if (m_buffer.size() < m_chunkLeft + 2)
                break;
            take(m_buffer.left(m_chunkLeft));
            m_buffer.remove(0, m_chunkLeft + 2);
            m_chunkLeft = -1;
            if (m_response.bodyTruncated) {
                complete(QString());
                return true;
            }
        }
        // Closed before the last chunk: incomplete.
        return !closed;
    }
    take(m_buffer);
    m_buffer.clear();
    if (m_contentLength >= 0 && m_response.bodyBytesOnWire >= m_contentLength) {
        complete(QString());
        return true;
    }
    if (m_response.bodyTruncated) {
        complete(QString());
        return true;
    }
    // Without a length, the body ends with the connection.
    if (closed) {
        if (m_contentLength >= 0)
            return false;
        complete(QString());
    }
    return true;
}

void HttpRequester::complete(const QString &error)
{
    if (!m_busy)
        return;
    m_response.error = error;
    m_response.totalMs = m_clock.elapsed();
    m_response.contentEncoding = QString::fromLatin1(m_response.header("Content-Encoding").trimmed());
    m_response.body = m_rawBody;
    const QByteArray encoding = m_response.contentEncoding.toLatin1().toLower();
    if (error.isEmpty() && !m_rawBody.isEmpty() && (encoding == "gzip" || encoding == "deflate" || encoding == "x-gzip")
        && !m_response.bodyTruncated) {
        bool ok = false;
        const QByteArray plain = decompress(m_rawBody, encoding, &ok);
        if (ok)
            m_response.body = plain;
    }
    // A redirect to follow: the same request again (GET after 303, and
    // after 301/302 for a POST, as browsers do).
    const int status = m_response.status;
    const QByteArray location = m_response.header("Location");
    if (error.isEmpty() && m_spec.followRedirects && !location.isEmpty()
        && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
        if (m_redirects.size() >= m_spec.maxRedirects) {
            m_response.error = QStringLiteral("More than %1 redirects; stopped.").arg(m_spec.maxRedirects);
        } else {
            const QUrl next = m_url.resolved(QUrl::fromEncoded(location));
            m_redirects << m_url.toString();
            if (status == 303 || ((status == 301 || status == 302) && m_method == QStringLiteral("POST"))) {
                m_method = QStringLiteral("GET");
                m_body.clear();
            }
            // Connect to, SNI and Host were meant for the first host.
            if (next.host().compare(m_url.host(), Qt::CaseInsensitive) != 0 || next.port() != m_url.port())
                m_overridesApply = false;
            QTimer::singleShot(0, this, [this, next] { start(next); });
            return;
        }
    }
    m_busy = false;
    m_timeout->stop();
    const HttpResponse response = m_response;
    cleanup();
    emit finished(response);
}

void HttpRequester::cleanup()
{
    ++m_generation;
    if (m_lookupId >= 0) {
        QHostInfo::abortHostLookup(m_lookupId);
        m_lookupId = -1;
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}
