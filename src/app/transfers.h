// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QWidget>

#include <functional>

// Every file transfer of the application, for the Transfers indicator, its
// list and the tab strips: SFTP/SCP/FTP (the Files panel, file sessions,
// files dropped on a terminal), remote-desktop clipboard copies, and Network
// Tools downloads. Each belongs to an owner widget (the tab's page, or a
// tool window); when the owner goes, its transfers end. GUI thread only.
class TransferList final : public QObject
{
    Q_OBJECT
public:
    enum class State { Running, Done, Failed, Cancelled };
    struct Transfer
    {
        int id = 0;
        QPointer<QWidget> owner;
        QString name;   // the file (or what) being transferred
        bool upload = true;
        quint64 done = 0;
        quint64 total = 0;
        QString detail; // "3.1 MB of 12 MB · 1.4 MB/s"
        State state = State::Running;
        std::function<void()> cancel; // empty: cannot be cancelled
        QString cancelHint;           // what cancelling stops, for the ✕'s tooltip
        std::function<void(bool)> pause; // pauses (true) or continues; empty: cannot pause
        bool paused = false;
        std::function<void()> retry; // starts it again from the beginning once ended; empty: cannot
        // What it writes, each "server|path" ("local|path" for a file here),
        // to find two transfers writing the same file (see writingTo()).
        QStringList targets;
        QDateTime ended;
        int percent() const { return total > 0 ? int(done * 100 / total) : 0; }
    };

    static TransferList *instance();

    int start(QWidget *owner, const QString &name, bool upload, std::function<void()> cancel = {},
              const QString &cancelHint = {});
    void update(int id, const QString &name, quint64 done, quint64 total, const QString &detail = {});
    void finish(int id, State state);
    void cancel(int id);
    // What a transfer can do besides cancelling (see Transfer).
    void setPause(int id, std::function<void(bool)> pause);
    void setRetry(int id, std::function<void()> retry);
    void pause(int id, bool paused);
    // A cancelled or failed transfer, from the beginning: it leaves the list,
    // its new run comes as a new transfer.
    void retry(int id);
    void clearEnded();
    void setTargets(int id, const QStringList &targets);
    // Takes a transfer out of the list without it ending (one that waited
    // and now starts as a transfer of its own).
    void remove(int id);
    bool isRunning(int id) const;
    // Running transfers writing one of targets, or a file in one of them,
    // or a folder one of them is in.
    QList<Transfer> writingTo(const QStringList &targets) const;
    // A target: "server|path", or "local|path" for a file here (server empty).
    static QString target(const QString &server, const QString &path);
    // The name of an owner (its tab), for questions about its transfers.
    static inline std::function<QString(QWidget *)> ownerName;

    const QList<Transfer> &transfers() const { return m_transfers; }
    QList<Transfer> running() const;
    // All running transfers together, 0–100 (by bytes, where known).
    int runningPercent() const;

signals:
    void changed();

private:
    using QObject::QObject;
    Transfer *find(int id);
    QList<Transfer> m_transfers; // newest first
    int m_nextId = 1;
};
