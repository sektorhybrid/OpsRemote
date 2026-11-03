// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "workerthreads.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <memory>

namespace {

constexpr int MaximumIdle = 8; // more idle than this end after all

struct Pool
{
    QMutex mutex;
    // Every thread still running, and whether its session asked it to stop.
    QHash<QThread *, std::shared_ptr<std::atomic_bool>> threads;
    QList<QThread *> idle;
    bool shuttingDown = false;
};

// Never destroyed: threads may park while static objects are being torn down.
Pool &pool()
{
    static auto *instance = new Pool;
    return *instance;
}

}

namespace WorkerThreads {

QThread *acquire(const QString &name)
{
    auto &all = pool();
    QMutexLocker locker(&all.mutex);
    QThread *thread = nullptr;
    if (!all.idle.isEmpty()) {
        thread = all.idle.takeLast();
        *all.threads.value(thread) = false;
    } else {
        thread = new QThread;
        all.threads.insert(thread, std::make_shared<std::atomic_bool>(false));
        QObject::connect(thread, &QThread::finished, thread, [thread] {
            auto &all = pool();
            QMutexLocker locker(&all.mutex);
            all.threads.remove(thread);
            all.idle.removeAll(thread);
        }, Qt::DirectConnection);
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }
    thread->setObjectName(name);
    return thread;
}

void release(QObject *worker)
{
    if (!worker)
        return;
    QThread *thread = worker->thread();
    {
        auto &all = pool();
        QMutexLocker locker(&all.mutex);
        if (const auto stop = all.threads.value(thread))
            *stop = true;
    }
    QMetaObject::invokeMethod(worker, [worker, thread] {
        delete worker;
        park(thread);
    }, Qt::QueuedConnection);
}

void park(QThread *thread)
{
    auto &all = pool();
    QMutexLocker locker(&all.mutex);
    if (!all.threads.contains(thread))
        return;
    if (all.shuttingDown || all.idle.size() >= MaximumIdle) {
        thread->quit();
        return;
    }
    thread->setObjectName(QStringLiteral("idle worker"));
    all.idle.append(thread);
}

bool stopRequested()
{
    auto &all = pool();
    QMutexLocker locker(&all.mutex);
    const auto stop = all.threads.value(QThread::currentThread());
    return stop && *stop;
}

bool shutdownAll(int timeoutMs)
{
    auto &all = pool();
    QList<QThread *> threads;
    {
        QMutexLocker locker(&all.mutex);
        all.shuttingDown = true;
        threads = all.threads.keys();
        for (const auto &stop : std::as_const(all.threads))
            *stop = true;
    }
    // Ending a thread's event loop deletes its worker there (each session
    // connects finished to its worker's deleteLater), which disconnects.
    for (auto *thread : std::as_const(threads))
        thread->quit();
    const QDeadlineTimer deadline(timeoutMs);
    bool finished = true;
    for (auto *thread : std::as_const(threads))
        finished = thread->wait(deadline) && finished;
    return finished;
}

}
