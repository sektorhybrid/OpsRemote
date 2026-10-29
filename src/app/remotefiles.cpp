// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "remotefiles.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <utility>

namespace {
constexpr int MaximumTreeDepth = 64;
// Transfers plan every entry first; this bounds the memory a huge tree uses.
constexpr qsizetype MaximumPlannedEntries = 200000;
constexpr qint64 ProgressIntervalMs = 100;

bool isDigits(QStringView text)
{
    return !text.isEmpty()
        && std::all_of(text.begin(), text.end(), [](QChar c) { return c.isDigit(); });
}

int monthNumber(QStringView text)
{
    static const char *const months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                         "jul", "aug", "sep", "oct", "nov", "dec"};
    for (int index = 0; index < 12; ++index) {
        if (text.compare(QLatin1String(months[index]), Qt::CaseInsensitive) == 0)
            return index + 1;
    }
    return 0;
}
}

namespace RemotePaths {

QString join(const QString &directory, const QString &name)
{
    return directory.endsWith(QLatin1Char('/')) ? directory + name
                                                : directory + QLatin1Char('/') + name;
}

QString fileName(const QString &path)
{
    auto trimmed = path;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    return trimmed.mid(trimmed.lastIndexOf(QLatin1Char('/')) + 1);
}

QString parent(const QString &path)
{
    auto trimmed = path;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : trimmed.left(slash);
}

// No separators, drive letters, or control characters, and on Windows none
// of its reserved device names.
bool isSafeLocalName(const QString &name)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return false;
    for (const QChar c : name) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') || c.unicode() < 0x20)
            return false;
    }
#ifdef Q_OS_WIN
    // Windows drops trailing dots and spaces, and maps these names to devices.
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        return false;
    static const QRegularExpression reserved(
        QStringLiteral("^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch())
        return false;
    for (const QChar c : name) {
        if (QStringLiteral("<>\"|?*").contains(c))
            return false;
    }
#endif
    return true;
}

bool isInside(const QString &path, const QString &folder)
{
    const auto root = QDir::cleanPath(QDir(folder).absolutePath());
    const auto target = QDir::cleanPath(QDir(path).absolutePath());
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return target.startsWith(root + QLatin1Char('/'), sensitivity);
}

quint32 modeFromString(const QString &text)
{
    if (text.size() < 10)
        return 0;
    quint32 mode = 0;
    switch (text.at(0).toLatin1()) {
    case 'd': mode = 0040000; break;
    case 'l': mode = 0120000; break;
    case '-': mode = 0100000; break;
    default: break;
    }
    static constexpr quint32 bits[9] = {0400, 0200, 0100, 040, 020, 010, 04, 02, 01};
    for (int index = 0; index < 9; ++index) {
        const QChar c = text.at(index + 1);
        if (c != QLatin1Char('-') && c != QLatin1Char('S') && c != QLatin1Char('T'))
            mode |= bits[index];
        // setuid, setgid, sticky
        if (index == 2 && (c == QLatin1Char('s') || c == QLatin1Char('S')))
            mode |= 04000;
        if (index == 5 && (c == QLatin1Char('s') || c == QLatin1Char('S')))
            mode |= 02000;
        if (index == 8 && (c == QLatin1Char('t') || c == QLatin1Char('T')))
            mode |= 01000;
    }
    return mode;
}

bool parseLsLine(const QString &line, SftpEntry *entry)
{
    // Fields and where each starts, so the name keeps its own spaces.
    QList<QStringView> fields;
    QList<qsizetype> starts;
    const QStringView text(line);
    for (qsizetype index = 0; index < text.size() && fields.size() < 9;) {
        while (index < text.size() && text[index] == QLatin1Char(' '))
            ++index;
        if (index >= text.size())
            break;
        const qsizetype start = index;
        while (index < text.size() && text[index] != QLatin1Char(' '))
            ++index;
        fields.append(text.mid(start, index - start));
        starts.append(start);
    }
    if (fields.size() < 6 || fields[0].size() < 10
        || !QStringLiteral("-dlcbps").contains(fields[0][0]))
        return false;

    qsizetype nameStart = -1;
    QDateTime modified;
    quint64 size = 0;
    if (fields.size() >= 7 && isDigits(fields[4]) && isDigits(fields[5])) {
        // perms links owner group size seconds name (--time-style=+%s)
        size = fields[4].toULongLong();
        modified = QDateTime::fromSecsSinceEpoch(fields[5].toLongLong());
        nameStart = starts[5] + fields[5].size() + 1;
    } else {
        // perms links owner [group] size month day time-or-year name
        int month = 0;
        qsizetype at = 3;
        for (; at + 2 < fields.size(); ++at) {
            month = monthNumber(fields[at]);
            if (month && isDigits(fields[at + 1]))
                break;
        }
        if (!month || at + 2 >= fields.size())
            return false;
        size = fields[at - 1].toULongLong();
        const int day = fields[at + 1].toInt();
        const QStringView clock = fields[at + 2];
        const QDate today = QDate::currentDate();
        if (clock.contains(QLatin1Char(':'))) {
            // Within the last six months: this year, or last year if that
            // would be in the future.
            QDate date(today.year(), month, day);
            if (date > today.addDays(1))
                date = QDate(today.year() - 1, month, day);
            const QTime time = QTime::fromString(clock.toString(), QStringLiteral("H:mm"));
            modified = QDateTime(date, time.isValid() ? time : QTime(0, 0));
        } else {
            modified = QDateTime(QDate(clock.toInt(), month, day), QTime(0, 0));
        }
        nameStart = starts[at + 2] + clock.size() + 1;
    }
    if (nameStart <= 0 || nameStart >= line.size())
        return false;
    QString name = line.mid(nameStart);
    const QChar type = fields[0][0];
    if (type == QLatin1Char('l')) {
        const int arrow = name.indexOf(QStringLiteral(" -> "));
        if (arrow > 0)
            name.truncate(arrow);
    }
    entry->name = name;
    entry->isDirectory = type == QLatin1Char('d');
    entry->isSymlink = type == QLatin1Char('l');
    entry->isOther = !entry->isDirectory && !entry->isSymlink && type != QLatin1Char('-');
    entry->size = size;
    entry->modified = modified;
    entry->permissions = modeFromString(fields[0].toString());
    return true;
}

}

