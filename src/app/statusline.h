// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QLabel>

// One line under a field for its state ("No pods found", an error): always
// there, one line tall even when empty, so a message never moves the rest
// of the window. Long messages end in "…"; the tooltip has all of it.
class StatusLine final : public QLabel
{
    Q_OBJECT

public:
    explicit StatusLine(QWidget *parent = nullptr);

    // problem: shown as a warning; otherwise as a quiet note.
    void setStatus(const QString &text, bool problem = false);
    void clearStatus() { setStatus(QString()); }

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void elide();
    QString m_full;
};
