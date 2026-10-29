// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QWidget>

#include <functional>

class QLabel;

// Under an option that needs something outside OpsRemote (a service, a
// program, a library): why it is unavailable, with Check again. Hidden while
// all is well; apply(available) enables or disables the option itself.
class AvailabilityNote final : public QWidget
{
    Q_OBJECT

public:
    AvailabilityNote(std::function<QString()> problem, std::function<void(bool)> apply,
                     QWidget *parent = nullptr);

    // Runs the check again; true when the resource is available.
    bool check();

private:
    std::function<QString()> m_problem;
    std::function<void(bool)> m_apply;
    QLabel *m_text = nullptr;
};
