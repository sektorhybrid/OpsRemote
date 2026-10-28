// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "vncsession.h"

#include <QMetaObject>
#include <QTimer>

#include <rfb/rfbclient.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace {
// Tag for rfbClientSetClientData: the owning session.
int clientTag;

// libvncclient logs through two global functions; each session runs on its
// own thread, so the last error is kept per thread and becomes the reason
// shown when a connection fails.
thread_local QString lastError;

void logMessage(const char *format, ...)
{
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
    qDebug("VNC: %s", QByteArray(buffer).trimmed().constData());
}

void logError(const char *format, ...)
{
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
    lastError = QString::fromUtf8(buffer).trimmed();
    qInfo("VNC error: %s", qUtf8Printable(lastError));
}

VncSession *ownerOf(rfbClient *client)
{
    return static_cast<VncSession *>(rfbClientGetClientData(client, &clientTag));
}

char *copyString(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    char *copy = static_cast<char *>(std::malloc(size_t(bytes.size()) + 1));
    if (copy)
        std::memcpy(copy, bytes.constData(), size_t(bytes.size()) + 1);
    return copy;
}

// The pixel format the frame is kept in, by colour depth.
void setPixelFormat(rfbClient *client, int depth)
{
    auto &format = client->format;
    format.bigEndian = FALSE;
    format.trueColour = TRUE;
    if (depth == 16) {
        // RGB565, as QImage::Format_RGB16.
        format.bitsPerPixel = 16;
        format.depth = 16;
        format.redMax = 31;
        format.greenMax = 63;
        format.blueMax = 31;
        format.redShift = 11;
        format.greenShift = 5;
        format.blueShift = 0;
    } else if (depth == 8) {
        // BGR233, drawn through a colour table.
        format.bitsPerPixel = 8;
        format.depth = 8;
        format.redMax = 7;
        format.greenMax = 7;
        format.blueMax = 3;
        format.redShift = 0;
        format.greenShift = 3;
        format.blueShift = 6;
    } else {
        // 0x00RRGGBB in a little-endian word: QImage::Format_RGB32.
        format.bitsPerPixel = 32;
        format.depth = 24;
        format.redMax = 255;
        format.greenMax = 255;
        format.blueMax = 255;
        format.redShift = 16;
        format.greenShift = 8;
        format.blueShift = 0;
    }
}

QRgb pixelColor(const unsigned char *pixel, int depth)
{
    if (depth == 16) {
        const quint16 value = quint16(pixel[0] | (pixel[1] << 8));
        return qRgb(((value >> 11) & 31) * 255 / 31, ((value >> 5) & 63) * 255 / 63,
                    (value & 31) * 255 / 31);
    }
    if (depth == 8)
        return qRgb((pixel[0] & 7) * 255 / 7, ((pixel[0] >> 3) & 7) * 255 / 7,
                    ((pixel[0] >> 6) & 3) * 255 / 3);
    return qRgb(pixel[2], pixel[1], pixel[0]);
}
}

struct VncCallbacks
{
    static rfbBool mallocFrameBuffer(rfbClient *client)
    {
        auto *owner = ownerOf(client);
        const int bytes = client->format.bitsPerPixel / 8;
        owner->m_frameBuffer = QByteArray(qsizetype(client->width) * client->height * bytes, '\0');
        client->frameBuffer = reinterpret_cast<uint8_t *>(owner->m_frameBuffer.data());
        owner->allocateFrame(client->width, client->height, owner->m_depth);
        const int width = client->width;
        const int height = client->height;
        QMetaObject::invokeMethod(owner, [owner, width, height] {
            emit owner->desktopResized(width, height);
        }, Qt::QueuedConnection);
        return TRUE;
    }

    static void gotFrameBufferUpdate(rfbClient *client, int x, int y, int w, int h)
    {
        const int stride = client->width * client->format.bitsPerPixel / 8;
        ownerOf(client)->copyFrame(client->frameBuffer, stride, QRect(x, y, w, h));
    }

