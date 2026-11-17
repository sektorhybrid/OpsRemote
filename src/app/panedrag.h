// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QObject>
#include <QPixmap>
#include <QPoint>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <optional>

class QLabel;
class QTabWidget;
class QWidget;
class TerminalPage;

// Moving terminals to other places by dragging, as Terminator does: a pane
// by its bar (or the grip at its top edge when it has no bar), a whole tab
// pulled down out of the tab strip. While the button is held, the half of
// the pane under the pointer where it would go is coloured, and a small
// picture of what is dragged follows the pointer. Held over a tab, that tab
// opens after a moment; let go on a tab: next to its active pane; on the
// strip past the tabs: a tab of its own. Esc cancels.
//
// No system drag and drop: the widget pressed keeps getting the pointer's
// moves while the button is held, and they are taken from it here.
class PaneDrag final : public QObject
{
    Q_OBJECT
public:
    struct Drop
    {
        bool newTab = false;
        int tabIndex = -1; // newTab: where (-1: at the end)
        QPointer<TerminalPage> beside;
        Qt::Orientation orientation = Qt::Horizontal;
        bool before = false; // left of beside, or above it
    };

    explicit PaneDrag(QTabWidget *tabs);

    // Whether dragged (a pane, or a tab's page) can go next to target.
    std::function<bool(QWidget *dragged, TerminalPage *target)> canDrop;
    // The panes of a tab's page, to drop next to.
    std::function<QList<TerminalPage *>(QWidget *tabPage)> panesOf;
    // The pane a tab's page shows active (let go on its tab: next to it).
    std::function<TerminalPage *(QWidget *tabPage)> activePaneOf;
    // Let go where it can go.
    std::function<void(QWidget *dragged, const Drop &drop)> dropped;
    // Whether a tab's page can be dragged out of the tab strip.
    std::function<bool(QWidget *tabPage)> movable;

    // Starts dragging (the mouse button is down): until it is let go.
    void start(QWidget *dragged);
    bool dragging() const { return m_dragged; }
    // What letting go at global would do, shown as the coloured part (also
    // for tests); nothing when it can't go there.
    std::optional<Drop> hover(const QPoint &global);
    // Ends the drag; with drop, moves what was dragged there.
    void finish(const QPoint &global, bool drop);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *window() const;
    void showIndicator(const QRect &globalRect);

    QTabWidget *m_tabs;
    QPointer<QWidget> m_dragged;
    QPointer<QWidget> m_indicator;
    QPointer<QLabel> m_picture;
    QTimer m_openTab; // a tab held over opens
    int m_overTab = -1;
    // A tab pressed in the tab strip, to drag it out when pulled down.
    int m_pressedTab = -1;
    QPoint m_pressedAt;
};
