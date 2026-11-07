// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QPushButton;
class QWidget;

// Required fields, the same in every window: a field that is missing or wrong
// gets a red border, the reason shows on the StatusLine under it, and
// Save/OK stays disabled (its tooltip says why) until it is fixed. Changing
// tabs stays possible.

// Marks a field (line edit, spin box, combo box) as wrong, or clears it.
void markInvalid(QWidget *field, bool invalid);

class SaveGuard final : public QObject
{
    Q_OBJECT

public:
    // save: the button that stays disabled while a check fails.
    explicit SaveGuard(QPushButton *save, QObject *parent = nullptr);

    // A check returns what is wrong, or an empty string. Call recheck()
    // whenever what it looks at changes.
    void addCheck(std::function<QString()> check);
    void recheck();

private:
    QPushButton *m_save = nullptr;
    QString m_tooltip; // the button's own, while nothing is wrong
    QList<std::function<QString()>> m_checks;
};
