// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "panebars.h"

#include "statusindicators.h"

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QFont>
#include <QResizeEvent>
#include <QWidget>

#include <optional>

namespace {

// A pane's grip, at its top edge when it has no bar: shown when the pointer
// rests there, pressed and moved to drag the pane.
class PaneGrip final : public QWidget
{
public:
    explicit PaneGrip(QWidget *parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_NoSystemBackground);
        setToolTip(QStringLiteral("Drag to move this pane"));
        m_wait.setSingleShot(true);
        m_wait.setInterval(300);
        QObject::connect(&m_wait, &QTimer::timeout, this, [this] { setShown(true); });
    }
    std::function<void()> dragged;
    static constexpr int Height = 7;

protected:
    void enterEvent(QEnterEvent *) override { m_wait.start(); }
    void leaveEvent(QEvent *) override
    {
        m_wait.stop();
        setShown(false);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressedAt.reset();
        if (event->button() == Qt::LeftButton)
            m_pressedAt = event->position().toPoint();
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_pressedAt && (event->buttons() & Qt::LeftButton)
            && (event->position().toPoint() - *m_pressedAt).manhattanLength() >= QApplication::startDragDistance()) {
            m_pressedAt.reset();
            setShown(false);
            if (dragged)
                dragged();
        }
    }
    void paintEvent(QPaintEvent *) override
    {
        if (!m_shown)
            return;
        // A pill in the middle of the edge.
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Highlight));
        const QRectF pill((width() - 48) / 2.0, 1.5, 48, Height - 3);
        painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
    }

private:
    void setShown(bool shown)
    {
        if (m_shown == shown)
            return;
        m_shown = shown;
        setCursor(shown ? Qt::OpenHandCursor : Qt::ArrowCursor);
        update();
    }
    QTimer m_wait;
    bool m_shown = false;
    std::optional<QPoint> m_pressedAt;
};

}

PaneBars::PaneBars(QWidget *host) : QObject(host), m_host(host)
{
    host->installEventFilter(this);
}

void PaneBars::setPlace(const QString &place, const QString &mode)
{
    m_place = place;
    m_mode = mode;
    placeAll();
}

TabMessageStrip *PaneBars::bar(QWidget *page)
{
    auto &pane = m_panes[page];
    if (pane.bar)
        return pane.bar;
    pane.bar = new TabMessageStrip(m_host);
    pane.bar->setCountShown(false);
    pane.bar->hide();
    pane.given = page->geometry();
    pane.placed = page->geometry();
    page->installEventFilter(this);
    pane.bar->installEventFilter(this);
    connect(pane.bar, &TabMessageStrip::dragRequested, this, [this, page] {
        if (movable && movable(page))
            emit dragRequested(page);
    });
    connect(page, &QObject::destroyed, this, [this, page] {
        const auto pane = m_panes.take(page);
        if (pane.bar)
            pane.bar->deleteLater();
        if (pane.grip)
            pane.grip->deleteLater();
    });
    emit barCreated(page, pane.bar);
    place(page);
    return pane.bar;
}

void PaneBars::setNeeded(QWidget *page, bool needed)
{
    auto it = m_panes.find(page);
    if (m_stopped || it == m_panes.end() || it->needed == needed)
        return;
    it->needed = needed;
    place(page);
}

void PaneBars::stop()
{
    m_stopped = true;
    placed = {};
    m_host->removeEventFilter(this);
    for (auto it = m_panes.cbegin(); it != m_panes.cend(); ++it) {
        it.key()->removeEventFilter(this);
        if (it->bar)
            it->bar->removeEventFilter(this);
    }
}

void PaneBars::placeAll()
{
    for (auto it = m_panes.cbegin(); it != m_panes.cend(); ++it)
        place(it.key());
}

bool PaneBars::pushes(QWidget *page, const Pane &pane) const
{
    if (!shown())
        return false;
    if (m_mode == QStringLiteral("fixed"))
        return true;
    return pane.needed && !(floats && floats(page));
}

bool PaneBars::barShown(QWidget *page, const Pane &pane) const
{
    return shown() && (m_mode == QStringLiteral("fixed") || pane.needed) && page->isVisibleTo(m_host);
}