using namespace RemotePaths;

bool RemoteFileBackend::removeTree(const QString &path, QString *error)
{
    std::function<bool(const QString &, int)> remove = [&](const QString &target, int depth) {
        const auto info = stat(target, false, error);
        if (!info)
            return false;
        if (!info->isDirectory)
            return removeFile(target, error);
        if (depth > MaximumTreeDepth) {
            *error = QStringLiteral("Could not delete %1: folders are nested too deeply.").arg(target);
            return false;
        }
        QList<SftpEntry> children;
        if (!list(target, false, &children, error))
            return false;
        for (const auto &child : std::as_const(children)) {
            if (!remove(join(target, child.name), depth + 1))
                return false;
        }
        return removeDirectory(target, error);
    };
    return remove(path, 0);
}

RemoteFileEngine::RemoteFileEngine(QObject *parent)
    : QObject(parent)
{
}

RemoteFileEngine::~RemoteFileEngine()
{
    reset();
}

void RemoteFileEngine::setBackend(std::unique_ptr<RemoteFileBackend> backend)
{
    reset();
    m_backend = std::move(backend);
}

void RemoteFileEngine::reset()
{
    closeTransferFiles(false);
    m_job.reset();
    m_jobQueue.clear();
    m_tasks.clear();
    m_awaitingConflict = false;
    m_backend.reset();
}

void RemoteFileEngine::listDirectory(const QString &path)
{
    if (!m_backend) {
        emit directoryListed(path, {}, m_unavailable);
        return;
    }
    QString error;
    const QString canonical = m_backend->canonicalPath(path, &error);
    if (canonical.isEmpty()) {
        emit directoryListed(path, {}, error);
        m_backend->idle();
        return;
    }
    QList<SftpEntry> entries;
    const bool listed = m_backend->list(canonical, true, &entries, &error);
    emit directoryListed(canonical, listed ? entries : QList<SftpEntry>(), listed ? QString() : error);
    m_backend->idle();
}

