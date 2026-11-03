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

HostStatusChecker::HostStatusChecker(QObject *parent)
    : QObject(parent), m_owner(std::make_shared<Owner>())
{
    m_owner->checker = this;
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
        // Do not start a second batch while the old one is still unwinding.
        // The latest tree snapshot is the only useful one, so replace a
        // previously queued request and ask the current batch to finish.
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

    // One worker thread for the checker's whole life: on Windows, every
    // thread that ends pauses the whole program for about a second (seen
    // as a GUI stall after each check when a thread was started per check).
    if (!m_thread) {
        m_thread = new QThread(this);
        m_thread->setObjectName(QStringLiteral("Host checks"));
        m_worker = new QObject;
        m_worker->moveToThread(m_thread);
        connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread->start();
    }
    // The worker owns its event loop, sockets, settings reads and serial
    // enumeration. Only the completed snapshot crosses back to the GUI.
    QMetaObject::invokeMethod(m_worker, [targets, cancel, result, generation, owner = m_owner] {
        run(targets, cancel, result);
        // Unless the checker is being destroyed (it waits for this lock).
        QMutexLocker locker(&owner->mutex);
        if (auto *checker = owner->checker)
            QMetaObject::invokeMethod(checker, [checker, generation, result] {
                checker->finished(generation, *result);
            }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void HostStatusChecker::finished(int generation, const QHash<QString, Status> &result)
{
    m_running = false;
    m_cancel.reset();
    if (m_pendingTargets) {
        const auto pending = std::move(*m_pendingTargets);
        m_pendingTargets.reset();
        start(pending);
        return;
    }
    if (generation == m_generation)
        m_status = result;
    emit changed();
}

// On the worker thread.
void HostStatusChecker::run(const QList<Target> &targets, const std::shared_ptr<std::atomic_bool> &cancel,
                            const std::shared_ptr<QHash<QString, Status>> &result)
{
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
}

void HostStatusChecker::clear()
{
    ++m_generation;
    m_pendingTargets.reset();
    if (m_cancel)
        *m_cancel = true;
    // m_running stays true until the batch has reported back. That keeps a
    // timer tick or session reload from starting overlapping probe batches.
    m_status.clear();
    emit changed();
}

bool HostStatusChecker::shutdown(int timeoutMs)
{
    ++m_generation;
    m_pendingTargets.reset();
    {
        // From now on a finishing batch reports to nobody.
        QMutexLocker locker(&m_owner->mutex);
        m_owner->checker = nullptr;
    }
    if (!m_thread)
        return true;
    if (m_cancel)
        *m_cancel = true;
    // The worker's local QEventLoop sees the cancellation flag within 50 ms;
    // the thread's own loop then ends.
    m_thread->quit();
    const bool done = m_thread->wait(timeoutMs);
    if (!done) {
        // Never destroy a running thread: leave it (and its worker) behind.
        qWarning() << "Host-status check did not finish before shutdown deadline";
        m_thread->setParent(nullptr);
    }
    m_thread = nullptr;
    return done;
}
