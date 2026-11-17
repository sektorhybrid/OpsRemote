// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QRect>

#include <QWidget>

// Rounds the corners of an area of a widget whatever is drawn there (a
// terminal, a remote screen): four small pieces over its corners paint the
// window colour outside the curve. Clicks go through them. One piece per
// corner, so a busy area under them repaints nothing more.
class RoundedCorners final : public QObject
{
    Q_OBJECT
public:
    explicit RoundedCorners(QWidget *parent);
    // The area, in the parent; empty: no corners.
    void setArea(const QRect &area);
    // Of the corners' curve (0: square), for every rounded area at once
    // (Preferences → Appearance → Corners).
    static int radius();
    static void setRadius(int radius);
    // Rounds target's own corners, wherever it moves or however it resizes.
    static void follow(QWidget *target);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *m_pieces[4] = {};
    QRect m_area;
    QPointer<QWidget> m_target; // follow()'s
};
