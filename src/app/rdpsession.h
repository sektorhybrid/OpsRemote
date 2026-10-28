// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QRect>
#include <QString>

#include "rdpoptions.h"

#include <atomic>
#include <memory>
#include <thread>

struct RdpConnectionSettings
{
    QString host;
    quint16 port = 3389;
    QString username;
    QString domain;
    // From the credential vault; empty asks when the server needs it.
    QString password;
    int width = 1280;
    int height = 800;
    // The local device pixel ratio; above 1, width and height are physical
    // pixels and the server is asked to scale its display to match.
    qreal scale = 1.0;
    // Where FreeRDP keeps the certificates the user trusted.
    QString configPath;
    // Fully resolved (defaults plus the session's overrides).
    RdpOptions options = RdpOptions::builtIn();
};

enum class RdpCertificateDecision { Reject, AcceptOnce, AcceptAndSave };

// GUI-thread handle for one RDP connection. FreeRDP runs its blocking event
// loop on a worker thread; the remote desktop is drawn by FreeRDP's software
// GDI into frame(), and input is queued to the worker.
class RdpClipboard;

class RdpSession final : public QObject
{
    Q_OBJECT

public:
    explicit RdpSession(QObject *parent = nullptr);
    // Disconnects and waits for the worker thread.
    ~RdpSession() override;

    void connectToHost(const RdpConnectionSettings &settings);
    void disconnectFromHost();
    // Disconnects and deletes this session once its worker thread has ended,
    // without blocking. Use instead of deleting a connected session.
    void retire();

    // The remote screen. Lock frameMutex() while reading it.
    const QImage &frame() const { return m_frame; }
    QMutex &frameMutex() { return m_frameMutex; }

    // Input; scancodes are RDP scancodes (MAKE_RDP_SCANCODE).
    void sendKey(quint32 rdpScancode, bool down, bool repeat);
    void sendUnicode(quint16 character, bool down);
    void sendPause();
    void sendMouse(quint16 flags, int x, int y);
    void sendExtendedMouse(quint16 flags, int x, int y);
    // Synchronizes Caps/Num/Scroll Lock (KBD_SYNC_* flags), e.g. on focus in.
    void sendSynchronize(quint32 toggleStates);
    // Asks the server to change the desktop size (Display Control channel).
    // Ignored if the server does not support it; the page then scales.
    void requestDesktopSize(int width, int height, qreal scale = 1.0);
    // Several monitors (full screen on all monitors), in remote pixels; the
    // first is the primary one and must be at 0,0. Servers that allow fewer
    // monitors get the first one only.
    void requestMonitorLayout(const QList<QRect> &monitors, qreal scale = 1.0);
    // How many monitors the server accepts (1 until Display Control is ready).
    int maxMonitors() const;

    // Clipboard (when the session's options share it): what this computer's
    // clipboard holds, offered to the remote computer; and saving the files
    // last copied there (remoteFilesAvailable) into a local folder.
    void setLocalClipboard(const QString &text, const QStringList &files);
    void saveRemoteFiles(const QString &folder);
    void cancelRemoteFileSave();

    // Called from the worker thread; block it until the user answered.
    RdpCertificateDecision askCertificate(const QString &host, const QString &subject,
                                          const QString &issuer, const QString &fingerprint,
                                          bool changed);
    bool askCredentials(QString *username, QString *password, QString *domain, bool gateway);

signals:
    void statusMessage(const QString &message);
    void connected(int width, int height);
    void frameUpdated(const QRect &area);
    void desktopResized(int width, int height);
    // The server accepts desktop size changes (requestDesktopSize()).
    void resizeSupported();
    // The remote mouse pointer: a shape with its hot spot, hidden, or the
    // system default arrow. Coordinates are in remote pixels.
    void pointerChanged(const QImage &shape, const QPoint &hotSpot);
    void pointerHidden();
    void pointerDefault();

    void remoteClipboardText(const QString &text);
    // Top-level names of the files copied on the remote computer.
    void remoteFilesAvailable(const QStringList &names, quint64 totalBytes);
    void remoteFilesProgress(quint64 doneBytes, quint64 totalBytes, const QString &current);
    // The remote computer reading files pasted from this computer.
    void localFilesProgress(quint64 sentBytes, quint64 totalBytes, const QString &current);
    // The saved top-level paths; error is empty on success.
    void remoteFilesSaved(const QStringList &paths, const QString &error);
    void disconnected(const QString &reason, bool error);

    // Answered by setting *decision / the credentials before returning.
    void certificateVerificationRequired(const QString &host, const QString &subject,
                                         const QString &issuer, const QString &fingerprint,
                                         bool changed, RdpCertificateDecision *decision);
    // gateway: the credentials are for the RD Gateway, not the computer.
    void credentialsRequired(QString *username, QString *password, QString *domain,
                             bool gateway, bool *accepted);
    // How the desktop is transferred, for the status bar.
    void graphicsMode(const QString &description);

private:
    friend struct RdpCallbacks;

    struct InputEvent
    {
        enum Type { Key, Unicode, Pause, Mouse, ExtendedMouse, Synchronize, Resize } type;
        quint32 code = 0;
        bool down = false;
        bool repeat = false;
        quint16 flags = 0;
        int x = 0;
        int y = 0;
    };

    void run(RdpConnectionSettings settings);
    template <typename Ask>
    bool askOnGuiThread(Ask ask);
    void queueInput(const InputEvent &event);
    void flushInput(void *input);
    // Copies a dirty rectangle of FreeRDP's frame buffer into m_frame.
    void copyFrame(const unsigned char *buffer, int stride, int width, int height,
                   const QRect &area);
    void resizeFrame(int width, int height);
    void postFrameUpdate(const QRect &area);

    std::thread m_thread;
    std::atomic<void *> m_context{nullptr};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_finished{true}; // the worker thread has returned
    void *m_inputEvent = nullptr; // a WinPR event HANDLE
    // Display Control channel; worker thread only.
    void *m_display = nullptr;
    std::atomic<bool> m_displayReady{false};
    bool m_askCredentials = true; // worker thread only
    std::atomic<quint32> m_maxMonitors{1};
    QMutex m_inputMutex;
    QList<InputEvent> m_inputQueue;
    // The latest requested layout (guarded by m_inputMutex).
    QList<QRect> m_monitorLayout;
    qreal m_layoutScale = 1.0;

    std::unique_ptr<RdpClipboard> m_clipboard;

    QMutex m_frameMutex;
    QImage m_frame;
    QRect m_pendingUpdate;
    bool m_updatePosted = false;
};