void RemoteFileEngine::makeDirectory(const QString &path)
{
    QString error;
    if (!m_backend)
        error = m_unavailable;
    else
        m_backend->makeDirectory(path, &error);
    emit fileOperationFinished(error);
    if (m_backend)
        m_backend->idle();
}

void RemoteFileEngine::renamePath(const QString &from, const QString &to)
{
    QString error;
    if (!m_backend)
        error = m_unavailable;
    else
        m_backend->rename(from, to, &error);
    emit fileOperationFinished(error);
    if (m_backend)
        m_backend->idle();
}

void RemoteFileEngine::removePaths(const QStringList &paths)
{
    QString error;
    if (!m_backend)
        error = m_unavailable;
    for (const auto &path : paths) {
        if (!error.isEmpty() || !m_backend->removeTree(path, &error))
            break;
    }
    emit fileOperationFinished(error);
    if (m_backend)
        m_backend->idle();
}

void RemoteFileEngine::scanTree(const QString &path)
{
    if (!m_backend) {
        emit treeScanned(path, {}, m_unavailable);
        return;
    }
    // A limit on size and depth; links to folders are not followed (loops).
    constexpr int MaximumEntries = 200000;
    constexpr int MaximumDepth = 64;
    QList<SftpTreeEntry> entries;
    QString error;
    std::function<void(const QString &, const QString &, int)> walk =
        [&](const QString &absolute, const QString &relative, int depth) {
            if (!error.isEmpty())
                return;
            QList<SftpEntry> children;
            if (!m_backend->list(absolute, false, &children, &error))
                return;
            QList<QPair<QString, QString>> folders;
            for (const auto &child : std::as_const(children)) {
                if (child.isSymlink)
                    continue;
                if (child.isOther)
                    continue;
                SftpTreeEntry entry;
                entry.path = relative.isEmpty() ? child.name : relative + QLatin1Char('/') + child.name;
                entry.isDirectory = child.isDirectory;
                entry.size = child.size;
                entry.modified = child.modified;
                if (!entry.isDirectory && !m_backend->exactListingTimes()) {
                    QString ignored;
                    if (const auto info = m_backend->stat(join(absolute, child.name), true, &ignored))
                        entry.modified = info->modified;
                }
                entries.append(entry);
                if (entry.isDirectory)
                    folders.append({join(absolute, child.name), entry.path});
                if (entries.size() > MaximumEntries) {
                    error = QStringLiteral("More than %1 files; choose a smaller folder.").arg(MaximumEntries);
                    return;
                }
            }
            m_backend->idle();
            if (depth < MaximumDepth) {
                for (const auto &[absoluteChild, relativeChild] : std::as_const(folders))
                    walk(absoluteChild, relativeChild, depth + 1);
            }
        };
    walk(path, QString(), 0);
    emit treeScanned(path, error.isEmpty() ? entries : QList<SftpTreeEntry>(), error);
}

void RemoteFileEngine::startTransfer(int jobId, bool upload, const QStringList &sources,
                                     const QString &destination,
                                     std::optional<SftpConflictResolution> policy)
{
    if (!m_backend) {
        emit transferFinished(jobId, upload, destination, 0, 0, {m_unavailable}, false);
        return;
    }
    m_jobQueue.append({jobId, upload, sources, destination, policy});
    startNextJob();
}

void RemoteFileEngine::startNextJob()
{
    if (m_job || m_jobQueue.isEmpty() || !m_backend)
        return;

    m_job = m_jobQueue.takeFirst();
    m_tasks.clear();
    m_taskIndex = 0;
    m_jobBytesTotal = 0;
    m_jobBytesDone = 0;
    m_jobFiles = 0;
    m_filesDone = 0;
    m_filesTransferred = 0;
    m_skipped = 0;
    m_transferErrors.clear();
    m_conflictPolicy = m_job->policy;
    m_taskResolution.reset();
    m_cancelRequested = false;

    QString error;
    const bool planned = m_job->upload ? planUpload(*m_job, &error) : planDownload(*m_job, &error);
    if (!planned)
        m_transferErrors.append(error);
    for (const auto &task : std::as_const(m_tasks)) {
        if (!task.isDirectory) {
            ++m_jobFiles;
            m_jobBytesTotal += task.size;
        }
    }
    m_progressTimer.invalidate();
    emitProgress(true);
    scheduleTransferStep();
}

