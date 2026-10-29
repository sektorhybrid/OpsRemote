// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "localservers.h"
#include "appdirs.h"
#include "listenaddress.h"
#include "remotefiles.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QSpinBox>
#include <QStorageInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QVBoxLayout>
#include <QtEndian>

#include <algorithm>

QString localServerFolder(const LocalServerSettings &settings)
{
    if (settings.folder == QStringLiteral("custom") && !settings.customFolder.isEmpty())
        return QDir::cleanPath(QDir(portableHome()).absoluteFilePath(settings.customFolder));
    return portableFilePath(QStringLiteral("shared"));
}

QString localServerPath(const QString &root, const QString &requested, bool forWriting)
{
    QString name = requested;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1Char('/')))
        name.remove(0, 1);
    const QString cleanRoot = QDir::cleanPath(root);
    const QString path = QDir::cleanPath(cleanRoot + QLatin1Char('/') + name);
    if (path != cleanRoot && !path.startsWith(cleanRoot + QLatin1Char('/')))
        return {};
    // A link inside the folder may not lead out of it either: the nearest
    // part of the path that exists (the file, or a folder above a new one)
    // must really be inside.
    const QString realRoot = QFileInfo(cleanRoot).canonicalFilePath();
    QString existing = path;
    while (!QFileInfo::exists(existing) && existing.size() > cleanRoot.size())
        existing = QFileInfo(existing).absolutePath();
    const QString real = QFileInfo(existing).canonicalFilePath();
    if (realRoot.isEmpty() || real.isEmpty()
        || (real != realRoot && !real.startsWith(realRoot + QLatin1Char('/'))))
        return {};
    if (forWriting) {
        const QFileInfo target(path);
        if (path == cleanRoot || target.isSymLink() || target.isDir())
            return {};
    }
    return path;
}

namespace {

QString now()
{
    return QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
}

QString sizeText(qint64 bytes)
{
    if (bytes < 10 * 1024)
        return QStringLiteral("%1 bytes").arg(bytes);
    if (bytes < 10 * 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
}

// Concurrent clients and transfers per server, and how long a client may
// stay silent.
constexpr int MaximumHttpClients = 32;
constexpr int MaximumTftpTransfers = 16;
constexpr int HttpIdleMs = 60000;

// A temporary name next to a file, unique to this upload.
QString temporarySibling(const QString &path)
{
    const QFileInfo info(path);
    return QDir(info.absolutePath())
        .filePath(QStringLiteral(".%1.%2.part").arg(info.fileName())
                      .arg(QRandomGenerator::global()->generate64(), 0, 16));
}

// Whether the folder has room for bytes more (with a little to spare).
bool hasRoom(const QString &folder, qint64 bytes)
{
    const QStorageInfo storage(folder);
    return !storage.isValid() || storage.bytesAvailable() < 0
           || storage.bytesAvailable() > bytes + 16 * 1024 * 1024;
}

QString bindProblem(int port, const QString &error)
{
#ifndef Q_OS_WIN
    if (port < 1024)
        return QStringLiteral("Port %1 needs administrator rights on Linux. Choose a port of 1024 or "
                              "higher (and point the device to it), or allow it once with: sudo "
                              "sysctl net.ipv4.ip_unprivileged_port_start=%1")
            .arg(port);
#endif
    return QStringLiteral("Cannot use port %1: %2").arg(port).arg(error);
}

// TFTP opcodes and errors.
enum : quint16 { Rrq = 1, Wrq = 2, Data = 3, Ack = 4, Error = 5, Oack = 6 };
enum : quint16 { NotFound = 1, AccessViolation = 2, IllegalOperation = 4, UnknownTid = 5, FileExists = 6 };

QByteArray packet(quint16 opcode, quint16 number)
{
    QByteArray bytes(4, '\0');
    qToBigEndian(opcode, bytes.data());
    qToBigEndian(number, bytes.data() + 2);
    return bytes;
}

QByteArray errorPacket(quint16 code, const QString &message)
{
    return packet(Error, code) + message.toUtf8() + '\0';
}

}

// ---- TFTP ----

struct TftpServer::Transfer
{
    QUdpSocket *socket = nullptr;
    QHostAddress peer;
    quint16 port = 0;
    QFile *file = nullptr;
    QString name;
    QString target; // uploads: the final name of file
    bool upload = false;
    quint16 block = 0; // last block sent (downloads) or acknowledged (uploads)
    int blockSize = 512;
    bool lastSent = false;
    bool waitingForOptionAck = false;
    QByteArray lastPacket;
    QTimer *timer = nullptr;
    int retries = 0;
    qint64 bytes = 0;
};

TftpServer::~TftpServer()
{
    stop();
}

bool TftpServer::isRunning() const
{
    return m_socket != nullptr;
}

bool TftpServer::start(const LocalServerSettings &settings, QString *error)
{
    stop();
    m_root = localServerFolder(settings);
    m_uploads = settings.uploads;
    m_maxUpload = qint64(settings.maxUploadMb) * 1024 * 1024;
    QDir().mkpath(m_root);
    QHostAddress address;
    if (!ListenAddress::resolve(settings.listen, &address, error))
        return false;
    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(address, quint16(settings.port))) {
        *error = bindProblem(settings.port, m_socket->errorString());
        delete m_socket;
        m_socket = nullptr;
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &TftpServer::readRequests);
    emit logged(QStringLiteral("%1  TFTP started on port %2 (%3), %4: %5")
                    .arg(now())
                    .arg(settings.port)
                    .arg(ListenAddress::describe(settings.listen),
                         m_uploads ? QStringLiteral("downloads and uploads") : QStringLiteral("read-only"),
                         QDir::toNativeSeparators(m_root)));
    return true;
}