    static void gotCursorShape(rfbClient *client, int xhot, int yhot, int width, int height,
                               int bytesPerPixel)
    {
        if (!client->rcSource || !client->rcMask || width <= 0 || height <= 0)
            return;
        auto *owner = ownerOf(client);
        QImage shape(width, height, QImage::Format_ARGB32);
        for (int row = 0; row < height; ++row) {
            auto *line = reinterpret_cast<QRgb *>(shape.scanLine(row));
            for (int column = 0; column < width; ++column) {
                const int index = row * width + column;
                const QRgb color = pixelColor(client->rcSource + index * bytesPerPixel, owner->m_depth);
                line[column] = client->rcMask[index] ? (color | 0xFF000000u) : 0u;
            }
        }
        const QPoint hotSpot(xhot, yhot);
        QMetaObject::invokeMethod(owner, [owner, shape, hotSpot] {
            emit owner->pointerChanged(shape, hotSpot);
        }, Qt::QueuedConnection);
    }

    static void gotText(rfbClient *client, QString text)
    {
        // Some servers count a terminating null in the length.
        while (text.endsWith(QChar(0)))
            text.chop(1);
        auto *owner = ownerOf(client);
        QMetaObject::invokeMethod(owner, [owner, text] { emit owner->remoteClipboardText(text); },
                                  Qt::QueuedConnection);
    }

    static void gotLatin1Text(rfbClient *client, const char *text, int length)
    {
        gotText(client, QString::fromLatin1(text, length));
    }

    static void gotUtf8Text(rfbClient *client, const char *text, int length)
    {
        gotText(client, QString::fromUtf8(text, length));
    }

    static char *getPassword(rfbClient *client)
    {
        auto *owner = ownerOf(client);
        if (owner->m_password.isEmpty()) {
            QString user = owner->m_username;
            if (!owner->askCredentials(&user, &owner->m_password, false))
                return nullptr;
        }
        char *copy = copyString(owner->m_password);
        return copy;
    }

    static rfbCredential *getCredential(rfbClient *client, int type)
    {
        auto *owner = ownerOf(client);
        auto *credential = static_cast<rfbCredential *>(std::calloc(1, sizeof(rfbCredential)));
        if (!credential)
            return nullptr;
        if (type == rfbCredentialTypeX509) {
            // No client certificate; the system's CA store checks the server.
            credential->x509Credential.x509CrlVerifyMode = rfbX509CrlVerifyNone;
            return credential;
        }
        if (type != rfbCredentialTypeUser) {
            std::free(credential);
            return nullptr;
        }
        if (owner->m_username.isEmpty() || owner->m_password.isEmpty()) {
            if (!owner->askCredentials(&owner->m_username, &owner->m_password, true)) {
                std::free(credential);
                return nullptr;
            }
        }
        credential->userCredential.username = copyString(owner->m_username);
        credential->userCredential.password = copyString(owner->m_password);
        return credential;
    }
};

VncSession::VncSession(QObject *parent)
    : QObject(parent)
{
}

VncSession::~VncSession()
{
    disconnectFromHost();
    if (m_thread.joinable())
        m_thread.join();
}

void VncSession::connectToHost(const VncConnectionSettings &settings)
{
    disconnectFromHost();
    if (m_thread.joinable())
        m_thread.join();
    m_stopping = false;
    m_finished = false;
    m_thread = std::thread(&VncSession::run, this, settings);
}

void VncSession::disconnectFromHost()
{
    m_stopping = true;
}

void VncSession::retire()
{
    disconnectFromHost();
    // Nobody listens any more; delete once the worker has returned.
    disconnect(this, nullptr, nullptr, nullptr);
    setParent(nullptr);
    auto *poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, [this] {
        if (!m_finished)
            return;
        if (m_thread.joinable())
            m_thread.join();
        deleteLater();
    });
    poll->start(100);
}