bool RemoteFileEngine::planUpload(const TransferJob &job, QString *error)
{
    for (const auto &source : job.sources) {
        const QFileInfo info(source);
        if (!info.exists()) {
            *error = QStringLiteral("%1 no longer exists.").arg(source);
            return false;
        }
        const auto destination = join(job.destination, info.fileName());
        if (info.isFile()) {
            m_tasks.append({false, info.absoluteFilePath(), destination,
                            static_cast<quint64>(info.size()), info.lastModified(),
                            info.isExecutable()});
            continue;
        }
        if (!info.isDir())
            continue;

        m_tasks.append({true, info.absoluteFilePath(), destination});
        const QDir root(info.absoluteFilePath());
        QDirIterator iterator(info.absoluteFilePath(),
                              QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                  | QDir::System,
                              QDirIterator::Subdirectories);
        // A folder is always listed before its contents, so remote parents
        // are created first.
        while (iterator.hasNext()) {
            iterator.next();
            const auto child = iterator.fileInfo();
            const auto remote = join(destination, root.relativeFilePath(child.absoluteFilePath()));
            if (child.isSymLink() && child.isDir())
                continue;
            if (m_tasks.size() >= MaximumPlannedEntries) {
                *error = QStringLiteral("%1 has more than %2 entries; upload smaller parts of it.")
                             .arg(source).arg(MaximumPlannedEntries);
                return false;
            }
            if (child.isDir())
                m_tasks.append({true, child.absoluteFilePath(), remote});
            else if (child.isFile())
                m_tasks.append({false, child.absoluteFilePath(), remote,
                                static_cast<quint64>(child.size()), child.lastModified(),
                                child.isExecutable()});
        }
    }
    return true;
}

bool RemoteFileEngine::planDownload(const TransferJob &job, QString *error)
{
    for (const auto &source : job.sources) {
        const auto name = fileName(source);
        const auto target = QDir(job.destination).filePath(name);
        if (!isSafeLocalName(name) || !isInside(target, job.destination)) {
            *error = QStringLiteral("%1 cannot be saved here: its name is not valid on this "
                                    "computer.").arg(source);
            return false;
        }
        if (!planRemoteTree(source, target, 0, error))
            return false;
    }
    return true;
}

bool RemoteFileEngine::planRemoteTree(const QString &remotePath, const QString &localPath,
                                      int depth, QString *error)
{
    const auto info = m_backend->stat(remotePath, true, error);
    if (!info)
        return false;
    TransferTask task{info->isDirectory, remotePath, localPath, info->size, info->modified,
                      (info->permissions & 0111) != 0};
    if (info->isRegular) {
        m_tasks.append(task);
        return true;
    }
    if (!info->isDirectory)
        return true;
    if (depth > MaximumTreeDepth) {
        *error = QStringLiteral("%1 is nested too deeply.").arg(remotePath);
        return false;
    }

    m_tasks.append(task);
    QList<SftpEntry> children;
    if (!m_backend->list(remotePath, false, &children, error))
        return false;
    for (const auto &child : std::as_const(children)) {
        const auto &name = child.name;
        // A server can return any entry name; one that would leave the
        // download folder (for example "..\\x" on Windows) is skipped.
        const auto target = QDir(localPath).filePath(name);
        if (!isSafeLocalName(name) || !isInside(target, localPath)) {
            m_transferErrors.append(QStringLiteral("Skipped %1: its name is not valid on this "
                                                   "computer.").arg(join(remotePath, name)));
            continue;
        }
        if (m_tasks.size() >= MaximumPlannedEntries) {
            *error = QStringLiteral("%1 has more than %2 entries; download smaller parts of it.")
                         .arg(remotePath).arg(MaximumPlannedEntries);
            return false;
        }
        // Entries the listing already describes need no further question,
        // except links, which are followed.
        const auto childPath = join(remotePath, name);
        if (child.isOther)
            continue;
        if (!child.isSymlink && !child.isDirectory && m_backend->exactListingTimes()) {
            m_tasks.append({false, childPath, target, child.size, child.modified,
                            (child.permissions & 0111) != 0});
            continue;
        }
        if (!planRemoteTree(childPath, target, depth + 1, error))
            return false;
    }
    return true;
}