void TftpServer::stop()
{
    while (!m_transfers.isEmpty())
        finish(m_transfers.constFirst(), QStringLiteral("stopped"));
    if (m_socket) {
        delete m_socket;
        m_socket = nullptr;
        emit logged(QStringLiteral("%1  TFTP stopped").arg(now()));
    }
}

void TftpServer::readRequests()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram(4096);
        const QByteArray data = datagram.data();
        if (data.size() < 4)
            continue;
        const quint16 opcode = qFromBigEndian<quint16>(data.constData());
        if (opcode != Rrq && opcode != Wrq) {
            m_socket->writeDatagram(errorPacket(IllegalOperation, QStringLiteral("Expected a request")),
                                    datagram.senderAddress(), quint16(datagram.senderPort()));
            continue;
        }
        // filename\0 mode\0 [option\0 value\0]...
        const QList<QByteArray> fields = data.mid(2).split('\0');
        if (fields.size() < 2 || fields.at(0).isEmpty())
            continue;
        const QHostAddress sender = datagram.senderAddress();
        const quint16 senderPort = quint16(datagram.senderPort());
        // A repeated request (the device did not hear the answer yet) is
        // already being served.
        const bool running = std::any_of(m_transfers.cbegin(), m_transfers.cend(), [&](const Transfer *t) {
            return t->port == senderPort && t->peer.isEqual(sender);
        });
        if (running)
            continue;
        const QByteArray mode = fields.at(1).toLower();
        if (mode != "octet" && mode != "netascii") {
            m_socket->writeDatagram(errorPacket(IllegalOperation, QStringLiteral("Only octet and netascii transfers")),
                                    sender, senderPort);
            continue;
        }
        if (m_transfers.size() >= MaximumTftpTransfers) {
            m_socket->writeDatagram(errorPacket(0, QStringLiteral("Server busy, try again")), sender, senderPort);
            emit logged(QStringLiteral("%1  TFTP refused %2: %3 transfers already running")
                            .arg(now(), sender.toString()).arg(MaximumTftpTransfers));
            continue;
        }
        QHash<QString, QString> options;
        for (int i = 2; i + 1 < fields.size(); i += 2)
            options.insert(QString::fromLatin1(fields.at(i)).toLower(), QString::fromLatin1(fields.at(i + 1)));
        startTransfer(datagram.senderAddress(), quint16(datagram.senderPort()), opcode == Wrq,
                      QString::fromUtf8(fields.at(0)), options);
    }
}

