// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "panedrag.h"

#include "terminalpage.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QTabBar>
#include <QTabWidget>

#include <algorithm>

namespace {

// The coloured part where the drop would go.
class DropIndicator final : public QWidget
{
public:
    explicit DropIndicator(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QColor colour = palette().color(QPalette::Highlight);
        painter.setPen(QPen(colour, 2));
        colour.setAlpha(110);
        painter.setBrush(colour);
        painter.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
    }
};

// A picture of what is dragged, this wide at most.
constexpr int PictureWidth = 220;
// How far below the tab strip a tab is pulled before it comes out.
constexpr int PullOut = 16;

}

PaneDrag::PaneDrag(QTabWidget *tabs) : QObject(tabs), m_tabs(tabs)
{
    m_openTab.setSingleShot(true);
    m_openTab.setInterval(500);
    connect(&m_openTab, &QTimer::timeout, this, [this] {
        if (m_dragged && m_overTab >= 0 && m_overTab < m_tabs->count())
            m_tabs->setCurrentIndex(m_overTab);
    });
    m_tabs->tabBar()->installEventFilter(this);
}

QWidget *PaneDrag::window() const
{
    return m_tabs->window();
}

void PaneDrag::start(QWidget *dragged)
{
    if (!dragged || m_dragged)
        return;
    m_dragged = dragged;
    // The picture: what is dragged, small, with an accent frame.
    QPixmap shot = dragged->grab();
    const int width = std::min(PictureWidth, window()->width() / 5);
    if (shot.width() > width)
        shot = shot.scaledToWidth(width, Qt::SmoothTransformation);
    QPixmap framed(shot.size() + QSize(4, 4));
    framed.fill(dragged->palette().color(QPalette::Highlight));
    {
        QPainter painter(&framed);
        painter.drawPixmap(2, 2, shot);
    }
    m_picture = new QLabel(window());
    m_picture->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_picture->setPixmap(framed);
    m_picture->resize(framed.size());
    m_indicator = new DropIndicator(window());
    m_indicator->hide();
    qApp->installEventFilter(this);
    QApplication::setOverrideCursor(Qt::ClosedHandCursor);
    hover(QCursor::pos());
}

std::optional<PaneDrag::Drop> PaneDrag::hover(const QPoint &global)
{
    if (m_picture) {
        m_picture->move(window()->mapFromGlobal(global) + QPoint(14, 18));
        m_picture->show();
        m_picture->raise();
    }
    std::optional<Drop> drop;
    QRect area; // the coloured part, global

    // The tab strip's row: a tab (its active pane, after a moment its
    // panes), or past the tabs: a tab of its own.
    auto *bar = m_tabs->tabBar();
    const QPoint inTabs = m_tabs->mapFromGlobal(global);
    const QRect strip(0, bar->geometry().top(), m_tabs->width(), bar->height());
    int overTab = -1;
    if (strip.contains(inTabs)) {
        overTab = bar->tabAt(bar->mapFromGlobal(global));
        if (overTab >= 0) {
            auto *active = activePaneOf ? activePaneOf(m_tabs->widget(overTab)) : nullptr;
            if (active && canDrop && canDrop(m_dragged, active)) {
                drop = Drop{false, -1, active, Qt::Horizontal, false};
                area = QRect(bar->mapToGlobal(bar->tabRect(overTab).topLeft()), bar->tabRect(overTab).size());
            }
        } else if (!movable || movable(m_dragged)) {
            drop = Drop{true, -1, {}, Qt::Horizontal, false};
            const int end = bar->count() > 0 ? bar->tabRect(bar->count() - 1).right() + 1 : 0;
            const QPoint left = bar->mapToGlobal(QPoint(end, 0));
            area = QRect(left, QPoint(m_tabs->mapToGlobal(QPoint(m_tabs->width() - 1, 0)).x(),
                                      left.y() + bar->height() - 1));
        }
    }
    if (overTab != m_overTab) {
        m_overTab = overTab;
        if (overTab >= 0 && overTab != m_tabs->currentIndex())
            m_openTab.start();
        else
            m_openTab.stop();
    }

    // A pane of the tab shown: the half nearest the pointer.
    if (!drop && overTab < 0 && panesOf) {
        for (auto *pane : panesOf(m_tabs->currentWidget())) {
            const QPoint at = pane->mapFromGlobal(global);
            if (!pane->isVisible() || !pane->rect().contains(at))
                continue;
            if (!canDrop || !canDrop(m_dragged, pane))
                break;
            const double x = double(at.x()) / qMax(1, pane->width());
            const double y = double(at.y()) / qMax(1, pane->height());
            const double nearest = std::min({x, 1 - x, y, 1 - y});
            Drop next{false, -1, pane, Qt::Horizontal, false};
            QRect half = pane->rect();
            if (nearest == x) {
                next.before = true;
                half.setWidth(half.width() / 2);
            } else if (nearest == 1 - x) {
                half.setLeft(half.width() / 2);
            } else if (nearest == y) {
                next.orientation = Qt::Vertical;
                next.before = true;
                half.setHeight(half.height() / 2);
            } else {
                next.orientation = Qt::Vertical;
                half.setTop(half.height() / 2);
            }
            drop = next;
            area = QRect(pane->mapToGlobal(half.topLeft()), half.size());
            break;
        }
    }

    if (drop)
        showIndicator(area);
    else if (m_indicator)
        m_indicator->hide();
    if (m_dragged)
        QApplication::changeOverrideCursor(drop ? Qt::ClosedHandCursor : Qt::ForbiddenCursor);
    return drop;
}

