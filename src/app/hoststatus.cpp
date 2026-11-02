// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "hoststatus.h"
#include "serialoptions.h"
#include "preferences.h"
#include "ftpoptions.h"
#include "proxy.h"

#include <QEventLoop>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <functional>
#include <optional>
#include <utility>

namespace {
constexpr int MaximumRunning = 12;
constexpr int TimeoutMs = 3000;
}

HostStatusChecker::~HostStatusChecker()
{
    // A normal probe has a three-second timeout. Leave a small margin for
    // cancellation polling and its final signal, but never block shutdown
    // indefinitely if an OS settings/serial call is slow.
    shutdown(TimeoutMs + 500);
}

void HostStatusChecker::check(const QList<Target> &targets)
{
    if (m_running) {
        // Do not create a second worker while the old one is still unwinding.
        // The latest tree snapshot is the only useful one, so replace a
        // previously queued request and ask the current worker to finish.
        m_pendingTargets = targets;
        if (m_cancel)
            *m_cancel = true;
        return;
    }
    start(targets);
}

void HostStatusChecker::start(const QList<Target> &targets)
{
    m_running = true;
    const int generation = ++m_generation;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    auto result = std::make_shared<QHash<QString, Status>>();
    for (const auto &target : targets)
        result->insert(target.id, Status::Checking);
    m_status = *result;
    emit changed();

    // The worker owns its event loop, sockets, settings reads and serial
    // enumeration. Only the completed snapshot crosses back to the GUI.
    auto *thread = QThread::create([targets, cancel, result] {
        const auto defaultProxy = loadSshPreferences().proxy;
        QList<QPair<QString, QString>> serialPorts;
        bool serialPortsLoaded = false;
        std::optional<bool> implicitFtp;
        QList<Target> queue;
        result->clear();
        for (auto target : targets) {
            if (*cancel)
                return;
            if (target.serial) {
                if (!serialPortsLoaded) {
                    serialPorts = availableSerialPorts();
                    serialPortsLoaded = true;
                }
                bool present = false;
                for (const auto &[name, label] : serialPorts)
                    present |= name.compare(target.host, Qt::CaseInsensitive) == 0;
                result->insert(target.id, present ? Status::Up : Status::Down);
            } else if (ProxySettings::parse(target.proxy.isEmpty() ? defaultProxy : target.proxy).type
                       == ProxySettings::Type::None) {
                // Only FTP's inherited default leaves its port unresolved.
                if (target.port == 0) {
                    if (!implicitFtp)
                        implicitFtp = loadFtpDefaults().security == QStringLiteral("implicit");
                    target.port = *implicitFtp ? 990 : 21;
                }
                queue.append(target);
            }
        }
        QEventLoop loop;
        QObject sockets;
        int running = 0;
        qsizetype next = 0;
        std::function<void()> startNext;
        startNext = [&] {
            if (*cancel) {
                loop.quit();
                return;
            }
            while (running < MaximumRunning && next < queue.size()) {
                const auto target = queue[next++];
                ++running;
                auto *socket = new QTcpSocket(&sockets);
                auto *timeout = new QTimer(socket);
                timeout->setSingleShot(true);
                auto done = std::make_shared<bool>(false);
                const auto finish = [&, socket, timeout, done, id = target.id](bool up) {
                    if (std::exchange(*done, true))
                        return;
                    timeout->stop();
                    socket->disconnect(&loop);
                    timeout->disconnect(&loop);
                    socket->abort();
                    socket->deleteLater();
                    result->insert(id, up ? Status::Up : Status::Down);
                    --running;
                    QTimer::singleShot(0, &loop, startNext);
                };
                QObject::connect(socket, &QTcpSocket::connected, &loop, [finish] { finish(true); });
                QObject::connect(socket, &QTcpSocket::errorOccurred, &loop, [finish] { finish(false); });
                QObject::connect(timeout, &QTimer::timeout, &loop, [finish] { finish(false); });
                timeout->start(TimeoutMs);
                socket->connectToHost(target.host, target.port);
            }
            if (!running && next == queue.size())
                loop.quit();
        };
        QTimer cancellation;
        QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
            if (*cancel)
                loop.quit();
        });
        cancellation.start(50);
        QTimer::singleShot(0, &loop, startNext);
        loop.exec();
        // Cancelled sockets must not call closures referring to locals while
        // those locals are being destroyed.
        for (auto *object : sockets.findChildren<QObject *>())
            object->disconnect(&loop);
    });
    m_thread = thread;
    connect(thread, &QThread::finished, this, [this, thread, generation, result] {
        if (m_thread != thread)
            return;
        m_thread = nullptr;
        m_running = false;
        m_cancel.reset();
        if (m_pendingTargets) {
            const auto pending = std::move(*m_pendingTargets);
            m_pendingTargets.reset();
            start(pending);
            return;
        }
        if (generation == m_generation)
            m_status = *result;
        emit changed();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void HostStatusChecker::clear()
{
    ++m_generation;
    m_pendingTargets.reset();
    if (m_cancel)
        *m_cancel = true;
    // m_running stays true until the worker has actually exited. That keeps
    // a timer tick or session reload from starting overlapping probe batches.
    if (!m_thread) {
        m_cancel.reset();
        m_running = false;
    }
    m_status.clear();
    emit changed();
}

bool HostStatusChecker::shutdown(int timeoutMs)
{
    ++m_generation;
    m_pendingTargets.reset();
    if (!m_thread)
        return true;
    if (m_cancel)
        *m_cancel = true;
    // The worker's local QEventLoop sees the cancellation flag within 50 ms.
    // quit() is also harmless if it has already returned to QThread's loop.
    m_thread->quit();
    const bool finished = m_thread->wait(timeoutMs);
    if (!finished)
        qWarning() << "Host-status check did not finish before shutdown deadline";
    return finished;
}
