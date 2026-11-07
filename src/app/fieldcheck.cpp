// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "fieldcheck.h"

#include <QPushButton>
#include <QStyle>
#include <QVariant>
#include <QWidget>

void markInvalid(QWidget *field, bool invalid)
{
    if (!field || field->property("invalid").toBool() == invalid)
        return;
    field->setProperty("invalid", invalid);
    // A border of its own only while wrong: the look's style otherwise.
    field->setStyleSheet(invalid ? QStringLiteral("border: 1px solid #d9534f; border-radius: 3px;")
                                 : QString());
}

SaveGuard::SaveGuard(QPushButton *save, QObject *parent)
    : QObject(parent ? parent : save), m_save(save), m_tooltip(save ? save->toolTip() : QString())
{
}

void SaveGuard::addCheck(std::function<QString()> check)
{
    m_checks.append(std::move(check));
    recheck();
}

void SaveGuard::recheck()
{
    if (!m_save)
        return;
    QStringList problems;
    for (const auto &check : std::as_const(m_checks))
        if (const auto problem = check(); !problem.isEmpty())
            problems.append(problem);
    m_save->setEnabled(problems.isEmpty());
    m_save->setToolTip(problems.isEmpty() ? m_tooltip
                                          : QStringLiteral("Not yet: %1").arg(problems.join(QLatin1Char(' '))));
}
