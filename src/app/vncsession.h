// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "vncoptions.h"

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>

#include <atomic>
#include <thread>

struct VncConnectionSettings
{
    QString host;
    quint16 port = 5900;
    // For VeNCrypt Plain and UltraVNC MS-Logon; most servers ask only for a
    // password.
    QString username;
    // From the credential vault; empty asks when the server needs one.
    QString password;
    // Fully resolved (defaults plus the session's overrides).
    VncOptions options = VncOptions::builtIn();
    // How the server is named in messages (host:port when empty), for
    // example when host is a local tunnel port.
    QString label;
};

// GUI-thread handle for one VNC connection. libvncclient runs on a worker
// thread; the remote screen is copied into frame(), and input is queued to
// the worker.
class VncSession final : public QObject
{
    Q_OBJECT

public:
    explicit VncSession(QObject *parent = nullptr);
    ~VncSession() override;

    void connectToHost(const VncConnectionSettings &settings);
    void disconnectFromHost();
    // Disconnects and deletes this session once its worker has ended,
    // without blocking. Use instead of deleting a connected session.
    void retire();

    // The remote screen (RGB32, RGB16, or Indexed8 by colour depth). Lock
    // frameMutex() while reading it.
    const QImage &frame() const { return m_frame; }
    QMutex &frameMutex() { return m_frameMutex; }

    // X11 keysyms.
    void sendKey(quint32 keysym, bool down);
    // Button mask: 1 left, 2 middle, 4 right, 8/16 wheel up/down, 32/64
    // wheel left/right, 128/256 back/forward.
    void sendPointer(int x, int y, int buttons);
    void setLocalClipboard(const QString &text);
    // Asks the server for a new desktop size (when resizeSupported()).
    void requestDesktopSize(int width, int height);
    // Asks the server for the whole screen again (a non-incremental update),
    // for servers that miss some changes and leave stale areas.
    void requestFullUpdate();

    // Called from the worker thread; waits for the user's answer.
    bool askCredentials(QString *username, QString *password, bool needUsername);

signals:
    void statusMessage(const QString &message);
    void connected(int width, int height, const QString &desktopName);
    void frameUpdated(const QRect &area);
    void desktopResized(int width, int height);
    // The server accepts desktop size requests.
    void resizeSupported();
    void pointerChanged(const QImage &shape, const QPoint &hotSpot);
    void remoteClipboardText(const QString &text);
    // error: lost or refused, as opposed to closed normally by the user.
    void disconnected(const QString &reason, bool error);
    // Answered by setting the values before returning.
    void credentialsRequired(QString *username, QString *password, bool needUsername,
                             bool *accepted);

private:
    friend struct VncCallbacks;

    struct InputEvent
    {
        enum Type { Key, Pointer, Clipboard, Resize, Refresh } type;
        quint32 code = 0;
        bool down = false;
        int x = 0;
        int y = 0;
        int buttons = 0;
        QString text;
    };

    void run(VncConnectionSettings settings);
    void queueInput(InputEvent event);
    void flushInput(void *client);
    // False when the memory could not be had.
    bool allocateFrame(int width, int height, int depth);
    void copyFrame(const unsigned char *buffer, int stride, const QRect &area);

    std::thread m_thread;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_finished{true};
    QMutex m_inputMutex;
    QList<InputEvent> m_inputQueue;

    QMutex m_frameMutex;
    QImage m_frame;
    QRect m_pendingUpdate;
    bool m_updatePosted = false;

    // Worker thread only.
    QString m_password;
    QString m_username;
    QByteArray m_frameBuffer; // libvncclient draws here
    int m_depth = 32;
};