void RemoteFileEngine::scheduleTransferStep()
{
    if (std::exchange(m_stepScheduled, true))
        return;
    // Each chunk returns to the event loop so other traffic (terminal
    // output, listings) keeps flowing while a transfer runs.
    QTimer::singleShot(0, this, [this] {
        m_stepScheduled = false;
        transferStep();
    });
}

void RemoteFileEngine::transferStep()
{
    if (!m_job || m_awaitingConflict || !m_backend)
        return;
    if (m_cancelRequested) {
        finishJob(true);
        return;
    }

    if (!m_remoteFileOpen) {
        if (m_taskIndex >= m_tasks.size()) {
            finishJob(false);
            return;
        }
        switch (beginTask()) {
        case TaskStart::Waiting:
            return;
        case TaskStart::Finished:
            ++m_taskIndex;
            scheduleTransferStep();
            return;
        case TaskStart::Opened:
            break;
        }
    }

    qint64 transferred = 0;
    QString error;
    if (m_job->upload) {
        const auto chunk = m_localFile->read(m_backend->writeChunk());
        if (chunk.isEmpty()) {
            if (m_localFile->atEnd())
                completeFile();
            else
                failFile(QStringLiteral("Could not read %1: %2")
                             .arg(m_tasks[m_taskIndex].source, m_localFile->errorString()));
            return;
        }
        if (!m_backend->write(chunk.constData(), chunk.size(), &error)) {
            failFile(error);
            return;
        }
        transferred = chunk.size();
    } else {
        const qint64 size = m_backend->readChunk();
        QByteArray chunk(static_cast<qsizetype>(size), Qt::Uninitialized);
        const auto count = m_backend->read(chunk.data(), size, &error);
        if (count < 0) {
            failFile(error);
            return;
        }
        if (count == 0) {
            completeFile();
            return;
        }
        if (m_localFile->write(chunk.constData(), count) != count) {
            failFile(QStringLiteral("Could not write %1: %2")
                         .arg(m_tasks[m_taskIndex].destination, m_localFile->errorString()));
            return;
        }
        transferred = count;
    }

    m_taskBytesDone += static_cast<quint64>(transferred);
    m_jobBytesDone += static_cast<quint64>(transferred);
    emitProgress(false);
    m_backend->idle();
    scheduleTransferStep();
}

RemoteFileEngine::TaskStart RemoteFileEngine::beginTask()
{
    auto &task = m_tasks[m_taskIndex];
    QString error;
    if (task.isDirectory) {
        if (m_job->upload) {
            if (!m_backend->makeDirectory(task.destination, &error)) {
                // An existing folder is merged into.
                QString ignored;
                const auto existing = m_backend->stat(task.destination, true, &ignored);
                if (!existing || !existing->isDirectory)
                    m_transferErrors.append(error);
            }
        } else if (!QDir().mkpath(task.destination)) {
            m_transferErrors.append(QStringLiteral("Could not create folder %1.")
                                        .arg(task.destination));
        }
        return TaskStart::Finished;
    }

    quint64 existingSize = 0;
    QDateTime existingModified;
    if (!m_taskResolution && destinationExists(task.destination, &existingSize, &existingModified)) {
        if (!m_conflictPolicy) {
            m_awaitingConflict = true;
            emit transferConflict(task.destination, existingSize, existingModified, task.size,
                                  task.modified, m_job->upload);
            return TaskStart::Waiting;
        }
        m_taskResolution = m_conflictPolicy;
    }
    if (const auto resolution = std::exchange(m_taskResolution, std::nullopt)) {
        if (*resolution == SftpConflictResolution::Skip) {
            ++m_skipped;
            ++m_filesDone;
            m_jobBytesDone += task.size;
            emitProgress(true);
            return TaskStart::Finished;
        }
        if (*resolution == SftpConflictResolution::Rename)
            task.destination = uniqueDestination(task.destination);
    }

    m_taskBytesDone = 0;
    emitProgress(true);
    if (m_job->upload) {
        auto file = std::make_unique<QFile>(task.source);
        if (!file->open(QIODevice::ReadOnly)) {
            failFile(QStringLiteral("Could not read %1: %2").arg(task.source, file->errorString()));
            return TaskStart::Waiting;
        }
        m_localFile = std::move(file);
        // Big files go to a temporary name first, so an interrupted upload
        // never leaves a half-written file in place of a complete one.
        const bool temporary = m_temporaryMinimum >= 0 && qint64(task.size) >= m_temporaryMinimum;
        m_writtenPath = temporary ? task.destination + QStringLiteral(".filepart") : task.destination;
        if (!m_backend->openWrite(m_writtenPath, task.size, task.modified, task.executable,
                                  &error)) {
            failFile(error);
            return TaskStart::Waiting;
        }
        m_remoteFileOpen = true;
        m_remoteFileWritten = true;
    } else {
        if (!m_backend->openRead(task.source, &error)) {
            failFile(error);
            return TaskStart::Waiting;
        }
        m_remoteFileOpen = true;
        // QSaveFile keeps an existing local file intact until the download
        // completes.
        auto file = std::make_unique<QSaveFile>(task.destination);
        if (!file->open(QIODevice::WriteOnly)) {
            failFile(QStringLiteral("Could not write %1: %2")
                         .arg(task.destination, file->errorString()));
            return TaskStart::Waiting;
        }
        m_localFile = std::move(file);
    }
    return TaskStart::Opened;
}

