// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "hoststatus.h"
#include "serialoptions.h"

#include <QSet>
#include <QTcpSocket>
#include <QTimer>

namespace {

constexpr int MaximumRunning = 12;
constexpr int TimeoutMs = 3000;

}

void HostStatusChecker::check(const QList<Target> &targets)
{
    QSet<QString> listed;
    for (const auto &target : targets) {
        listed.insert(target.id);
        // A check already queued or running for this session is enough.
        if (m_status.value(target.id) == Status::Checking)
            continue;
        if (target.serial) {
            bool present = false;
            for (const auto &[name, label] : availableSerialPorts())
                present = present || name.compare(target.host, Qt::CaseInsensitive) == 0;
            m_status.insert(target.id, present ? Status::Up : Status::Down);
            continue;
        }
        m_status.insert(target.id, Status::Checking);
        m_queue.append(target);
    }
    // Sessions that cannot be checked (or are gone) show no status.
    for (auto it = m_status.begin(); it != m_status.end();) {
        if (!listed.contains(it.key()))
            it = m_status.erase(it);
        else
            ++it;
    }
    emit changed();
    startNext();
}

void HostStatusChecker::clear()
{
    ++m_generation;
    m_queue.clear();
    // Checks still under way keep counting until they end, so the limit on
    // simultaneous checks holds across a clear.
    m_status.clear();
    emit changed();
}

void HostStatusChecker::startNext()
{
    while (m_running < MaximumRunning && !m_queue.isEmpty()) {
        const Target target = m_queue.takeFirst();
        ++m_running;
        auto *socket = new QTcpSocket(this);
        auto *timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        const int generation = m_generation;
        const auto finish = [this, socket, timeout, id = target.id, generation](bool up) {
            timeout->stop();
            timeout->disconnect(this);
            socket->disconnect(this);
            socket->abort();
            socket->deleteLater();
            --m_running;
            if (generation != m_generation) {
                startNext();
                return;
            }
            if (m_status.contains(id))
                m_status.insert(id, up ? Status::Up : Status::Down);
            emit changed();
            startNext();
        };
        connect(socket, &QTcpSocket::connected, this, [finish] { finish(true); });
        connect(socket, &QTcpSocket::errorOccurred, this, [finish] { finish(false); });
        connect(timeout, &QTimer::timeout, this, [finish] { finish(false); });
        timeout->start(TimeoutMs);
        socket->connectToHost(target.host, target.port);
    }
}
