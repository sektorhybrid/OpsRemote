// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QObject>
#include <QTimer>

#include <atomic>
#include <thread>

// For a session whose library runs on a thread of its own (FreeRDP,
// libvncclient), told to stop: nobody listens to it any more, and it
// deletes itself once that thread has returned (a library may take a moment
// to come back from a connection attempt) instead of making the GUI wait.
inline void deleteWhenThreadEnds(QObject *session, std::thread &thread, const std::atomic<bool> &finished)
{
    QObject::disconnect(session, nullptr, nullptr, nullptr);
    session->setParent(nullptr);
    auto *poll = new QTimer(session);
    QObject::connect(poll, &QTimer::timeout, session, [session, &thread, &finished] {
        if (!finished)
            return;
        if (thread.joinable())
            thread.join();
        session->deleteLater();
    });
    poll->start(100);
}