void VncSession::run(VncConnectionSettings settings)
{
    rfbClientLog = logMessage;
    rfbClientErr = logError;
    lastError.clear();
    const auto &options = settings.options;
    m_password = settings.password;
    settings.password.fill(QChar(0));
    m_username = settings.username;
    m_depth = options.colorDepth == 16 || options.colorDepth == 8 ? options.colorDepth : 32;

    const auto finish = [this](const QString &reason, bool error) {
        m_password.fill(QChar(0));
        QMetaObject::invokeMethod(this, [this, reason, error] { emit disconnected(reason, error); },
                                  Qt::QueuedConnection);
        m_finished = true;
    };

    rfbClient *client = m_depth == 16 ? rfbGetClient(5, 3, 2)
                        : m_depth == 8 ? rfbGetClient(2, 3, 1)
                                       : rfbGetClient(8, 3, 4);
    if (!client) {
        finish(QStringLiteral("Could not create a VNC session."), true);
        return;
    }
    rfbClientSetClientData(client, &clientTag, this);
    setPixelFormat(client, m_depth);
    client->canHandleNewFBSize = TRUE;
    client->MallocFrameBuffer = VncCallbacks::mallocFrameBuffer;
    client->GotFrameBufferUpdate = VncCallbacks::gotFrameBufferUpdate;
    client->GotCursorShape = VncCallbacks::gotCursorShape;
    client->GetPassword = VncCallbacks::getPassword;
    client->GetCredential = VncCallbacks::getCredential;
    if (options.clipboard == 1) {
        client->GotXCutText = VncCallbacks::gotLatin1Text;
        client->GotXCutTextUTF8 = VncCallbacks::gotUtf8Text;
    }
    client->connectTimeout = 15;
    client->appData.useRemoteCursor = options.remoteCursor == 1;
    client->appData.shareDesktop = TRUE;
    // Quality presets: lossless encodings only for "best"; JPEG (Tight, true
    // colour only) with falling quality and rising compression otherwise.
    if (options.quality == QStringLiteral("best")) {
        client->appData.encodingsString = "copyrect zrle zlib hextile raw";
        client->appData.enableJPEG = FALSE;
        client->appData.compressLevel = 1;
    } else {
        client->appData.encodingsString = "copyrect tight zrle ultra zlib hextile corre rre raw";
        client->appData.enableJPEG = m_depth == 32;
        const bool medium = options.quality == QStringLiteral("medium");
        const bool poor = options.quality == QStringLiteral("poor");
        client->appData.qualityLevel = poor ? 2 : medium ? 5 : 8;
        client->appData.compressLevel = poor ? 9 : medium ? 6 : 3;
    }
    if (options.encryption == QStringLiteral("require")) {
        // Only the TLS-based security types; plain VNC is refused.
        static const uint32_t secure[] = {rfbVeNCrypt, rfbTLS};
        SetClientAuthSchemes(client, secure, 2);
    }

    const QByteArray host = settings.host.toUtf8();
    QByteArray repeaterHost;
    QByteArray repeaterTarget;
    if (options.listen == 1) {
        client->listenPort = options.listenPort > 0 ? options.listenPort : 5500;
        QMetaObject::invokeMethod(this, [this, port = client->listenPort] {
            emit statusMessage(QStringLiteral("Waiting for the remote computer to connect on "
                                              "port %1…").arg(port));
        }, Qt::QueuedConnection);
        int result = 0;
        while (!m_stopping && result == 0)
            result = listenForIncomingConnectionsNoFork(client, 200000);
        if (result <= 0) {
            const QString error = lastError;
            rfbClientCleanup(client);
            finish(m_stopping ? QStringLiteral("Disconnected.")
                              : QStringLiteral("Could not listen for connections: %1").arg(error),
                   !m_stopping);
            return;
        }
    } else if (!options.repeater.isEmpty()) {
        // UltraVNC repeater: connect to it and name the server, by ID or by
        // its host and port.
        QString repeater = options.repeater;
        int repeaterPort = 5901;
        if (repeater.count(QLatin1Char(':')) == 1) {
            repeaterPort = repeater.section(QLatin1Char(':'), 1).toInt();
            repeater = repeater.section(QLatin1Char(':'), 0, 0);
        }
        repeaterHost = repeater.toUtf8();
        repeaterTarget = options.repeaterId.isEmpty() ? host
                                                      : ("ID:" + options.repeaterId.toUtf8());
        client->serverHost = strdup(repeaterHost.constData());
        client->serverPort = repeaterPort;
        client->destHost = strdup(repeaterTarget.constData());
        client->destPort = options.repeaterId.isEmpty() ? settings.port : 0;
    } else {
        client->serverHost = strdup(host.constData());
        client->serverPort = settings.port;
    }
    if (options.listen != 1) {
        const QString target = settings.label.isEmpty()
                                   ? QStringLiteral("%1:%2").arg(settings.host).arg(settings.port)
                                   : settings.label;
        QMetaObject::invokeMethod(this, [this, target] {
            emit statusMessage(QStringLiteral("Connecting to %1…").arg(target));
        }, Qt::QueuedConnection);
    }

    // rfbInitClient frees the client when it fails.
    if (!rfbInitClient(client, nullptr, nullptr)) {
        finish(m_stopping ? QStringLiteral("Disconnected.")
               : lastError.isEmpty()
                   ? QStringLiteral("Could not connect to %1.")
                         .arg(settings.label.isEmpty() ? settings.host : settings.label)
                   : QStringLiteral("Could not connect: %1").arg(lastError),
               !m_stopping);
        return;
    }
    {
        const int width = client->width;
        const int height = client->height;
        const QString name = QString::fromUtf8(client->desktopName ? client->desktopName : "");
        QMetaObject::invokeMethod(this, [this, width, height, name] {
            emit connected(width, height, name);
        }, Qt::QueuedConnection);
    }

    QString reason;
    bool error = false;
    bool resizeKnown = false;
    while (!m_stopping) {
        flushInput(client);
        const int ready = WaitForMessage(client, 5000);
        if (ready < 0 || (ready > 0 && !HandleRFBServerMessage(client))) {
            reason = lastError.isEmpty() ? QStringLiteral("The connection was closed.")
                                         : QStringLiteral("Connection lost: %1").arg(lastError);
            error = true;
            break;
        }
        // The server reported its screen layout, so it takes size requests.
        if (!resizeKnown && (client->screen.width != 0 || client->screen.height != 0)) {
            resizeKnown = true;
            QMetaObject::invokeMethod(this, [this] { emit resizeSupported(); }, Qt::QueuedConnection);
        }
    }
    if (m_stopping)
        reason = QStringLiteral("Disconnected.");
    rfbClientCleanup(client);
    m_frameBuffer.clear();
    finish(reason, error && !m_stopping);
}

