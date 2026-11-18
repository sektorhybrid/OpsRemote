// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <functional>

class QWidget;
class TabMessageStrip;

// A bar on each pane of the session tabs (a tab's page, or each pane of a
// split tab) with what that pane says: a question, its newest message, its
// transfer. Placed from outside the pane, so every kind of pane gets it the
// same way: when the pane is given its room (by the tab or its splitter),
// it is moved in by the bar's height before it sees the change, so it is
// resized once, and the bar takes the room left.
//
// "fixed": every pane always has its bar, empty when there is nothing to
// say; nothing ever moves. "auto": a bar shows only while there is
// something; a terminal (any pane but a remote screen) is pushed down by it,
// a remote screen is covered at its edge instead (a remote desktop resized
// whenever a bar came or went would flicker and change its resolution).
class PaneBars final : public QObject
{
    Q_OBJECT
public:
    // host: where the bars live (above the panes), the session tab widget.
    explicit PaneBars(QWidget *host);

    // place: "top", "bottom" or "off" (no bars); mode: "fixed" or "auto".
    void setPlace(const QString &place, const QString &mode);
    bool shown() const { return m_place != QStringLiteral("off"); }
    // Panes that are covered rather than pushed in "auto" (remote screens).
    std::function<bool(QWidget *)> floats;
    // Called after bars were placed (the host's rounded corners go back on top).
    std::function<void()> placed;
    // Panes that can be dragged to other places: by their bar, or without
    // one by the grip shown when the pointer rests at their top edge.
    std::function<bool(QWidget *)> movable;

    // page's bar, made on first use.
    TabMessageStrip *bar(QWidget *page);
    // Whether page has something to show ("auto" shows its bar only then).
    void setNeeded(QWidget *page, bool needed);
    // Places every bar again (after the current tab changed).
    void placeAll();
    // The window is closing: nothing is placed any more (the panes and the
    // host are being destroyed).
    void stop();

signals:
    void barCreated(QWidget *page, TabMessageStrip *bar);
    // page's bar (or grip) was pressed and moved: page is being dragged.
    void dragRequested(QWidget *page);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Pane
    {
        QPointer<TabMessageStrip> bar;
        QPointer<QWidget> grip; // see movable
        QRect given;  // the room the pane was given, in its parent
        QRect placed; // where it was put (given, less the bar's room)
        bool needed = false;
    };
    bool pushes(QWidget *page, const Pane &pane) const;
    bool barShown(QWidget *page, const Pane &pane) const;
    void place(QWidget *page);
    void placeGrip(QWidget *page, Pane &pane, bool wanted);

    QWidget *m_host;
    QString m_place = QStringLiteral("top");
    QString m_mode = QStringLiteral("fixed");
    QHash<QWidget *, Pane> m_panes;
    QWidget *m_placing = nullptr; // the pane place() is moving
    bool m_stopped = false;
};
