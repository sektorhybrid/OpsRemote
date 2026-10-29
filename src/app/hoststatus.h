// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

// Whether saved sessions' hosts answer: a TCP connection to the session's
// port, closed at once (nothing is sent, nobody logs on). Serial sessions
// are up when their port exists on this computer.
class HostStatusChecker final : public QObject
{
    Q_OBJECT

public:
    enum class Status { Unknown, Checking, Up, Down };

    struct Target
    {
        QString id;
        QString host; // a TCP host, or a serial port with serial set
        quint16 port = 0;
        bool serial = false;
    };

    using QObject::QObject;

    // Checks these, a few at a time; Unknown for sessions not listed.
    void check(const QList<Target> &targets);
    Status status(const QString &id) const { return m_status.value(id, Status::Unknown); }
    bool isChecking() const { return !m_queue.isEmpty() || m_running > 0; }
    void clear();

signals:
    void changed();

private:
    void startNext();

    QHash<QString, Status> m_status;
    QList<Target> m_queue;
    int m_running = 0;
    int m_generation = 0; // checks from before clear() are ignored
};
