// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "remotescreenpage.h"
#include "sshsession.h"
#include "vncsession.h"

#include <QHash>
#include <optional>

class QLabel;
class QTimer;
class SessionEndedPanel;

struct VncPageSettings
{
    VncConnectionSettings connection;
    // Through an SSH server: its connection, with the VNC host and port as
    // the forwarding target.
    std::optional<SshConnectionSettings> tunnel;
    // The vault can store a password typed at the log-on prompt.
    bool canSavePassword = false;
};

// A tab showing one VNC desktop, drawn like a remote desktop tab: scaled to
// fit (or following the tab size when the server allows it), keyboard and
// mouse going to the server while the tab has focus.
class VncPage final : public RemoteScreenPage
{
    Q_OBJECT

public:
    VncPage(const VncPageSettings &settings, QWidget *parent = nullptr);
    ~VncPage() override;

    bool isConnected() const override { return m_connected; }
    QString host() const override { return m_settings.connection.host; }
    QImage screenshot() const override;

signals:
    // The user typed a password at the prompt and chose to save it.
    void passwordSaveRequested(const QString &password);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool focusNextPrevChild(bool) override { return false; }

    void sendKey(Key key, bool down) override;
    bool canSpanMonitors() const override;
    void fullScreenToggled() override;

private:
    void start();
    void startVnc(const QString &host, quint16 port);
    void stopTunnel();
    void showOverlay(const QString &text, bool ended, bool error = false);
    QRect targetRect() const;
    bool toRemote(const QPointF &position, int *x, int *y) const;
    void sendPointer(const QPointF &position);
    void releaseAllKeys();
    void requestTabSize();
    qreal scale() const;
    void updateCursor();
    void offerLocalClipboard();
    bool askCredentials(QString *username, QString *password, bool needUsername);

    VncPageSettings m_settings;
    VncSession *m_session = nullptr;
    SshSession *m_tunnel = nullptr;
    QWidget *m_overlay = nullptr;
    QLabel *m_overlayText = nullptr;
    SessionEndedPanel *m_endedPanel = nullptr;
    bool m_connected = false;
    bool m_started = false;
    bool m_wasConnected = false;
    bool m_resizeSupported = false;
    int m_reconnectAttempts = 0;
    QTimer *m_resizeTimer = nullptr;
    // Pressed keys (by native scan code or Qt key) and the keysym sent, so
    // the release sends the same one.
    QHash<quint32, quint32> m_pressedKeys;
    int m_buttons = 0;
    int m_wheelRemainder = 0;
    QImage m_pointerShape;
    QPoint m_pointerHotSpot;
    int m_ignoreClipboardChanges = 0;
    qreal m_connectScale = 1.0;
};