void VncSession::queueInput(InputEvent event)
{
    QMutexLocker locker(&m_inputMutex);
    m_inputQueue.append(std::move(event));
}

void VncSession::flushInput(void *clientPointer)
{
    auto *client = static_cast<rfbClient *>(clientPointer);
    QList<InputEvent> events;
    {
        QMutexLocker locker(&m_inputMutex);
        events.swap(m_inputQueue);
    }
    for (const auto &event : std::as_const(events)) {
        switch (event.type) {
        case InputEvent::Key:
            SendKeyEvent(client, event.code, event.down ? TRUE : FALSE);
            break;
        case InputEvent::Pointer:
            SendPointerEvent(client, event.x, event.y, event.buttons);
            break;
        case InputEvent::Clipboard: {
            // UTF-8 when the server supports the extended clipboard, Latin-1
            // (the original protocol) otherwise.
            QByteArray utf8 = event.text.toUtf8();
            if (client->extendedClipboardServerCapabilities != 0
                && SendClientCutTextUTF8(client, utf8.data(), int(utf8.size())))
                break;
            QByteArray latin1 = event.text.toLatin1();
            SendClientCutText(client, latin1.data(), int(latin1.size()));
            break;
        }
        case InputEvent::Resize: {
            // SetDesktopSize with the screen the server reported, resized.
            // (libvncclient's SendExtDesktopSize leaves the screen's id and
            // position uninitialised, which servers reject.)
            const auto width = uint16_t(qBound(64, event.x, 8192));
            const auto height = uint16_t(qBound(64, event.y, 8192));
            rfbSetDesktopSizeMsg message{};
            message.type = rfbSetDesktopSize;
            message.width = rfbClientSwap16IfLE(width);
            message.height = rfbClientSwap16IfLE(height);
            message.numberOfScreens = 1;
            rfbExtDesktopScreen screen = client->screen; // network byte order
            screen.x = 0;
            screen.y = 0;
            screen.width = rfbClientSwap16IfLE(width);
            screen.height = rfbClientSwap16IfLE(height);
            if (WriteToRFBServer(client, reinterpret_cast<const char *>(&message), sz_rfbSetDesktopSizeMsg))
                WriteToRFBServer(client, reinterpret_cast<const char *>(&screen), sz_rfbExtDesktopScreen);
            break;
        }
        }
    }
}