bool RemoteFileEngine::destinationExists(const QString &path, quint64 *size, QDateTime *modified)
{
    if (!m_job->upload) {
        const QFileInfo info(path);
        if (!info.exists())
            return false;
        *size = static_cast<quint64>(info.size());
        *modified = info.lastModified();
        return true;
    }
    QString error;
    const auto info = m_backend->stat(path, true, &error);
    if (!info)
        return false;
    *size = info->size;
    *modified = info->modified;
    return true;
}

// Returns "name (n).ext" for the first n that does not exist yet.
QString RemoteFileEngine::uniqueDestination(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const auto directory = path.left(slash + 1);
    const auto name = path.mid(slash + 1);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    const auto stem = dot > 0 ? name.left(dot) : name;
    const auto suffix = dot > 0 ? name.mid(dot) : QString();
    quint64 size = 0;
    QDateTime modified;
    for (int counter = 1;; ++counter) {
        const auto candidate = QStringLiteral("%1%2 (%3)%4").arg(directory, stem).arg(counter)
                                   .arg(suffix);
        if (!destinationExists(candidate, &size, &modified))
            return candidate;
    }
}

void RemoteFileEngine::completeFile()
{
    auto &task = m_tasks[m_taskIndex];
    const QString written = m_writtenPath;
    if (!closeTransferFiles(true)) {
        failFile(QStringLiteral("Could not save %1.").arg(task.destination));
        return;
    }
    if (m_job->upload && written != task.destination) {
        QString error;
        if (!m_backend->replace(written, task.destination, &error)) {
            QString ignored;
            m_backend->removeFile(written, &ignored);
            m_transferErrors.append(error);
            ++m_filesDone;
            ++m_taskIndex;
            emitProgress(true);
            scheduleTransferStep();
            return;
        }
    }
    if (task.modified.isValid()) {
        if (m_job->upload) {
            m_backend->setModified(task.destination, task.modified);
        } else {
            QFile file(task.destination);
            if (file.open(QIODevice::Append))
                file.setFileTime(task.modified, QFileDevice::FileModificationTime);
            if (task.executable)
                file.setPermissions(file.permissions() | QFileDevice::ExeOwner
                                    | QFileDevice::ExeGroup | QFileDevice::ExeOther);
        }
    }
    ++m_filesTransferred;
    ++m_filesDone;
    ++m_taskIndex;
    emitProgress(true);
    m_backend->idle();
    scheduleTransferStep();
}

