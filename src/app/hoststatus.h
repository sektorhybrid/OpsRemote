// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <atomic>
#include <memory>
#include <optional>

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
        QString proxy; // empty inherits SSH preferences, resolved off the GUI thread
    };

    using QObject::QObject;
    ~HostStatusChecker() override;

    // Checks these, a few at a time; Unknown for sessions not listed.
    void check(const QList<Target> &targets);
    Status status(const QString &id) const { return m_status.value(id, Status::Unknown); }
    bool isChecking() const { return m_running; }
    void clear();

    // Cancels the active check and waits only for its normal, cooperative
    // cleanup. This is used during teardown; it never terminates a thread.
    bool shutdown(int timeoutMs);

signals:
    void changed();

private:
    void start(const QList<Target> &targets);

    QHash<QString, Status> m_status;
    bool m_running = false;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QPointer<QThread> m_thread;
    std::optional<QList<Target>> m_pendingTargets;
    int m_generation = 0; // checks from before clear() are ignored
};
