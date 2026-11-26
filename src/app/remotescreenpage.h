// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QPointer>
#include <QWidget>

class QKeyEvent;
class QLabel;
class QPushButton;
class QMutex;
class QPainter;
class QTabWidget;
class QTimer;
class SessionEndedPanel;

// What remote screen tabs (RDP, VNC) have in common: full screen in a window
// of their own (Ctrl+Alt+Enter by default), key combinations the local system would
// otherwise catch (Send Keys), a screenshot, closing the tab after the
// session ended, and drawing the remote picture: scaled to fit, with its
// pointer, the overlay while connecting and the bar once it ended.
class RemoteScreenPage : public QWidget
{
    Q_OBJECT

public:
    // Key combinations for the Send Keys menu.
    enum class KeyCombination { CtrlAltDelete, CtrlAltBackspace, CtrlEscape, AltTab, PrintScreen,
                                Windows };
    static QList<QPair<KeyCombination, QString>> keyCombinations();

    // The full-screen key ("remote_full_screen" in Keyboard shortcuts), for
    // every remote desktop tab; set by the main window.
    static void setFullScreenKey(const QKeySequence &key);

    explicit RemoteScreenPage(QWidget *parent = nullptr);
    ~RemoteScreenPage() override;

    virtual bool isConnected() const = 0;
    virtual QString host() const = 0;
    // The remote screen as last received (empty before connecting).
    virtual QImage screenshot() const = 0;
    void sendKeyCombination(KeyCombination combination);
    // Asks the server for the whole screen again; false where not offered.
    virtual bool canRefresh() const { return false; }
    virtual void refreshScreen() {}

    // Full screen, on this monitor or across all of them (see
    // canSpanMonitors()); Ctrl+Alt+Enter toggles it from inside the tab.
    bool isFullScreenDesktop() const { return m_fullScreen; }
    void setFullScreen(bool on);

signals:
    void statusMessage(const QString &message);
    // A password typed at a log-on prompt worked: this tab's own (savedSession
    // empty) or that of the jump host from that saved session.
    void typedPasswordWorked(const QString &savedSession, const QString &password, bool replaces);
    void fullScreenChanged(bool on);
    // The user asked to close this tab (after the session ended).
    void closeRequested();

protected:
    // The keys of a combination, pressed and released in order.
    enum class Key { Control, Alt, Delete, Backspace, Escape, Tab, Print, Windows };
    virtual void sendKey(Key key, bool down) = 0;
    // Whether full screen may span every monitor for this session.
    virtual bool canSpanMonitors() const { return false; }
    // After entering or leaving full screen (resize the desktop, release keys).
    virtual void fullScreenToggled() {}

    bool spansAllMonitors() const { return m_allMonitors; }
    // Leaves full screen if needed and asks for the tab to be closed.
    void requestClose();
    // The full-screen key (Ctrl+Alt+Enter by default): toggles full screen.
    // True if the event was used.
    bool handleFullScreenKey(QKeyEvent *event);
    // Mouse movement in the page: the exit bar appears at the top edge.
    void trackFullScreenPointer(const QPointF &position);

    // OPSREMOTE_TEST_MONITORS=N (testing): N monitors side by side.
    static int testMonitorCount();

    // The session's picture; read frame() only with frameMutex() locked.
    virtual QMutex &frameMutex() const = 0;
    virtual const QImage &frame() const = 0;
    // The mouse pointer for the page's state (showPointerShape() for the
    // server's shape).
    virtual void updateCursor() = 0;

    // While connected every key goes to the remote computer (no shortcuts);
    // a new size or screen scale places the overlays, restarts the desktop
    // resize (m_resizeTimer) and rescales the pointer.
    bool event(QEvent *event) override;
    // The dimmed overlay (m_overlay, m_overlayText; name: its object name)
    // and the bar shown once the session ended (m_endedPanel), hidden.
    void buildOverlay(const QString &name);
    // Where the picture goes: scaled down to fit, never up, and centred; one
    // remote pixel per physical pixel on high-DPI screens when the session
    // asked for that (m_connectScale).
    QRect targetRect() const;
    // A remote area that changed, repainted where it shows.
    void repaintRemoteArea(const QRect &area);
    // The picture, on black.
    void paintFrame(QPainter &painter, const QRect &dirty);
    // The server's pointer shape, scaled like the desktop when it is shown
    // smaller.
    void showPointerShape();

    QWidget *m_overlay = nullptr;
    QLabel *m_overlayText = nullptr;
    SessionEndedPanel *m_endedPanel = nullptr;
    bool m_connected = false;
    // The server can follow the tab's size; m_resizeTimer waits until the
    // tab stops changing size before asking (the page creates it).
    bool m_resizeSupported = false;
    QTimer *m_resizeTimer = nullptr;
    QImage m_pointerShape;
    QPoint m_pointerHotSpot;
    // The scale the desktop was last sized for.
    qreal m_connectScale = 1.0;

private:
    QTabWidget *tabWidget() const;
    void showFullScreenBar();

    bool m_fullScreen = false;
    bool m_allMonitors = false;
    QPointer<QTabWidget> m_tabs;
    QWidget *m_placeholder = nullptr;
    QWidget *m_fullScreenWindow = nullptr; // owns this page while in full screen
    QString m_tabTitle;
    QIcon m_tabIcon;
    QString m_tabToolTip;
    QWidget *m_fullScreenBar = nullptr;
    QLabel *m_fullScreenText = nullptr;
    QPushButton *m_fullScreenLeave = nullptr;
    QTimer *m_fullScreenBarTimer = nullptr;
};