void PaneDrag::showIndicator(const QRect &globalRect)
{
    if (!m_indicator)
        return;
    m_indicator->setGeometry(QRect(window()->mapFromGlobal(globalRect.topLeft()), globalRect.size()));
    m_indicator->show();
    m_indicator->raise();
    if (m_picture)
        m_picture->raise();
}

void PaneDrag::finish(const QPoint &global, bool drop)
{
    if (!m_dragged)
        return;
    const auto where = drop ? hover(global) : std::nullopt;
    const QPointer<QWidget> dragged = std::exchange(m_dragged, nullptr);
    qApp->removeEventFilter(this);
    QApplication::restoreOverrideCursor();
    m_openTab.stop();
    m_overTab = -1;
    delete m_indicator;
    delete m_picture;
    if (where && dragged && dropped)
        dropped(dragged, *where);
}

bool PaneDrag::eventFilter(QObject *watched, QEvent *event)
{
    if (m_dragged) {
        // While dragging, the pointer and Esc are this drag's alone.
        switch (event->type()) {
        case QEvent::MouseMove:
            hover(static_cast<QMouseEvent *>(event)->globalPosition().toPoint());
            return true;
        case QEvent::MouseButtonRelease:
            finish(static_cast<QMouseEvent *>(event)->globalPosition().toPoint(), true);
            return true;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
            return true;
        case QEvent::KeyPress:
            if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape)
                finish({}, false);
            return true;
        case QEvent::ShortcutOverride:
        case QEvent::KeyRelease:
            return true;
        default:
            return false;
        }
    }

    // A tab pulled down out of the tab strip is dragged.
    auto *bar = m_tabs->tabBar();
    if (watched != bar)
        return false;
    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        m_pressedTab = mouse->button() == Qt::LeftButton ? bar->tabAt(mouse->position().toPoint()) : -1;
        m_pressedAt = mouse->position().toPoint();
    } else if (event->type() == QEvent::MouseButtonRelease) {
        m_pressedTab = -1;
    } else if (event->type() == QEvent::MouseMove && m_pressedTab >= 0) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (!(mouse->buttons() & Qt::LeftButton) || mouse->position().y() < bar->height() + PullOut)
            return false;
        QWidget *page = m_tabs->widget(m_pressedTab);
        m_pressedTab = -1;
        if (!page || (movable && !movable(page)))
            return false;
        // The tab strip lets go of the tab (it snaps back) first.
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(m_pressedAt), bar->mapToGlobal(QPointF(m_pressedAt)),
                            Qt::LeftButton, Qt::NoButton, mouse->modifiers());
        QApplication::sendEvent(bar, &release);
        start(page);
        return true;
    }
    return false;
}
