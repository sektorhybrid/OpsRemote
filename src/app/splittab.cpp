// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "splittab.h"
#include "terminalpage.h"

#include <QApplication>
#include <QPainter>
#include <QSplitter>
#include <QVBoxLayout>

namespace {

// A pane's frame: one pixel around the terminal, drawn in the accent colour
// for the active pane of a split tab.
class PaneFrame final : public QWidget
{
public:
    explicit PaneFrame(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("splitPane"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(1, 1, 1, 1);
    }

    void setHighlighted(bool on)
    {
        if (m_highlighted != on) {
            m_highlighted = on;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!m_highlighted)
            return;
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::Highlight));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }

private:
    bool m_highlighted = false;
};

QSplitter *newSplitter(Qt::Orientation orientation)
{
    auto *splitter = new QSplitter(orientation);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(4);
    return splitter;
}

// Gives every child of a splitter the same share.
void equalize(QSplitter *splitter)
{
    const int total = splitter->orientation() == Qt::Horizontal ? splitter->width() : splitter->height();
    const int count = splitter->count();
    if (count > 0)
        splitter->setSizes(QList<int>(count, qMax(1, total / count)));
}

}

SplitTab::SplitTab(TerminalPage *first, QWidget *parent) : QWidget(parent)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->addWidget(wrap(first));
    m_active = first;
    // The active pane follows the keyboard focus.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (QWidget *widget = now; widget && widget != this; widget = widget->parentWidget()) {
            if (auto *pane = qobject_cast<TerminalPage *>(widget); pane && isAncestorOf(pane)) {
                setActivePane(pane);
                return;
            }
        }
    });
    updateFrames();
}

QWidget *SplitTab::wrap(TerminalPage *pane)
{
    auto *frame = new PaneFrame;
    frame->layout()->addWidget(pane);
    // A terminal taken out of a tab was hidden by the tab widget.
    pane->show();
    return frame;
}

QWidget *SplitTab::frameOf(TerminalPage *pane) const
{
    return pane ? pane->parentWidget() : nullptr;
}

void SplitTab::split(TerminalPage *beside, TerminalPage *added, Qt::Orientation orientation, bool before)
{
    QWidget *frame = frameOf(beside);
    if (!frame)
        return;
    insertNextTo(frame, wrap(added), orientation, before);
    setActivePane(added);
    added->setFocus();
}

void SplitTab::insertGroup(TerminalPage *beside, SplitTab *from, Qt::Orientation orientation, bool before)
{
    QWidget *frame = frameOf(beside);
    QLayoutItem *item = from ? from->m_layout->itemAt(0) : nullptr;
    if (!frame || !item || !item->widget())
        return;
    QWidget *group = item->widget();
    from->m_layout->removeWidget(group);
    insertNextTo(frame, group, orientation, before);
    group->show();
    const auto moved = group->findChildren<TerminalPage *>();
    if (!moved.isEmpty()) {
        auto *active = from->activePane() && moved.contains(from->activePane()) ? from->activePane()
                                                                               : moved.constFirst();
        setActivePane(active);
        active->setFocus();
    }
    updateFrames();
}

void SplitTab::insertNextTo(QWidget *frame, QWidget *addedFrame, Qt::Orientation orientation, bool before)
{
    auto *parentSplitter = qobject_cast<QSplitter *>(frame->parentWidget());
    if (parentSplitter && parentSplitter->orientation() == orientation) {
        // Same direction: one more pane in this row (or column).
        parentSplitter->insertWidget(parentSplitter->indexOf(frame) + (before ? 0 : 1), addedFrame);
        equalize(parentSplitter);
    } else {
        // The pane becomes a row (or column) of two in its place.
        auto *splitter = newSplitter(orientation);
        if (parentSplitter) {
            const auto sizes = parentSplitter->sizes();
            parentSplitter->replaceWidget(parentSplitter->indexOf(frame), splitter);
            parentSplitter->setSizes(sizes);
        } else {
            m_layout->replaceWidget(frame, splitter);
        }
        splitter->addWidget(before ? addedFrame : frame);
        splitter->addWidget(before ? frame : addedFrame);
        frame->show();
        equalize(splitter);
    }
}

void SplitTab::removePane(TerminalPage *pane)
{
    QWidget *frame = frameOf(pane);
    if (!frame)
        return;
    pane->setParent(nullptr);
    auto *splitter = qobject_cast<QSplitter *>(frame->parentWidget());
    delete frame;
    // A divider left with one side goes, its remaining side taking its place.
    while (splitter && splitter->count() == 1) {
        QWidget *only = splitter->widget(0);
        auto *outer = qobject_cast<QSplitter *>(splitter->parentWidget());
        if (outer) {
            const auto sizes = outer->sizes();
            outer->replaceWidget(outer->indexOf(splitter), only);
            outer->setSizes(sizes);
        } else {
            m_layout->replaceWidget(splitter, only);
        }
        only->show();
        delete splitter;
        splitter = outer;
    }
    if (m_active == pane || !m_active) {
        const auto remaining = panes();
        m_active = remaining.isEmpty() ? nullptr : remaining.constFirst();
        if (m_active) {
            m_active->setFocus();
            emit activePaneChanged(m_active);
        }
    }
    updateFrames();
}

QList<TerminalPage *> SplitTab::panes() const
{
    return findChildren<TerminalPage *>();
}

TerminalPage *SplitTab::activePane() const
{
    return m_active;
}

void SplitTab::setActivePane(TerminalPage *pane)
{
    if (!pane || pane == m_active || !isAncestorOf(pane))
        return;
    m_active = pane;
    updateFrames();
    emit activePaneChanged(pane);
}

void SplitTab::updateFrames()
{
    const auto all = panes();
    for (auto *pane : all) {
        if (auto *frame = static_cast<PaneFrame *>(frameOf(pane)))
            frame->setHighlighted(all.size() > 1 && pane == m_active);
    }
}