void RemoteFileEngine::failFile(const QString &error)
{
    closeTransferFiles(false);
    const auto &task = m_tasks[m_taskIndex];
    m_jobBytesDone += task.size > m_taskBytesDone ? task.size - m_taskBytesDone : 0;
    m_transferErrors.append(error);
    ++m_filesDone;
    ++m_taskIndex;
    emitProgress(true);
    scheduleTransferStep();
}

// Closes the current file pair. On failure or cancellation the partial
// destination file is removed.
bool RemoteFileEngine::closeTransferFiles(bool success)
{
    if (!m_remoteFileOpen && !m_localFile)
        return true;
    bool closed = true;
    if (std::exchange(m_remoteFileOpen, false) && m_backend) {
        QString error;
        closed = m_backend->closeFile(success, &error);
    }
    if (m_localFile) {
        if (auto *saveFile = qobject_cast<QSaveFile *>(m_localFile.get())) {
            if (success && closed)
                closed = saveFile->commit();
            else
                saveFile->cancelWriting();
        }
        m_localFile.reset();
    }
    if ((!success || !closed) && std::exchange(m_remoteFileWritten, false) && m_backend
        && !m_writtenPath.isEmpty()) {
        QString ignored;
        m_backend->removeFile(m_writtenPath, &ignored);
    }
    m_remoteFileWritten = false;
    return closed;
}

void RemoteFileEngine::finishJob(bool cancelled)
{
    closeTransferFiles(false);
    const auto job = *std::exchange(m_job, std::nullopt);
    m_tasks.clear();
    m_awaitingConflict = false;
    emit transferFinished(job.id, job.upload, job.destination, m_filesTransferred, m_skipped,
                          m_transferErrors, cancelled);
    if (cancelled) {
        for (const auto &queued : std::as_const(m_jobQueue))
            emit transferFinished(queued.id, queued.upload, queued.destination, 0, 0, {}, true);
        m_jobQueue.clear();
    }
    m_cancelRequested = false;
    if (m_backend)
        m_backend->idle();
    startNextJob();
}

void RemoteFileEngine::emitProgress(bool force)
{
    if (!m_job)
        return;
    if (!force && m_progressTimer.isValid() && m_progressTimer.elapsed() < ProgressIntervalMs)
        return;
    m_progressTimer.start();
    const auto current = m_taskIndex < m_tasks.size()
        ? QFileInfo(m_tasks[m_taskIndex].destination).fileName()
        : QString();
    emit transferProgress(m_job->id, current, m_jobBytesDone, m_jobBytesTotal, m_filesDone,
                          m_jobFiles, static_cast<int>(m_jobQueue.size()));
}

void RemoteFileEngine::resolveConflict(SftpConflictResolution resolution, bool applyToAll)
{
    if (!std::exchange(m_awaitingConflict, false))
        return;
    if (resolution == SftpConflictResolution::Cancel) {
        m_cancelRequested = true;
    } else {
        if (applyToAll)
            m_conflictPolicy = resolution;
        m_taskResolution = resolution;
    }
    scheduleTransferStep();
}

void RemoteFileEngine::cancelTransfers()
{
    if (!m_job)
        return;
    m_cancelRequested = true;
    m_awaitingConflict = false;
    scheduleTransferStep();
}

void RemoteFileSession::setTemporaryUploads(qint64 minimumBytes)
{
    if (m_engine)
        QMetaObject::invokeMethod(m_engine, [engine = m_engine, minimumBytes] {
            engine->setTemporaryUploads(minimumBytes);
        }, Qt::QueuedConnection);
}

void RemoteFileSession::relay(RemoteFileEngine *engine)
{
    m_engine = engine;
    connect(engine, &RemoteFileEngine::directoryListed, this, &RemoteFileSession::directoryListed);
    connect(engine, &RemoteFileEngine::fileOperationFinished,
            this, &RemoteFileSession::fileOperationFinished);
    connect(engine, &RemoteFileEngine::treeScanned, this, &RemoteFileSession::treeScanned);
    connect(engine, &RemoteFileEngine::transferProgress, this, &RemoteFileSession::transferProgress);
    connect(engine, &RemoteFileEngine::transferConflict, this, &RemoteFileSession::transferConflict);
    connect(engine, &RemoteFileEngine::transferFinished, this, &RemoteFileSession::transferFinished);
}
