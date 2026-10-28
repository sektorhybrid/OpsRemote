// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "remotescreenpage.h"

#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

QList<QPair<RemoteScreenPage::KeyCombination, QString>> RemoteScreenPage::keyCombinations()
{
    return {{KeyCombination::CtrlAltDelete, QStringLiteral("Ctrl+Alt+Del")},
            {KeyCombination::CtrlAltBackspace, QStringLiteral("Ctrl+Alt+Backspace")},
            {KeyCombination::CtrlEscape, QStringLiteral("Ctrl+Esc")},
            {KeyCombination::AltTab, QStringLiteral("Alt+Tab")},
            {KeyCombination::Windows, QStringLiteral("Windows key")},
            {KeyCombination::PrintScreen, QStringLiteral("Print Screen")}};
}

RemoteScreenPage::RemoteScreenPage(QWidget *parent)
    : QWidget(parent)
{
    // In full screen, a small bar at the top edge leads back to the tab.
    m_fullScreenBar = new QWidget(this);
    m_fullScreenBar->setAttribute(Qt::WA_StyledBackground);
    m_fullScreenBar->setObjectName(QStringLiteral("remoteFullScreenBar"));
    m_fullScreenBar->setStyleSheet(QStringLiteral(
        "#remoteFullScreenBar { background: palette(window); border: 1px solid palette(mid);"
        " border-top: none; border-bottom-left-radius: 6px; border-bottom-right-radius: 6px; }"));
    auto *layout = new QHBoxLayout(m_fullScreenBar);
    layout->setContentsMargins(12, 2, 4, 4);
    m_fullScreenText = new QLabel(m_fullScreenBar);
    m_fullScreenText->setTextFormat(Qt::PlainText);
    auto *leave = new QPushButton(QStringLiteral("Exit Full Screen (Ctrl+Alt+Enter)"),
                                  m_fullScreenBar);
    leave->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_fullScreenText);
    layout->addWidget(leave);
    m_fullScreenBar->hide();
    connect(leave, &QPushButton::clicked, this, [this] { setFullScreen(false); });
    m_fullScreenBarTimer = new QTimer(this);
    m_fullScreenBarTimer->setSingleShot(true);
    m_fullScreenBarTimer->setInterval(2500);
    connect(m_fullScreenBarTimer, &QTimer::timeout, this, [this] {
        // Stay while the mouse is on the bar.
        if (m_fullScreenBar->geometry().contains(mapFromGlobal(QCursor::pos())))
            m_fullScreenBarTimer->start();
        else
            m_fullScreenBar->hide();
    });
}

RemoteScreenPage::~RemoteScreenPage()
{
    if (m_placeholder) {
        // Deleted while in full screen: the tab goes too.
        disconnect(m_placeholder, nullptr, this, nullptr);
        m_placeholder->deleteLater();
    }
}

int RemoteScreenPage::testMonitorCount()
{
    return qBound(0, qEnvironmentVariableIntValue("OPSREMOTE_TEST_MONITORS"), 4);
}

void RemoteScreenPage::sendKeyCombination(KeyCombination combination)
{
    if (!isConnected())
        return;
    QList<Key> keys;
    switch (combination) {
    case KeyCombination::CtrlAltDelete:
        keys = {Key::Control, Key::Alt, Key::Delete};
        break;
    case KeyCombination::CtrlAltBackspace:
        keys = {Key::Control, Key::Alt, Key::Backspace};
        break;
    case KeyCombination::CtrlEscape:
        keys = {Key::Control, Key::Escape};
        break;
    case KeyCombination::AltTab:
        keys = {Key::Alt, Key::Tab};
        break;
    case KeyCombination::PrintScreen:
        keys = {Key::Print};
        break;
    case KeyCombination::Windows:
        keys = {Key::Windows};
        break;
    }
    for (const Key key : std::as_const(keys))
        sendKey(key, true);
    for (auto key = keys.crbegin(); key != keys.crend(); ++key)
        sendKey(*key, false);
    setFocus();
}

bool RemoteScreenPage::handleFullScreenKey(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
               == (Qt::ControlModifier | Qt::AltModifier)
        && !event->isAutoRepeat()) {
        setFullScreen(!m_fullScreen);
        event->accept();
        return true;
    }
    return false;
}

void RemoteScreenPage::trackFullScreenPointer(const QPointF &position)
{
    if (m_fullScreen && position.y() < 2 && !m_fullScreenBar->isVisible())
        showFullScreenBar();
}

