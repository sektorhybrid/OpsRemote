// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QColor>
#include <QWidget>

class QVariantAnimation;

// Of the candidates, the first that stands out against background (a WCAG
// contrast ratio of 3 or more), else the one that stands out most. Used for
// marks that must show with every look.
QColor visibleColor(const QColor &background, const QList<QColor> &candidates);

// The mark of a background tab with new output: a dot beside its title,
// painted by OpsRemote so it shows with every style. Pulsing, it fades
// softly between two colours. Every tab keeps one, idle (empty), so a
// tab does not grow when it is marked.
class ActivityDot final : public QWidget
{
    Q_OBJECT

public:
    explicit ActivityDot(QWidget *parent = nullptr);

    void setColors(const QColor &first, const QColor &second);
    void setPulsing(bool on);
    void setActive(bool on);
    bool isActive() const { return m_active; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QColor m_first;
    QColor m_second;
    qreal m_phase = 0; // 0: first colour, 1: second
    bool m_active = false;
    bool m_pulsing = false;
    QVariantAnimation *m_animation = nullptr;
};
