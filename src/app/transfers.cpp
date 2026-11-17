// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "transfers.h"

#include <QCoreApplication>
#include <QDir>

#include <algorithm>

namespace {
constexpr int MaxEnded = 50; // finished transfers kept in the list
}

TransferList *TransferList::instance()
{
    static auto *list = new TransferList(QCoreApplication::instance());
    return list;
}

int TransferList::start(QWidget *owner, const QString &name, bool upload, std::function<void()> cancel,
                        const QString &cancelHint)
{
    Transfer transfer;
    transfer.id = m_nextId++;
    transfer.owner = owner;
    transfer.name = name;
    transfer.upload = upload;
    transfer.cancel = std::move(cancel);
    transfer.cancelHint = cancelHint;
    m_transfers.prepend(transfer);
    // A closed tab or window cannot report any more: its transfers end.
    if (owner) {
        connect(owner, &QObject::destroyed, this, [this, id = transfer.id] { finish(id, State::Cancelled); });
    }
    emit changed();
    return transfer.id;
}

TransferList::Transfer *TransferList::find(int id)
{
    for (auto &transfer : m_transfers) {
        if (transfer.id == id)
            return &transfer;
    }
    return nullptr;
}

void TransferList::update(int id, const QString &name, quint64 done, quint64 total, const QString &detail)
{
    auto *transfer = find(id);
    if (!transfer || transfer->state != State::Running)
        return;
    if (!name.isEmpty())
        transfer->name = name;
    transfer->done = done;
    transfer->total = total;
    transfer->detail = detail;
    emit changed();
}

void TransferList::finish(int id, State state)
{
    auto *transfer = find(id);
    if (!transfer || transfer->state != State::Running)
        return;
    transfer->state = state;
    if (state == State::Done && transfer->total > 0)
        transfer->done = transfer->total;
    transfer->cancel = {};
    transfer->pause = {};
    transfer->paused = false;
    transfer->ended = QDateTime::currentDateTime();
    int ended = 0;
    for (int index = 0; index < m_transfers.size(); ++index) {
        if (m_transfers[index].state != State::Running && ++ended > MaxEnded)
            m_transfers.removeAt(index--);
    }
    emit changed();
}

void TransferList::cancel(int id)
{
    auto *transfer = find(id);
    if (!transfer || transfer->state != State::Running || !transfer->cancel)
        return;
    // The owner reports the end (cancelled) as it would after its own Cancel.
    const auto cancelIt = transfer->cancel;
    cancelIt();
}

void TransferList::setPause(int id, std::function<void(bool)> pause)
{
    if (auto *transfer = find(id)) {
        transfer->pause = std::move(pause);
        emit changed();
    }
}

void TransferList::setRetry(int id, std::function<void()> retry)
{
    if (auto *transfer = find(id)) {
        transfer->retry = std::move(retry);
        emit changed();
    }
}

void TransferList::pause(int id, bool paused)
{
    auto *transfer = find(id);
    if (!transfer || transfer->state != State::Running || !transfer->pause || transfer->paused == paused)
        return;
    transfer->paused = paused;
    const auto pauseIt = transfer->pause;
    emit changed();
    pauseIt(paused);
}

void TransferList::retry(int id)
{
    auto *transfer = find(id);
    if (!transfer || (transfer->state != State::Cancelled && transfer->state != State::Failed) || !transfer->retry)
        return;
    const auto retryIt = transfer->retry;
    m_transfers.removeIf([id](const Transfer &other) { return other.id == id; });
    emit changed();
    retryIt();
}

void TransferList::clearEnded()
{
    m_transfers.removeIf([](const Transfer &transfer) { return transfer.state != State::Running; });
    emit changed();
}

void TransferList::setTargets(int id, const QStringList &targets)
{
    if (auto *transfer = find(id))
        transfer->targets = targets;
}

void TransferList::remove(int id)
{
    if (m_transfers.removeIf([id](const Transfer &transfer) { return transfer.id == id; }) > 0)
        emit changed();
}

bool TransferList::isRunning(int id) const
{
    for (const auto &transfer : m_transfers) {
        if (transfer.id == id)
            return transfer.state == State::Running;
    }
    return false;
}

QString TransferList::target(const QString &server, const QString &path)
{
    if (!server.isEmpty())
        return server + QLatin1Char('|') + path;
    QString local = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    local = local.toLower(); // the same file, however it is written
#endif
    return QStringLiteral("local|") + local;
}

QList<TransferList::Transfer> TransferList::writingTo(const QStringList &targets) const
{
    // The same file, or one inside the other ("a/b" and "a/b/c").
    const auto overlap = [](const QString &one, const QString &other) {
        const auto &shorter = one.size() <= other.size() ? one : other;
        const auto &longer = one.size() <= other.size() ? other : one;
        return longer.startsWith(shorter)
               && (longer.size() == shorter.size() || longer.at(shorter.size()) == QLatin1Char('/')
                   || shorter.endsWith(QLatin1Char('/')));
    };
    QList<Transfer> result;
    for (const auto &transfer : m_transfers) {
        if (transfer.state != State::Running)
            continue;
        const bool writes = std::any_of(transfer.targets.cbegin(), transfer.targets.cend(), [&](const QString &mine) {
            return std::any_of(targets.cbegin(), targets.cend(),
                               [&](const QString &other) { return overlap(mine, other); });
        });
        if (writes)
            result.append(transfer);
    }
    return result;
}

QList<TransferList::Transfer> TransferList::running() const
{
    QList<Transfer> result;
    for (const auto &transfer : m_transfers) {
        if (transfer.state == State::Running)
            result.append(transfer);
    }
    return result;
}

int TransferList::runningPercent() const
{
    quint64 done = 0, total = 0;
    for (const auto &transfer : m_transfers) {
        if (transfer.state == State::Running) {
            done += transfer.done;
            total += transfer.total;
        }
    }
    return total > 0 ? int(done * 100 / total) : 0;
}
