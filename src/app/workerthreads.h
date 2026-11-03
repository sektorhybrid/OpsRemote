// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

class QObject;
class QString;
class QThread;

// Threads for session workers (SSH, FTP, iperf), kept and reused instead of
// ended: on Windows every QThread that ends pauses the whole program for a
// second or more (Qt's thread teardown; plain Windows threads do not), which
// made the window stall each time a session closed or reconnected. A thread
// ends only at shutdownAll(), or when more than a few are idle.
namespace WorkerThreads {

// A running thread to move a worker to (GUI thread only).
QThread *acquire(const QString &name);

// Deletes the worker in its own thread, once the call it may be in returns,
// then keeps the thread for the next acquire(). The worker must have been
// moved to a thread from acquire(). Calls queued to it before still run;
// stopRequested() tells them to give up.
void release(QObject *worker);

// The thread is no longer used (its worker is gone already): keep it.
void park(QThread *thread);

// In a worker: its session was closed, stop waiting.
bool stopRequested();

// Ends every thread, waiting at most this long; false if some did not end.
bool shutdownAll(int timeoutMs);

}