void TftpServer::startTransfer(const QHostAddress &peer, quint16 port, bool upload,
                               const QString &name, const QHash<QString, QString> &options)
{
    auto *transfer = new Transfer;
    transfer->peer = peer;
    transfer->port = port;
    transfer->name = name;
    transfer->upload = upload;
    // Each transfer answers from a port of its own (its transfer id).
    transfer->socket = new QUdpSocket(this);
    // From the address the request came to, when the server listens on one.
    transfer->socket->bind(m_socket->localAddress(), 0);
    transfer->timer = new QTimer(transfer->socket);
    transfer->timer->setInterval(3000);
    m_transfers.append(transfer);
    const auto refuse = [&](quint16 code, const QString &message) {
        transfer->socket->writeDatagram(errorPacket(code, message), peer, port);
        finish(transfer, message);
    };
    const QString who = peer.toString().remove(QStringLiteral("::ffff:"));
    emit logged(QStringLiteral("%1  TFTP %2 %3 %4").arg(now(), who, upload ? QStringLiteral("uploads") : QStringLiteral("gets"), name));

    if (transfer->socket->state() != QAbstractSocket::BoundState)
        return refuse(0, QStringLiteral("Cannot open a transfer port"));
    const QString path = localServerPath(m_root, name, upload);
    if (path.isEmpty())
        return refuse(AccessViolation, QStringLiteral("Outside the shared folder, or not a file"));
    if (upload) {
        if (!m_uploads)
            return refuse(AccessViolation, QStringLiteral("Uploads are off"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        // Written to a file of its own, put in place when complete.
        transfer->target = path;
        transfer->file = new QFile(temporarySibling(path));
        if (!transfer->file->open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return refuse(AccessViolation, QStringLiteral("Cannot write the file"));
    } else {
        transfer->file = new QFile(path);
        if (!QFileInfo(path).isFile() || !transfer->file->open(QIODevice::ReadOnly))
            return refuse(NotFound, QStringLiteral("File not found"));
    }

    // Options: a bigger block size and the transfer size, when asked for.
    // Options that are not numbers are left out of the answer (RFC 2347).
    QByteArray acknowledged;
    if (options.contains(QStringLiteral("blksize"))) {
        bool ok = false;
        const int asked = options.value(QStringLiteral("blksize")).toInt(&ok);
        if (ok && asked > 0) {
            const int size = qBound(8, asked, 65464);
            transfer->blockSize = size;
            acknowledged += "blksize" + QByteArray(1, '\0') + QByteArray::number(size) + '\0';
        }
    }
    if (options.contains(QStringLiteral("tsize"))) {
        bool ok = false;
        const qint64 announced = options.value(QStringLiteral("tsize")).toLongLong(&ok);
        if (ok && announced >= 0) {
            if (upload && announced > m_maxUpload)
                return refuse(3, QStringLiteral("Larger than this server accepts"));
            if (upload && !hasRoom(QFileInfo(path).absolutePath(), announced))
                return refuse(3, QStringLiteral("Disk full"));
            const qint64 size = upload ? announced : transfer->file->size();
            acknowledged += "tsize" + QByteArray(1, '\0') + QByteArray::number(size) + '\0';
        }
    }
    connect(transfer->socket, &QUdpSocket::readyRead, this, [this, transfer] { handleTransferPacket(transfer); });
    connect(transfer->timer, &QTimer::timeout, this, [this, transfer] {
        if (++transfer->retries > 5)
            return finish(transfer, QStringLiteral("timed out"));
        transfer->socket->writeDatagram(transfer->lastPacket, transfer->peer, transfer->port);
        transfer->timer->start();
    });
    if (!acknowledged.isEmpty()) {
        transfer->lastPacket = QByteArray(2, '\0') + acknowledged;
        qToBigEndian<quint16>(Oack, transfer->lastPacket.data());
        transfer->waitingForOptionAck = !upload;
        transfer->socket->writeDatagram(transfer->lastPacket, peer, port);
        transfer->timer->start();
    } else if (upload) {
        transfer->lastPacket = packet(Ack, 0);
        transfer->socket->writeDatagram(transfer->lastPacket, peer, port);
        transfer->timer->start();
    } else {
        sendBlock(transfer);
    }
}

void TftpServer::sendBlock(Transfer *transfer)
{
    ++transfer->block;
    const QByteArray chunk = transfer->file->read(transfer->blockSize);
    transfer->bytes += chunk.size();
    transfer->lastSent = chunk.size() < transfer->blockSize;
    transfer->lastPacket = packet(Data, transfer->block) + chunk;
    transfer->retries = 0;
    transfer->socket->writeDatagram(transfer->lastPacket, transfer->peer, transfer->port);
    transfer->timer->start();
}

void TftpServer::handleTransferPacket(Transfer *transfer)
{
    while (transfer->socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = transfer->socket->receiveDatagram(65536 + 4);
        if (datagram.senderPort() != transfer->port || datagram.senderAddress() != transfer->peer) {
            transfer->socket->writeDatagram(errorPacket(UnknownTid, QStringLiteral("Unknown transfer")),
                                            datagram.senderAddress(), quint16(datagram.senderPort()));
            continue;
        }
        const QByteArray data = datagram.data();
        if (data.size() < 4)
            continue;
        const quint16 opcode = qFromBigEndian<quint16>(data.constData());
        const quint16 number = qFromBigEndian<quint16>(data.constData() + 2);
        if (opcode == Error)
            return finish(transfer, QStringLiteral("cancelled by the device"));
        if (!transfer->upload && opcode == Ack) {
            if (transfer->waitingForOptionAck && number == 0) {
                transfer->waitingForOptionAck = false;
                sendBlock(transfer);
            } else if (!transfer->waitingForOptionAck && number == transfer->block) {
                if (transfer->lastSent)
                    return finish(transfer, QStringLiteral("sent %1").arg(sizeText(transfer->bytes)));
                sendBlock(transfer);
            }
        } else if (transfer->upload && opcode == Data) {
            const quint16 expected = quint16(transfer->block + 1);
            if (number == expected) {
                const QByteArray chunk = data.mid(4);
                if (transfer->bytes + chunk.size() > m_maxUpload) {
                    transfer->socket->writeDatagram(errorPacket(3, QStringLiteral("Larger than this server accepts")),
                                                    transfer->peer, transfer->port);
                    return finish(transfer, QStringLiteral("refused: larger than %1").arg(sizeText(m_maxUpload)));
                }
                if (transfer->file->write(chunk) != chunk.size()) {
                    transfer->socket->writeDatagram(errorPacket(AccessViolation, QStringLiteral("Disk full")),
                                                    transfer->peer, transfer->port);
                    return finish(transfer, QStringLiteral("cannot write (disk full?)"));
                }
                transfer->bytes += chunk.size();
                transfer->block = expected;
                transfer->retries = 0;
                transfer->lastPacket = packet(Ack, number);
                transfer->socket->writeDatagram(transfer->lastPacket, transfer->peer, transfer->port);
                transfer->timer->start();
                if (chunk.size() < transfer->blockSize) {
                    // Complete: put in place in one step, replacing an
                    // older copy only now.
                    QString error;
                    const bool flushed = transfer->file->flush();
                    transfer->file->close();
                    const QString target = localServerPath(m_root, transfer->name, true);
                    if (!flushed || transfer->file->error() != QFileDevice::NoError || target.isEmpty()
                        || !replaceFile(transfer->file->fileName(), target, &error)) {
                        transfer->file->remove();
                        return finish(transfer, QStringLiteral("cannot save the received file %1").arg(error));
                    }
                    return finish(transfer, QStringLiteral("received %1").arg(sizeText(transfer->bytes)));
                }
            } else if (number == transfer->block) {
                // Our acknowledgement was lost: repeat it.
                transfer->socket->writeDatagram(transfer->lastPacket, transfer->peer, transfer->port);
            }
        }
    }
}

void TftpServer::finish(Transfer *transfer, const QString &result)
{
    m_transfers.removeOne(transfer);
    if (transfer->file) {
        const bool incomplete = transfer->upload && transfer->file->isOpen();
        transfer->file->close();
        if (incomplete)
            transfer->file->remove(); // an unfinished .part file
        delete transfer->file;
    }
    transfer->socket->deleteLater();
    emit logged(QStringLiteral("%1  TFTP %2: %3").arg(now(), transfer->name, result));
    delete transfer;
}

// ---- HTTP ----

struct HttpFileServer::Client
{
    QTcpSocket *socket = nullptr;
    QByteArray buffer;
    bool headersDone = false;
    QString method;
    QString urlPath;
    QHash<QByteArray, QByteArray> headers;
    qint64 contentLength = 0;
    qint64 received = 0;
    QFile *upload = nullptr; // the request body, while it arrives
    QFile *download = nullptr; // a file being sent
    QString who;
    QTimer *idle = nullptr; // closes a client that stays silent
    QElapsedTimer uploadClock;
};

HttpFileServer::~HttpFileServer()
{
    stop();
}

bool HttpFileServer::isRunning() const
{
    return m_server != nullptr;
}

bool HttpFileServer::start(const LocalServerSettings &settings, QString *error)
{
    stop();
    m_root = localServerFolder(settings);
    m_uploads = settings.uploads;
    m_maxUpload = qint64(settings.maxUploadMb) * 1024 * 1024;
    QDir().mkpath(m_root);
    QHostAddress address;
    if (!ListenAddress::resolve(settings.listen, &address, error))
        return false;
    m_server = new QTcpServer(this);
    if (!m_server->listen(address, quint16(settings.port))) {
        *error = bindProblem(settings.port, m_server->errorString());
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QTcpServer::newConnection, this, &HttpFileServer::accept);
    emit logged(QStringLiteral("%1  HTTP started on port %2 (%3), %4: %5")
                    .arg(now())
                    .arg(settings.port)
                    .arg(ListenAddress::describe(settings.listen),
                         m_uploads ? QStringLiteral("downloads and uploads") : QStringLiteral("read-only"),
                         QDir::toNativeSeparators(m_root)));
    return true;
}

void HttpFileServer::stop()
{
    while (!m_clients.isEmpty())
        drop(m_clients.constFirst());
    if (m_server) {
        delete m_server;
        m_server = nullptr;
        emit logged(QStringLiteral("%1  HTTP stopped").arg(now()));
    }
}

void HttpFileServer::accept()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if (m_clients.size() >= MaximumHttpClients) {
            socket->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            continue;
        }
        auto *client = new Client;
        client->socket = socket;
        client->who = client->socket->peerAddress().toString().remove(QStringLiteral("::ffff:"));
        client->idle = new QTimer(client->socket);
        client->idle->setSingleShot(true);
        client->idle->setInterval(HttpIdleMs);
        m_clients.append(client);
        connect(client->socket, &QTcpSocket::readyRead, this, [this, client] { readClient(client); });
        connect(client->socket, &QTcpSocket::bytesWritten, client->idle, qOverload<>(&QTimer::start));
        connect(client->socket, &QTcpSocket::disconnected, this, [this, client] { drop(client); });
        connect(client->idle, &QTimer::timeout, this, [this, client] {
            emit logged(QStringLiteral("%1  HTTP %2: closed after %3 s without traffic")
                            .arg(now(), client->who).arg(HttpIdleMs / 1000));
            drop(client);
        });
        client->idle->start();
    }
}

void HttpFileServer::readClient(Client *client)
{
    client->idle->start();
    if (client->upload) {
        // The body of an upload goes straight to disk.
        const QByteArray data = client->socket->read(client->contentLength - client->received);
        if (client->upload->write(data) != data.size()) {
            emit logged(QStringLiteral("%1  HTTP %2: cannot write the upload (%3)")
                            .arg(now(), client->urlPath, client->upload->errorString()));
            return drop(client);
        }
        client->received += data.size();
        // A body trickled in to hold the server: at least 16 KB/s on
        // average after the first ten minutes.
        const qint64 seconds = client->uploadClock.elapsed() / 1000;
        if (seconds > 600 && client->received < seconds * 16 * 1024) {
            emit logged(QStringLiteral("%1  HTTP %2: upload too slow, closed").arg(now(), client->urlPath));
            return drop(client);
        }
        if (client->received >= client->contentLength)
            finishUpload(client);
        return;
    }
    if (client->headersDone) {
        // A request is answered once; more bytes are not read.
        client->socket->readAll();
        return;
    }
    client->buffer += client->socket->read(65536 + 4 - client->buffer.size());
    const int end = client->buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (client->buffer.size() > 65536)
            sendSimple(client, 431, QStringLiteral("Request headers too large"));
        return;
    }
    client->headersDone = true;
    const QList<QByteArray> lines = client->buffer.left(end).split('\n');
    // "METHOD /path HTTP/1.x", strictly.
    const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
    static const QRegularExpression method(QStringLiteral("^[A-Z]{1,16}$"));
    if (request.size() != 3 || !method.match(QString::fromLatin1(request.at(0))).hasMatch()
        || !request.at(1).startsWith('/'))
        return sendSimple(client, 400, QStringLiteral("Bad request"));
    if (request.at(2) != "HTTP/1.1" && request.at(2) != "HTTP/1.0")
        return sendSimple(client, 505, QStringLiteral("HTTP version not supported"));
    client->method = QString::fromLatin1(request.at(0));
    client->urlPath = QUrl::fromPercentEncoding(request.at(1).split('?').value(0));
    if (client->urlPath.contains(QChar(0)))
        return sendSimple(client, 400, QStringLiteral("Bad request"));
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        const QByteArray name = colon > 0 ? line.left(colon).toLower() : QByteArray();
        if (name.isEmpty() || name.contains(' ') || name.contains('\t'))
            return sendSimple(client, 400, QStringLiteral("Bad request header"));
        const QByteArray value = line.mid(colon + 1).trimmed();
        // Two different lengths make the body's end ambiguous.
        if (client->headers.contains(name) && name == "content-length" && client->headers.value(name) != value)
            return sendSimple(client, 400, QStringLiteral("Conflicting Content-Length"));
        client->headers.insert(name, value);
    }
    if (client->headers.contains("transfer-encoding"))
        return sendSimple(client, 501, QStringLiteral("Chunked uploads are not supported; send a Content-Length"));
    client->buffer.remove(0, end + 4);
    respond(client);
}

