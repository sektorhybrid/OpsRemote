// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "statusline.h"

StatusLine::StatusLine(QWidget *parent) : QLabel(parent)
{
    setFixedHeight(fontMetrics().height() + 2);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setTextInteractionFlags(Qt::TextSelectableByMouse);
}

void StatusLine::setStatus(const QString &text, bool problem)
{
    m_full = text.simplified();
    // The same warning colour as AvailabilityNote.
    setStyleSheet(problem ? QStringLiteral("color: #b9770e;") : QString());
    setForegroundRole(problem ? QPalette::WindowText : QPalette::PlaceholderText);
    setToolTip(m_full.size() > 0 ? text : QString());
    elide();
}

void StatusLine::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    elide();
}

void StatusLine::elide()
{
    setText(fontMetrics().elidedText(m_full, Qt::ElideRight, qMax(0, width() - 2)));
}
