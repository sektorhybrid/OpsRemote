// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QFrame>

class QLabel;
class QPushButton;

// Shown when a session tab's connection has ended: why, and what to do next
// (Reconnect, or Close Tab). A card for remote desktop tabs, a bar across
// the top of terminal tabs. Enter and Esc are handled by the tab itself,
// since the keyboard focus stays there.
class SessionEndedPanel final : public QFrame
{
    Q_OBJECT

public:
    enum class Style { Card, Bar };
    explicit SessionEndedPanel(Style style, QWidget *parent = nullptr);

    // error: the message is shown as a problem (not a normal logout).
    void setMessage(const QString &title, const QString &detail, bool error);

signals:
    void reconnectRequested();
    void closeRequested();

private:
    Style m_style;
    QLabel *m_title = nullptr;
    QLabel *m_detail = nullptr;
    QPushButton *m_reconnect = nullptr;
    QPushButton *m_close = nullptr;
};
