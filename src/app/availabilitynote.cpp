// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "availabilitynote.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

AvailabilityNote::AvailabilityNote(std::function<QString()> problem, std::function<void(bool)> apply,
                                   QWidget *parent)
    : QWidget(parent), m_problem(std::move(problem)), m_apply(std::move(apply))
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_text = new QLabel(this);
    m_text->setWordWrap(true);
    m_text->setStyleSheet(QStringLiteral("color: #b9770e;"));
    auto *retry = new QPushButton(QStringLiteral("Check again"), this);
    layout->addWidget(m_text, 1);
    layout->addWidget(retry);
    connect(retry, &QPushButton::clicked, this, &AvailabilityNote::check);
    check();
}

bool AvailabilityNote::check()
{
    const QString reason = m_problem ? m_problem() : QString();
    m_text->setText(QStringLiteral("⚠ %1").arg(reason));
    setVisible(!reason.isEmpty());
    if (m_apply)
        m_apply(reason.isEmpty());
    return reason.isEmpty();
}
