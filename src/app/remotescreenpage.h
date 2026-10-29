// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QIcon>
#include <QImage>
#include <QPointer>
#include <QWidget>

class QKeyEvent;
class QLabel;
class QTabWidget;
class QTimer;

// What remote screen tabs (RDP, VNC) have in common: full screen in a window
// of their own (Ctrl+Alt+Enter), key combinations the local system would
// otherwise catch (Send Keys), a screenshot, and closing the tab after the
// session ended.
class RemoteScreenPage : public QWidget
{
    Q_OBJECT

public:
    // Key combinations for the Send Keys menu.
    enum class KeyCombination { CtrlAltDelete, CtrlAltBackspace, CtrlEscape, AltTab, PrintScreen,
                                Windows };
    static QList<QPair<KeyCombination, QString>> keyCombinations();

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
    // Ctrl+Alt+Enter: toggles full screen. True if the event was used.
    bool handleFullScreenKey(QKeyEvent *event);
    // Mouse movement in the page: the exit bar appears at the top edge.
    void trackFullScreenPointer(const QPointF &position);

    // OPSREMOTE_TEST_MONITORS=N (testing): N monitors side by side.
    static int testMonitorCount();

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
    QTimer *m_fullScreenBarTimer = nullptr;
};
