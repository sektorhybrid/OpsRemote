// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

class QVBoxLayout;
class TerminalPage;

// A tab split into terminal panes: split right or down as often as wanted,
// with dividers to drag. The active pane (the one last clicked or typed in)
// has a thin accent frame.
class SplitTab final : public QWidget
{
    Q_OBJECT

public:
    explicit SplitTab(TerminalPage *first, QWidget *parent = nullptr);

    // Puts added next to beside: on its right (Qt::Horizontal) or below it.
    void split(TerminalPage *beside, TerminalPage *added, Qt::Orientation orientation);
    // Takes a pane out (the caller deletes it); empty dividers go too.
    void removePane(TerminalPage *pane);
    QList<TerminalPage *> panes() const;
    TerminalPage *activePane() const;
    void setActivePane(TerminalPage *pane);

signals:
    void activePaneChanged(TerminalPage *pane);

private:
    QWidget *frameOf(TerminalPage *pane) const;
    QWidget *wrap(TerminalPage *pane);
    void updateFrames();

    QVBoxLayout *m_layout = nullptr;
    QPointer<TerminalPage> m_active;
};
