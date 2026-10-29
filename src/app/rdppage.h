// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "rdpsession.h"
#include "remotescreenpage.h"

#include <QPointer>
#include <QSet>

class QLabel;
class QProgressBar;
class QPushButton;
class JumpChain;
class SessionEndedPanel;
class QTimer;

// A tab showing one remote desktop. The desktop is scaled to fit the tab,
// keeping its aspect ratio; keyboard and mouse input go to the server while
// the tab has focus (application shortcuts included, like mstsc).
class RdpPage final : public RemoteScreenPage
{
    Q_OBJECT

public:
    RdpPage(const RdpConnectionSettings &settings, QWidget *parent = nullptr);
    ~RdpPage() override;

    bool isConnected() const override { return m_connected; }
    QString host() const override { return m_settings.host; }
    QImage screenshot() const override;

protected:
    void sendKey(Key key, bool down) override;
    // All monitors: when the options ask for it and the server takes several.
    bool canSpanMonitors() const override;
    void fullScreenToggled() override;

    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void stopJumpChain();
    void start();
    // ended: the session is over; offers Reconnect and Close Tab.
    void showOverlay(const QString &text, bool ended, bool error = false);
    QRect targetRect() const;
    bool toRemote(const QPointF &position, int *x, int *y) const;
    void sendKeyEvent(QKeyEvent *event, bool down);
    void sendButton(Qt::MouseButton button, bool down, const QPointF &position);
    void releaseAllKeys();
    void requestTabSize();
    // Remote pixels per widget unit: the device pixel ratio with high DPI.
    qreal scale() const;
    QList<QRect> monitorLayout() const;
    void updateCursor();
    // Clipboard sharing (see RdpOptions::clipboardText/clipboardFiles).
    void offerLocalClipboard();
    // The bar at the top of the tab for clipboard file transfers.
    enum class BarAction { None, Save, Cancel, OpenFolder };
    void showBar(const QString &text, BarAction action, bool closable, int hideAfterMs = 0);
    void hideBar();

    RdpConnectionSettings m_settings;
    RdpSession *m_session = nullptr;
    QWidget *m_overlay = nullptr;
    QLabel *m_overlayText = nullptr;
    SessionEndedPanel *m_endedPanel = nullptr;
    bool m_connected = false;
    QPointer<JumpChain> m_jumpChain;
    bool m_started = false;
    bool m_resizeSupported = false;
    // Waits until the tab stops changing size before resizing the desktop.
    QTimer *m_resizeTimer = nullptr;
    QTimer *m_resizeRetryTimer = nullptr;
    QSize m_requestedSize;
    int m_resizeRetries = 0;
    QSet<quint32> m_pressedKeys;
    int m_wheelRemainder = 0;
    // The remote pointer shape, scaled with the desktop when shown smaller.
    QImage m_pointerShape;
    QPoint m_pointerHotSpot;
    int m_ignoreClipboardChanges = 0;
    QWidget *m_bar = nullptr;
    QLabel *m_barText = nullptr;
    QProgressBar *m_barProgress = nullptr;
    QPushButton *m_barAction = nullptr;
    QPushButton *m_barClose = nullptr;
    QTimer *m_barTimer = nullptr;
    BarAction m_barActionKind = BarAction::None;
    QString m_savedFolder;
    bool m_pointerHidden = false;
    // The scale the desktop was last sized for (see scale()).
    qreal m_connectScale = 1.0;
    QString m_graphicsMode;
};