// Full screen moves this page out of its tab into a window of its own; a
// placeholder keeps the tab's place, and closing it closes the session.
void RemoteScreenPage::setFullScreen(bool on)
{
    if (on == m_fullScreen)
        return;
    if (on) {
        auto *tabs = tabWidget();
        if (!tabs)
            return;
        const int index = tabs->indexOf(this);
        m_tabs = tabs;
        m_tabTitle = tabs->tabText(index);
        m_tabIcon = tabs->tabIcon(index);
        m_tabToolTip = tabs->tabToolTip(index);
        auto *screen = window()->screen();

        m_placeholder = new QWidget;
        // The tab's bookkeeping (such as its session id) stays with the tab.
        for (const auto &name : dynamicPropertyNames())
            m_placeholder->setProperty(name.constData(), property(name.constData()));
        auto *layout = new QVBoxLayout(m_placeholder);
        layout->addStretch();
        auto *label = new QLabel(QStringLiteral("%1 is in full screen.").arg(host()), m_placeholder);
        label->setAlignment(Qt::AlignCenter);
        auto *back = new QPushButton(QStringLiteral("Return to Tab"), m_placeholder);
        layout->addWidget(label);
        layout->addWidget(back, 0, Qt::AlignHCenter);
        layout->addStretch();
        connect(back, &QPushButton::clicked, this, [this] { setFullScreen(false); });
        // Closing the placeholder tab ends the session too.
        connect(m_placeholder, &QObject::destroyed, this, [this] {
            m_placeholder = nullptr;
            // The full-screen window owns this page now and takes it along.
            if (m_fullScreen && m_fullScreenWindow) {
                m_fullScreenWindow->hide();
                m_fullScreenWindow->deleteLater();
            }
        });

        const QSignalBlocker blocker(tabs);
        tabs->removeTab(index);
        tabs->insertTab(index, m_placeholder, m_tabIcon, m_tabTitle);
        tabs->setTabToolTip(index, m_tabToolTip);
        tabs->setCurrentIndex(index);

        m_fullScreen = true;
        // Every monitor: one frameless window across all of them, when the
        // session allows it and the platform lets windows be placed (not on
        // Wayland).
        const auto screens = QGuiApplication::screens();
        m_allMonitors = (screens.size() > 1 || testMonitorCount() > 1) && canSpanMonitors()
                        && !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
        qInfo("Full screen: %lld local monitors, all monitors %s", qlonglong(screens.size()),
              m_allMonitors ? "on" : "off");
        // The page moves into a window of its own as an ordinary child; it
        // never becomes a window itself, which Qt does not undo cleanly.
        m_fullScreenWindow = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint
                                                      | (m_allMonitors ? Qt::WindowStaysOnTopHint
                                                                       : Qt::WindowFlags()));
        m_fullScreenWindow->setWindowTitle(m_tabTitle);
        m_fullScreenWindow->setWindowIcon(m_tabIcon);
        auto *windowLayout = new QVBoxLayout(m_fullScreenWindow);
        windowLayout->setContentsMargins(0, 0, 0, 0);
        windowLayout->addWidget(this);
        if (m_allMonitors) {
            QRect area;
            for (auto *each : screens)
                area |= each->geometry();
            m_fullScreenWindow->setGeometry(area);
            m_fullScreenWindow->show();
        } else {
            m_fullScreenWindow->setGeometry(screen->geometry());
            m_fullScreenWindow->showFullScreen();
        }
        show();
        m_fullScreenWindow->raise();
        m_fullScreenWindow->activateWindow();
        setFocus();
        showFullScreenBar();
    } else {
        m_fullScreen = false;
        m_fullScreenBar->hide();
        auto *tabs = m_tabs.data();
        if (!tabs || !m_placeholder) {
            m_fullScreenWindow->deleteLater();
            return;
        }
        const int index = tabs->indexOf(m_placeholder);
        disconnect(m_placeholder, nullptr, this, nullptr);
        m_fullScreenWindow->hide();
        const QSignalBlocker blocker(tabs);
        tabs->insertTab(index, this, m_tabIcon, m_tabTitle);
        m_fullScreenWindow->deleteLater();
        m_fullScreenWindow = nullptr;
        tabs->setTabToolTip(index, m_tabToolTip);
        tabs->removeTab(tabs->indexOf(m_placeholder));
        delete m_placeholder;
        m_placeholder = nullptr;
        tabs->setCurrentIndex(tabs->indexOf(this));
        show();
        setFocus();
        tabs->window()->activateWindow();
    }
    fullScreenToggled();
    emit fullScreenChanged(m_fullScreen);
}

QTabWidget *RemoteScreenPage::tabWidget() const
{
    for (auto *widget = parentWidget(); widget; widget = widget->parentWidget()) {
        if (auto *tabs = qobject_cast<QTabWidget *>(widget))
            return tabs;
    }
    return nullptr;
}

void RemoteScreenPage::requestClose()
{
    // The tab is closed from its place in the tab bar.
    if (m_fullScreen)
        setFullScreen(false);
    emit closeRequested();
}

void RemoteScreenPage::showFullScreenBar()
{
    m_fullScreenText->setText(m_tabTitle);
    m_fullScreenBar->adjustSize();
    // Centred on the primary (or only) monitor.
    QRect area = rect();
    if (m_allMonitors && QGuiApplication::primaryScreen())
        area = QRect(mapFromGlobal(QGuiApplication::primaryScreen()->geometry().topLeft()),
                     QGuiApplication::primaryScreen()->geometry().size());
    m_fullScreenBar->move(area.left() + (area.width() - m_fullScreenBar->width()) / 2, area.top());
    m_fullScreenBar->show();
    m_fullScreenBar->raise();
    m_fullScreenBarTimer->start();
}