void HttpFileServer::respond(Client *client)
{
    const QString path = localServerPath(m_root, client->urlPath);
    emit logged(QStringLiteral("%1  HTTP %2 %3 %4").arg(now(), client->who, client->method, client->urlPath));
    if (path.isEmpty())
        return sendSimple(client, 403, QStringLiteral("Outside the shared folder"));
    if (client->method == QStringLiteral("GET") || client->method == QStringLiteral("HEAD")) {
        const QFileInfo info(path);
        if (info.isDir())
            return sendListing(client, path, client->urlPath);
        if (info.isFile())
            return sendFile(client, path);
        return sendSimple(client, 404, QStringLiteral("Not found"));
    }
    if (client->method == QStringLiteral("PUT") || client->method == QStringLiteral("POST")) {
        if (!m_uploads)
            return sendSimple(client, 403, QStringLiteral("Uploads are off on this server"));
        const QByteArray length = client->headers.value("content-length");
        static const QRegularExpression digits(QStringLiteral("^[0-9]{1,18}$"));
        if (!digits.match(QString::fromLatin1(length)).hasMatch() || length.toLongLong() <= 0)
            return sendSimple(client, 411, QStringLiteral("Content-Length needed"));
        client->contentLength = length.toLongLong();
        if (client->contentLength > m_maxUpload)
            return sendSimple(client, 413, QStringLiteral("Larger than this server accepts (%1)")
                                               .arg(sizeText(m_maxUpload)));
        if (client->method == QStringLiteral("PUT") && localServerPath(m_root, client->urlPath, true).isEmpty())
            return sendSimple(client, 403, QStringLiteral("Not a file that can be written"));
        // The body is kept in a temporary file next to its destination.
        const QString folder = client->method == QStringLiteral("PUT") ? QFileInfo(path).absolutePath() : path;
        if (!QFileInfo(folder).isDir() && !QDir().mkpath(folder))
            return sendSimple(client, 409, QStringLiteral("No such folder"));
        if (!hasRoom(folder, client->contentLength))
            return sendSimple(client, 507, QStringLiteral("Not enough free space"));
        client->upload = new QFile(temporarySibling(QDir(folder).filePath(QStringLiteral("upload"))));
        if (!client->upload->open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return sendSimple(client, 500, QStringLiteral("Cannot write in that folder"));
        client->uploadClock.start();
        const QByteArray already = client->buffer.left(client->contentLength);
        if (client->upload->write(already) != already.size())
            return sendSimple(client, 500, QStringLiteral("Cannot write in that folder"));
        client->received = already.size();
        client->buffer.clear();
        if (client->received >= client->contentLength)
            finishUpload(client);
        return;
    }
    sendSimple(client, 405, QStringLiteral("Method not allowed"));
}

void HttpFileServer::sendFile(Client *client, const QString &path)
{
    client->download = new QFile(path);
    if (!client->download->open(QIODevice::ReadOnly))
        return sendSimple(client, 403, QStringLiteral("Cannot read the file"));
    QByteArray head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: "
                      + QByteArray::number(client->download->size())
                      + "\r\nConnection: close\r\n\r\n";
    client->socket->write(head);
    if (client->method == QStringLiteral("HEAD")) {
        client->socket->disconnectFromHost();
        return;
    }
    // Sent in pieces as the socket takes them, so large files use no memory.
    const auto pump = [this, client] {
        if (!client->download)
            return;
        while (client->socket->bytesToWrite() < 256 * 1024 && !client->download->atEnd())
            client->socket->write(client->download->read(64 * 1024));
        if (client->download->atEnd() && client->socket->bytesToWrite() == 0) {
            emit logged(QStringLiteral("%1  HTTP %2: sent %3")
                            .arg(now(), client->urlPath, sizeText(client->download->size())));
            client->download->close();
            client->socket->disconnectFromHost();
        }
    };
    connect(client->socket, &QTcpSocket::bytesWritten, this, pump);
    pump();
}

void HttpFileServer::sendListing(Client *client, const QString &path, const QString &urlPath)
{
    QString base = urlPath;
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    QString html = QStringLiteral("<!doctype html><meta charset=utf-8><title>OpsRemote: %1</title>"
                                  "<style>body{font:14px sans-serif;margin:2em}td{padding:2px 12px}"
                                  "</style><h2>%1</h2><table>")
                       .arg(base.toHtmlEscaped());
    if (base != QStringLiteral("/"))
        html += QStringLiteral("<tr><td><a href=\"../\">../</a></td></tr>");
    for (const auto &entry : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                                       QDir::DirsFirst | QDir::Name)) {
        const QString name = entry.fileName() + (entry.isDir() ? QStringLiteral("/") : QString());
        html += QStringLiteral("<tr><td><a href=\"%1\">%2</a></td><td>%3</td></tr>")
                    .arg(QString::fromLatin1(QUrl::toPercentEncoding(name, "/")), name.toHtmlEscaped(),
                         entry.isDir() ? QString() : sizeText(entry.size()));
    }
    html += QStringLiteral("</table>");
    if (m_uploads)
        html += QStringLiteral("<form method=post enctype=multipart/form-data><p><input type=file "
                               "name=file> <button>Upload here</button></form>");
    const QByteArray body = html.toUtf8();
    client->socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                          + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n");
    if (client->method != QStringLiteral("HEAD"))
        client->socket->write(body);
    client->socket->disconnectFromHost();
}

