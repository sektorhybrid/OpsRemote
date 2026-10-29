// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSslCertificate>
#include <QString>
#include <QStringList>
#include <QUrl>

class QSslSocket;
class QTimer;

// One HTTP request as the HTTP Request tool sends it. Where the connection
// goes, the TLS server name (SNI) and the Host header can each differ from
// the URL's host, like curl's --connect-to and -H Host:.
struct HttpRequestSpec
{
    struct Header
    {
        bool enabled = true;
        QString name;
        QString value;
        bool operator==(const Header &) const = default;
    };

    QString method = QStringLiteral("GET");
    QString url;
    QString connectTo; // host[:port]; empty: the URL's
    QString sni; // empty: the URL's host (none for an IP address)
    QString hostHeader; // empty: the URL's host[:port]
    QList<Header> headers;
    QByteArray body;
    QString contentType; // sent when the headers have none and there is a body
    bool verifyCertificates = true;
    bool followRedirects = true;
    int maxRedirects = 10;
    int timeoutSeconds = 30;
    qint64 maxBodyBytes = 50LL * 1024 * 1024;
    QString userAgent;

    bool operator==(const HttpRequestSpec &) const = default;
};

struct HttpResponse
{
    QString error; // empty when a response came
    QString url; // the one that answered (after redirects)
    QStringList redirects; // the URLs that sent us on, in order
    QString httpVersion;
    int status = 0;
    QString reason;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body; // decoded (chunked, gzip, deflate)
    qint64 bodyBytesOnWire = 0;
    bool bodyTruncated = false;
    QString contentEncoding;
    QByteArray requestHead; // what was sent, without the body
    QByteArray responseHead;
    // Milliseconds; -1 when not part of this request.
    qint64 dnsMs = -1;
    qint64 connectMs = -1;
    qint64 tlsMs = -1;
    qint64 firstByteMs = -1;
    qint64 totalMs = -1;
    QHostAddress address;
    quint16 port = 0;
    // TLS
    bool tls = false;
    QString sniSent;
    QString tlsProtocol;
    QString cipher;
    QList<QSslCertificate> certificates;
    QStringList certificateProblems; // empty: verified
    QByteArray header(const QByteArray &name) const;
};

// Sends one request (and follows redirects when asked), measuring each step.
class HttpRequester final : public QObject
{
    Q_OBJECT

public:
    explicit HttpRequester(QObject *parent = nullptr);
    ~HttpRequester() override;

    void send(const HttpRequestSpec &spec);
    void cancel();
    bool busy() const { return m_busy; }

    // A gzip or deflate body decoded (zlib); ok false when it is not one.
    static QByteArray decompress(const QByteArray &data, const QByteArray &encoding, bool *ok);

signals:
    void finished(const HttpResponse &response);

private:
    void start(const QUrl &url);
    void connectTo(const QHostAddress &address);
    void sendRequest();
    void readSocket();
    bool parseHead();
    bool parseBody(bool closed);
    void complete(const QString &error);
    void cleanup();

    HttpRequestSpec m_spec;
    QUrl m_url;
    QString m_method;
    QByteArray m_body;
    bool m_overridesApply = true; // still on the URL the overrides were for
    QString m_connectHost;
    quint16 m_connectPort = 0;
    HttpResponse m_response;
    QStringList m_redirects;
    QSslSocket *m_socket = nullptr;
    QTimer *m_timeout = nullptr;
    QElapsedTimer m_clock;
    int m_lookupId = -1;
    QByteArray m_buffer;
    bool m_headDone = false;
    bool m_chunked = false;
    qint64 m_contentLength = -1;
    qint64 m_chunkLeft = -1; // bytes left of the current chunk; -1: a size line is next
    QByteArray m_rawBody;
    bool m_busy = false;
    int m_generation = 0;
};