void VncSession::sendKey(quint32 keysym, bool down)
{
    queueInput({InputEvent::Key, keysym, down});
}

void VncSession::sendPointer(int x, int y, int buttons)
{
    queueInput({InputEvent::Pointer, 0, false, x, y, buttons});
}

void VncSession::setLocalClipboard(const QString &text)
{
    if (!text.isEmpty())
        queueInput({InputEvent::Clipboard, 0, false, 0, 0, 0, text});
}

void VncSession::requestDesktopSize(int width, int height)
{
    queueInput({InputEvent::Resize, 0, false, width, height});
}

void VncSession::allocateFrame(int width, int height, int depth)
{
    QMutexLocker locker(&m_frameMutex);
    const auto format = depth == 16 ? QImage::Format_RGB16
                        : depth == 8 ? QImage::Format_Indexed8
                                     : QImage::Format_RGB32;
    m_frame = QImage(width, height, format);
    if (depth == 8) {
        // BGR233: blue in the top two bits, green in the middle three.
        QList<QRgb> table(256);
        for (int value = 0; value < 256; ++value) {
            const unsigned char pixel = static_cast<unsigned char>(value);
            table[value] = pixelColor(&pixel, 8);
        }
        m_frame.setColorTable(table);
    }
    m_frame.fill(0);
}

void VncSession::copyFrame(const unsigned char *buffer, int stride, const QRect &area)
{
    {
        QMutexLocker locker(&m_frameMutex);
        const QRect clipped = area & m_frame.rect();
        if (clipped.isEmpty() || !buffer)
            return;
        const int bytes = m_frame.depth() / 8;
        for (int row = clipped.top(); row <= clipped.bottom(); ++row) {
            std::memcpy(m_frame.scanLine(row) + clipped.left() * bytes,
                        buffer + qsizetype(row) * stride + clipped.left() * bytes,
                        size_t(clipped.width()) * size_t(bytes));
        }
        m_pendingUpdate |= clipped;
        if (m_updatePosted)
            return;
        m_updatePosted = true;
    }
    // One queued repaint per GUI event-loop pass.
    QMetaObject::invokeMethod(this, [this] {
        QRect update;
        {
            QMutexLocker locker(&m_frameMutex);
            update = std::exchange(m_pendingUpdate, QRect());
            m_updatePosted = false;
        }
        emit frameUpdated(update);
    }, Qt::QueuedConnection);
}

// Runs the prompt on the GUI thread and waits, but gives up once the session
// is disconnecting so closing the tab never waits for a dialog.
bool VncSession::askCredentials(QString *username, QString *password, bool needUsername)
{
    if (m_stopping)
        return false;
    struct Prompt
    {
        std::mutex mutex;
        std::condition_variable answered;
        bool done = false;
        QString username, password;
        bool accepted = false;
    };
    auto prompt = std::make_shared<Prompt>();
    prompt->username = *username;
    QMetaObject::invokeMethod(this, [this, prompt, needUsername] {
        QString user = prompt->username;
        QString secret;
        bool accepted = false;
        emit credentialsRequired(&user, &secret, needUsername, &accepted);
        std::lock_guard<std::mutex> lock(prompt->mutex);
        prompt->username = user;
        prompt->password = secret;
        prompt->accepted = accepted;
        prompt->done = true;
        prompt->answered.notify_all();
    }, Qt::QueuedConnection);
    std::unique_lock<std::mutex> lock(prompt->mutex);
    while (!prompt->done) {
        if (m_stopping)
            return false;
        prompt->answered.wait_for(lock, std::chrono::milliseconds(100));
    }
    if (!prompt->accepted)
        return false;
    *username = prompt->username;
    *password = prompt->password;
    prompt->password.fill(QChar(0));
    return true;
}