void HttpFileServer::sendSimple(Client *client, int status, const QString &text)
{
    static const QHash<int, QByteArray> reasons{{200, "OK"}, {303, "See Other"}, {400, "Bad Request"},
                                                {403, "Forbidden"}, {404, "Not Found"},
                                                {405, "Method Not Allowed"}, {409, "Conflict"},
                                                {411, "Length Required"}, {413, "Content Too Large"},
                                                {431, "Request Header Fields Too Large"},
                                                {500, "Internal Server Error"}, {501, "Not Implemented"},
                                                {505, "HTTP Version Not Supported"},
                                                {507, "Insufficient Storage"}};
    const QByteArray body = text.toUtf8() + '\n';
    client->socket->write("HTTP/1.1 " + QByteArray::number(status) + ' ' + reasons.value(status, "Error")
                          + "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: "
                          + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    client->socket->disconnectFromHost();
    if (status >= 400)
        emit logged(QStringLiteral("%1  HTTP %2: %3").arg(now(), client->urlPath, text));
}

// PUT: the body is the file. POST (a browser form): the file is the first
// part of a multipart body, between its headers and the closing boundary.
void HttpFileServer::finishUpload(Client *client)
{
    QFile *body = client->upload;
    client->upload = nullptr;
    const bool written = body->flush() && body->error() == QFileDevice::NoError;
    body->close();
    if (!written) {
        body->remove();
        delete body;
        return sendSimple(client, 500, QStringLiteral("Cannot save the file"));
    }
    if (client->method == QStringLiteral("PUT")) {
        // Checked again: the folder may have changed while the body came.
        const QString targetPath = localServerPath(m_root, client->urlPath, true);
        QString error;
        if (targetPath.isEmpty() || !replaceFile(body->fileName(), targetPath, &error)) {
            body->remove();
            delete body;
            return sendSimple(client, 500, QStringLiteral("Cannot save the file"));
        }
        emit logged(QStringLiteral("%1  HTTP %2: received %3").arg(now(), client->urlPath, sizeText(client->received)));
        delete body;
        return sendSimple(client, 200, QStringLiteral("Saved"));
    }
    const QString targetPath = localServerPath(m_root, client->urlPath);
    const QByteArray type = client->headers.value("content-type");
    const int at = type.indexOf("boundary=");
    QString saved;
    if (at >= 0 && body->open(QIODevice::ReadOnly)) {
        const QByteArray boundary = "--" + type.mid(at + 9).trimmed().replace("\"", "");
        const QByteArray head = body->read(65536);
        const int headersEnd = head.indexOf("\r\n\r\n");
        const QByteArray partHeaders = head.left(headersEnd);
        const int name = partHeaders.indexOf("filename=\"");
        QString fileName;
        if (name >= 0)
            fileName = QFileInfo(QString::fromUtf8(partHeaders.mid(name + 10).split('"').value(0))).fileName();
        if (!RemotePaths::isSafeLocalName(fileName))
            fileName.clear();
        // The closing boundary is near the end.
        const qint64 total = body->size();
        const qint64 tailStart = qMax<qint64>(0, total - 4096);
        body->seek(tailStart);
        const QByteArray tail = body->readAll();
        const int closing = tail.lastIndexOf("\r\n" + boundary);
        const QString destination = targetPath.isEmpty() || fileName.isEmpty()
            ? QString()
            : localServerPath(m_root, client->urlPath + QLatin1Char('/') + fileName, true);
        if (headersEnd >= 0 && closing >= 0 && !destination.isEmpty()) {
            const qint64 start = headersEnd + 4;
            const qint64 end = tailStart + closing;
            // The file is copied out whole, then put in place in one step.
            QFile out(temporarySibling(destination));
            bool complete = end >= start && out.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                            && body->seek(start);
            for (qint64 left = end - start; complete && left > 0;) {
                const QByteArray piece = body->read(qMin<qint64>(left, 1 << 20));
                complete = !piece.isEmpty() && out.write(piece) == piece.size();
                left -= piece.size();
            }
            complete = complete && out.flush();
            out.close();
            QString error;
            if (complete && replaceFile(out.fileName(), destination, &error)) {
                saved = fileName;
                emit logged(QStringLiteral("%1  HTTP %2: received %3 (%4)")
                                .arg(now(), fileName, sizeText(end - start), client->who));
            } else {
                out.remove();
            }
        }
        body->close();
    }
    body->remove();
    delete body;
    if (saved.isEmpty())
        return sendSimple(client, 500, QStringLiteral("The upload could not be read"));
    // Back to the folder, which now lists the file.
    client->socket->write("HTTP/1.1 303 See Other\r\nLocation: " + QUrl::toPercentEncoding(client->urlPath, "/")
                          + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    client->socket->disconnectFromHost();
}

void HttpFileServer::drop(Client *client)
{
    if (!m_clients.removeOne(client))
        return;
    if (client->upload) {
        client->upload->close();
        client->upload->remove(); // an unfinished upload
        delete client->upload;
    }
    delete client->download;
    client->socket->disconnect(this);
    client->socket->deleteLater();
    delete client;
}

// ---- The window ----

LocalServersWindow::LocalServersWindow(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Local Servers"));
    resize(640, 600);
    m_tftp = new TftpServer(this);
    m_http = new HttpFileServer(this);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    for (QObject *server : {static_cast<QObject *>(m_tftp), static_cast<QObject *>(m_http)})
        connect(server, SIGNAL(logged(QString)), m_log, SLOT(appendPlainText(QString)));

    QStringList addresses;
    for (const auto &address : QNetworkInterface::allAddresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
            addresses.append(address.toString());
    }
    auto *intro = new QLabel(QStringLiteral("Files for network equipment: firmware, configurations, "
                                            "logs. This computer: %1. The servers run only while "
                                            "this window is open. They have no passwords and no "
                                            "encryption: anyone who can reach them can read the "
                                            "folder (and write to it, with uploads on), so listen "
                                            "only where the devices are.")
                                 .arg(addresses.isEmpty() ? QStringLiteral("no network address")
                                                          : addresses.join(QStringLiteral(", "))),
                             this);
    intro->setWordWrap(true);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(buildCard(true));
    layout->addWidget(buildCard(false));
    layout->addWidget(new QLabel(QStringLiteral("Log:"), this));
    layout->addWidget(m_log, 1);
    refresh();
}

QWidget *LocalServersWindow::buildCard(bool tftp)
{
    const auto current = loadLocalServersPreferences();
    const LocalServerSettings settings = tftp ? current.tftp : current.http;
    auto *box = new QGroupBox(tftp ? QStringLiteral("TFTP server") : QStringLiteral("HTTP server"), this);
    auto *form = new QFormLayout(box);
    auto *button = new QPushButton(box);
    auto *status = new QLabel(box);
    status->setWordWrap(true);
    auto *top = new QHBoxLayout;
    top->addWidget(button);
    top->addWidget(status, 1);
    form->addRow(top);
    auto *port = new QSpinBox(box);
    port->setRange(1, 65535);
    port->setValue(settings.port);
    form->addRow(QStringLiteral("Port:"), port);
    auto *listen = ListenAddress::makeBox(settings.listen, box);
    form->addRow(QStringLiteral("Listen on:"), listen);
    auto *folderChoice = new QComboBox(box);
    folderChoice->addItem(QStringLiteral("The package's shared folder"), QStringLiteral("shared"));
    folderChoice->addItem(QStringLiteral("Another folder"), QStringLiteral("custom"));
    folderChoice->setCurrentIndex(qMax(0, folderChoice->findData(settings.folder)));
    auto *custom = new QLineEdit(settings.customFolder, box);
    auto *browse = new QPushButton(QStringLiteral("Browse…"), box);
    auto *open = new QPushButton(QStringLiteral("Open"), box);
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(folderChoice);
    folderRow->addWidget(custom, 1);
    folderRow->addWidget(browse);
    folderRow->addWidget(open);
    form->addRow(QStringLiteral("Folder:"), folderRow);
    auto *uploads = new QComboBox(box);
    uploads->addItem(QStringLiteral("Read-only (devices can only download)"), false);
    uploads->addItem(QStringLiteral("Downloads and uploads"), true);
    uploads->setCurrentIndex(settings.uploads ? 1 : 0);
    form->addRow(QStringLiteral("Access:"), uploads);
    auto *largest = new QSpinBox(box);
    largest->setRange(1, 1024 * 1024);
    largest->setSuffix(QStringLiteral(" MB"));
    largest->setValue(settings.maxUploadMb);
    largest->setToolTip(QStringLiteral("Larger uploads are refused, as are uploads the disk has no "
                                       "room for."));
    form->addRow(QStringLiteral("Largest upload:"), largest);
    const auto updateUploads = [uploads, largest] { largest->setEnabled(uploads->currentData().toBool()); };
    // Every change is remembered at once; a running server picks it up on
    // its next start.
    const auto save = [tftp, port, folderChoice, custom, uploads, listen, largest] {
        auto all = loadLocalServersPreferences();
        LocalServerSettings &value = tftp ? all.tftp : all.http;
        value.port = port->value();
        value.listen = listen->currentData().toString();
        value.maxUploadMb = largest->value();
        value.folder = folderChoice->currentData().toString();
        value.customFolder = custom->text().trimmed();
        value.uploads = uploads->currentData().toBool();
        saveLocalServersPreferences(all);
    };
    const auto updateFolder = [folderChoice, custom, browse] {
        const bool own = folderChoice->currentData().toString() == QStringLiteral("custom");
        custom->setEnabled(own);
        browse->setEnabled(own);
    };
    connect(port, &QSpinBox::valueChanged, this, save);
    connect(folderChoice, &QComboBox::currentIndexChanged, this, [save, updateFolder] { save(); updateFolder(); });
    connect(custom, &QLineEdit::editingFinished, this, save);
    connect(uploads, &QComboBox::currentIndexChanged, this, [save, updateUploads] { save(); updateUploads(); });
    connect(listen, &QComboBox::currentIndexChanged, this, save);
    connect(largest, &QSpinBox::valueChanged, this, save);
    updateUploads();
    connect(browse, &QPushButton::clicked, this, [this, custom, save] {
        const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Folder to Share"), custom->text());
        if (!folder.isEmpty()) {
            custom->setText(folder);
            save();
        }
    });
    connect(open, &QPushButton::clicked, this, [tftp] {
        const auto all = loadLocalServersPreferences();
        const QString folder = localServerFolder(tftp ? all.tftp : all.http);
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    connect(button, &QPushButton::clicked, this, [this, tftp] { toggle(tftp); });
    updateFolder();
    (tftp ? m_tftpButton : m_httpButton) = button;
    (tftp ? m_tftpStatus : m_httpStatus) = status;
    return box;
}

void LocalServersWindow::toggle(bool tftp)
{
    const auto all = loadLocalServersPreferences();
    QString error;
    if (tftp) {
        if (m_tftp->isRunning())
            m_tftp->stop();
        else if (!m_tftp->start(all.tftp, &error))
            m_log->appendPlainText(QStringLiteral("%1  TFTP: %2").arg(now(), error));
    } else {
        if (m_http->isRunning())
            m_http->stop();
        else if (!m_http->start(all.http, &error))
            m_log->appendPlainText(QStringLiteral("%1  HTTP: %2").arg(now(), error));
    }
    if (!error.isEmpty())
        (tftp ? m_tftpStatus : m_httpStatus)->setText(error);
    refresh();
    if (!error.isEmpty())
        (tftp ? m_tftpStatus : m_httpStatus)->setText(error);
}

// The servers are for the moment: closing the window stops them, so none
// is left open on the network unnoticed.
void LocalServersWindow::closeEvent(QCloseEvent *event)
{
    m_tftp->stop();
    m_http->stop();
    refresh();
    QDialog::closeEvent(event);
}

void LocalServersWindow::reject()
{
    close();
}

void LocalServersWindow::refresh()
{
    const auto all = loadLocalServersPreferences();
    const auto describe = [](bool running, const LocalServerSettings &settings) {
        return running ? QStringLiteral("Running on port %1 (%2), %3.")
                             .arg(settings.port)
                             .arg(ListenAddress::describe(settings.listen),
                                  settings.uploads ? QStringLiteral("downloads and uploads")
                                                   : QStringLiteral("read-only"))
                       : QStringLiteral("Stopped.");
    };
    m_tftpButton->setText(m_tftp->isRunning() ? QStringLiteral("Stop") : QStringLiteral("Start"));
    m_httpButton->setText(m_http->isRunning() ? QStringLiteral("Stop") : QStringLiteral("Start"));
    m_tftpStatus->setText(describe(m_tftp->isRunning(), all.tftp));
    m_httpStatus->setText(describe(m_http->isRunning(), all.http));
}