// The pane in its room (less the bar's, when the bar pushes it), and the bar
// at the room's top or bottom edge, over the pane when it floats.
void PaneBars::place(QWidget *page)
{
    auto it = m_panes.find(page);
    if (m_stopped || it == m_panes.end() || !it->bar)
        return;
    auto &pane = *it;
    const int height = pane.bar->height();
    const bool atBottom = m_place == QStringLiteral("bottom");
    QRect wanted = pane.given;
    if (pushes(page, pane))
        wanted = atBottom ? wanted.adjusted(0, 0, 0, -height) : wanted.adjusted(0, height, 0, 0);
    if (page->geometry() != wanted) {
        // Its own move is not a new room (Qt may keep the pane at its minimum
        // size instead: taking that for a new room looped without end).
        m_placing = page;
        page->setGeometry(wanted);
        m_placing = nullptr;
    }
    pane.placed = page->geometry();
    // Out of the tabs for a moment (being moved): nothing to place by.
    if (!page->parentWidget() || !m_host->isAncestorOf(page)) {
        pane.bar->hide();
        placeGrip(page, pane, false);
        return;
    }
    const bool shownBar = barShown(page, pane);
    placeGrip(page, pane, !shownBar);
    if (!shownBar) {
        pane.bar->hide();
        return;
    }
    const QPoint corner = page->parentWidget()->mapTo(m_host, pane.given.topLeft());
    pane.bar->setGeometry(corner.x(), atBottom ? corner.y() + pane.given.height() - height : corner.y(),
                          pane.given.width(), height);
    pane.bar->show();
    pane.bar->raise();
    if (placed)
        placed();
}

// Without a bar, a movable pane gets its grip along its top edge.
void PaneBars::placeGrip(QWidget *page, Pane &pane, bool wanted)
{
    wanted = wanted && movable && movable(page) && page->isVisibleTo(m_host);
    if (!wanted) {
        if (pane.grip)
            pane.grip->hide();
        return;
    }
    if (!pane.grip) {
        auto *grip = new PaneGrip(m_host);
        grip->dragged = [this, page = QPointer<QWidget>(page)] {
            if (page)
                emit dragRequested(page);
        };
        pane.grip = grip;
    }
    const QPoint corner = page->parentWidget()->mapTo(m_host, pane.placed.topLeft());
    pane.grip->setGeometry(corner.x(), corner.y(), pane.placed.width(), PaneGrip::Height);
    pane.grip->show();
    pane.grip->raise();
}

// The tab or its splitter gives the pane its room: kept, and the pane moved
// in by the bar before it sees the change (resized once, not twice).
bool PaneBars::eventFilter(QObject *watched, QEvent *event)
{
    // A new interface font: every bar takes it again (a bar made while the
    // font was being handed down kept the old one).
    if (watched == m_host) {
        if (event->type() == QEvent::FontChange)
            for (const auto &pane : std::as_const(m_panes))
                if (pane.bar)
                    pane.bar->setFont(QFont());
        return QObject::eventFilter(watched, event);
    }
    // A bar of another height (a new font or look): its pane moves out of
    // its way again (before, the bar covered the terminal's last line).
    if (auto *bar = qobject_cast<TabMessageStrip *>(watched)) {
        if (event->type() == QEvent::Resize
            && static_cast<QResizeEvent *>(event)->oldSize().height() != bar->height()) {
            QWidget *page = nullptr;
            for (auto it = m_panes.cbegin(); it != m_panes.cend() && !page; ++it)
                if (it->bar == bar)
                    page = it.key();
            if (page)
                place(page);
        }
        return QObject::eventFilter(watched, event);
    }
    auto *page = static_cast<QWidget *>(watched);
    auto it = m_panes.find(page);
    if (it == m_panes.end() || page == m_placing)
        return QObject::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
        if (page->geometry() != it->placed) {
            it->given = page->geometry();
            place(page);
            return true;
        }
        // Moved with its parent's room unchanged: the bar follows.
        place(page);
        break;
    case QEvent::Show:
    case QEvent::Hide:
        place(page);
        break;
    case QEvent::ParentChange:
        // Moved to another place: its next room is a new one.
        it->placed = QRect();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
