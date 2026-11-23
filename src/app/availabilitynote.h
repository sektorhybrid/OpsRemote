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
    // Where problem runs. Background: on a worker thread, so a slow check
    // (a connection attempt, a long PATH) never holds up the window; the
    // option keeps its state until the answer comes, and problem must not
    // touch widgets. Here: at once, on the window's thread (a check that
    // reads fields of the window).
    enum class Run { Background, Here };

    AvailabilityNote(std::function<QString()> problem, std::function<void(bool)> apply,
                     QWidget *parent = nullptr, Run run = Run::Here);

    // Runs the check again (in the background: the answer comes later).
    void check();

private:
    void show(const QString &reason);

    std::function<QString()> m_problem;
    std::function<void(bool)> m_apply;
    Run m_run;
    int m_generation = 0; // a later check's answer replaces an earlier one's
    QLabel *m_text = nullptr;
};
