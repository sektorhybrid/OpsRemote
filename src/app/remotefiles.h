// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

class QFileDevice;

struct SftpEntry
{
    QString name;
    bool isDirectory = false;
    bool isSymlink = false;
    // A device, pipe, or socket: not transferred.
    bool isOther = false;
    quint64 size = 0;
    QDateTime modified;
    quint32 permissions = 0;
};

enum class SftpConflictResolution { Overwrite, Skip, Rename, Cancel };

// One entry of a remote folder tree (RemoteFileSession::scanTree), relative
// to it.
struct SftpTreeEntry
{
    QString path; // with '/' separators
    bool isDirectory = false;
    quint64 size = 0;
    QDateTime modified;
};

namespace RemotePaths {
QString join(const QString &directory, const QString &name);
QString fileName(const QString &path);
QString parent(const QString &path);
// Whether a remote entry name can be used as one local file name without
// leaving the download folder.
bool isSafeLocalName(const QString &name);
// Whether path lies inside folder (after resolving "." and "..").
bool isInside(const QString &path, const QString &folder);
// "rwxr-xr-x" style permissions (with the type letter first) as mode bits.
quint32 modeFromString(const QString &text);
// Parses one line of "ls -l" output (also FTP LIST output in that format),
// with the usual date columns or seconds since 1970 (--time-style=+%s).
// The name keeps everything after the time; a link's " -> target" is
// removed.
bool parseLsLine(const QString &line, SftpEntry *entry);
}

// What a file protocol (SFTP, SCP, FTP) offers the transfer engine. Called
// on the engine's thread only, and blocking: each call returns once the
// server answered. Paths are absolute and use '/'.
class RemoteFileBackend
{
public:
    struct Info
    {
        bool isDirectory = false;
        bool isRegular = false;
        bool isSymlink = false;
        quint64 size = 0;
        QDateTime modified;
        quint32 permissions = 0;
    };

    virtual ~RemoteFileBackend() = default;

    // The folder logged on to.
    virtual QString home() = 0;
    // The path with links and "." and ".." resolved; empty on error.
    virtual QString canonicalPath(const QString &path, QString *error) = 0;
    // A folder's entries without "." and "..", links to folders shown as
    // folders when resolveLinks.
    virtual bool list(const QString &path, bool resolveLinks, QList<SftpEntry> *entries,
                      QString *error) = 0;
    // Without followLinks, a link is reported as a link (isSymlink).
    virtual std::optional<Info> stat(const QString &path, bool followLinks, QString *error) = 0;
    virtual bool makeDirectory(const QString &path, QString *error) = 0;
    virtual bool rename(const QString &from, const QString &to, QString *error) = 0;
    virtual bool removeFile(const QString &path, QString *error) = 0;
    virtual bool removeDirectory(const QString &path, QString *error) = 0; // an empty one
    // Renames from to to, replacing an existing to: as one step where the
    // server can, otherwise by deleting to just before.
    virtual bool replace(const QString &from, const QString &to, QString *error) = 0;
    // A file or a folder with everything in it.
    virtual bool removeTree(const QString &path, QString *error);

    // One file is open at a time, for reading or for writing.
    virtual bool openRead(const QString &path, QString *error) = 0;
    // Bytes read, 0 at the end, -1 on error.
    virtual qint64 read(char *data, qint64 size, QString *error) = 0;
    virtual bool openWrite(const QString &path, quint64 size, const QDateTime &modified,
                           bool executable, QString *error) = 0;
    virtual bool write(const char *data, qint64 size, QString *error) = 0;
    // Closes the open file; with success false, the transfer is abandoned.
    virtual bool closeFile(bool success, QString *error) = 0;
    // After an upload: gives the file this modification time if the
    // protocol can.
    virtual void setModified(const QString &path, const QDateTime &modified)
    {
        Q_UNUSED(path);
        Q_UNUSED(modified);
    }
    // Whether list() gives modification times to the second (FTP LIST and
    // plain "ls -l" give minutes, or days for older files); otherwise
    // stat() is asked for files whose time matters.
    virtual bool exactListingTimes() const { return true; }
    virtual qint64 readChunk() const { return 32 * 1024; }
    virtual qint64 writeChunk() const { return 32 * 1024; }
    // Called between steps, so the connection can handle other traffic.
    virtual void idle() {}
};

// The remote file operations of a connection, on its worker thread: listing,
// folder operations, and a queue of transfers run one at a time in small
// steps through the event loop, with conflict questions and progress.
class RemoteFileEngine final : public QObject
{
    Q_OBJECT

public:
    explicit RemoteFileEngine(QObject *parent = nullptr);
    ~RemoteFileEngine() override;

    // Takes over the backend; nullptr (or reset()) ends every operation.
    void setBackend(std::unique_ptr<RemoteFileBackend> backend);
    RemoteFileBackend *backend() const { return m_backend.get(); }
    // Abandons the running transfer and the queue, without reporting them.
    void reset();
    // The message used while no backend is set.
    void setUnavailableMessage(const QString &message) { m_unavailable = message; }
    // Uploads go to "name.filepart" first and replace name only when
    // complete (as WinSCP does), for files of at least minimumBytes;
    // minimumBytes < 0: never.
    void setTemporaryUploads(qint64 minimumBytes) { m_temporaryMinimum = minimumBytes; }

    void listDirectory(const QString &path);
    void makeDirectory(const QString &path);
    void renamePath(const QString &from, const QString &to);
    void removePaths(const QStringList &paths);
    void scanTree(const QString &path);
    void startTransfer(int jobId, bool upload, const QStringList &sources,
                       const QString &destination,
                       std::optional<SftpConflictResolution> policy);
    void resolveConflict(SftpConflictResolution resolution, bool applyToAll);
    void cancelTransfers();

signals:
    void directoryListed(const QString &path, const QList<SftpEntry> &entries,
                         const QString &error);
    void fileOperationFinished(const QString &error);
    void treeScanned(const QString &path, const QList<SftpTreeEntry> &entries, const QString &error);
    void transferProgress(int jobId, const QString &currentFile, quint64 bytesDone,
                          quint64 bytesTotal, int filesDone, int filesTotal, int queuedJobs);
    void transferConflict(const QString &path, quint64 existingSize,
                          const QDateTime &existingModified, quint64 newSize,
                          const QDateTime &newModified, bool upload);
    void transferFinished(int jobId, bool upload, const QString &destination,
                          int filesTransferred, int skipped, const QStringList &errors,
                          bool cancelled);

private:
    enum class TaskStart { Opened, Finished, Waiting };

    struct TransferTask
    {
        bool isDirectory = false;
        QString source;
        QString destination;
        quint64 size = 0;
        QDateTime modified;
        bool executable = false;
    };

    struct TransferJob
    {
        int id = 0;
        bool upload = true;
        QStringList sources;
        QString destination;
        std::optional<SftpConflictResolution> policy;
    };

    bool planUpload(const TransferJob &job, QString *error);
    bool planDownload(const TransferJob &job, QString *error);
    bool planRemoteTree(const QString &remotePath, const QString &localPath, int depth,
                        QString *error);
    void startNextJob();
    void scheduleTransferStep();
    void transferStep();
    TaskStart beginTask();
    void completeFile();
    void failFile(const QString &error);
    bool closeTransferFiles(bool success);
    void finishJob(bool cancelled);
    void emitProgress(bool force);
    bool destinationExists(const QString &path, quint64 *size, QDateTime *modified);
    QString uniqueDestination(const QString &path);

    std::unique_ptr<RemoteFileBackend> m_backend;
    QString m_unavailable = QStringLiteral("The file connection is not available.");
    QList<TransferJob> m_jobQueue;
    std::optional<TransferJob> m_job;
    QList<TransferTask> m_tasks;
    qsizetype m_taskIndex = 0;
    quint64 m_jobBytesTotal = 0;
    quint64 m_jobBytesDone = 0;
    quint64 m_taskBytesDone = 0;
    int m_jobFiles = 0;
    int m_filesDone = 0;
    int m_filesTransferred = 0;
    int m_skipped = 0;
    QStringList m_transferErrors;
    std::optional<SftpConflictResolution> m_conflictPolicy;
    std::optional<SftpConflictResolution> m_taskResolution;
    bool m_awaitingConflict = false;
    bool m_stepScheduled = false;
    bool m_cancelRequested = false;
    bool m_remoteFileOpen = false;
    // True once an upload has created or truncated m_writtenPath: the
    // destination, or its temporary name.
    bool m_remoteFileWritten = false;
    QString m_writtenPath;
    qint64 m_temporaryMinimum = 100 * 1024;
    std::unique_ptr<QFileDevice> m_localFile;
    QElapsedTimer m_progressTimer;
};

// GUI-thread handle for the files of one connection (SSH or FTP): what the
// file browser and folder sync use. Signals are delivered on the GUI thread.
class RemoteFileSession : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // "SFTP", "SCP", "FTP", "FTPS": shown in the file browser.
    virtual QString fileProtocol() const = 0;
    virtual void listDirectory(const QString &path) = 0;
    virtual void makeDirectory(const QString &path) = 0;
    virtual void renamePath(const QString &from, const QString &to) = 0;
    // Deletes files, and folders with all of their contents.
    virtual void removePaths(const QStringList &paths) = 0;
    // Transfers are queued and run one at a time; each returns its job ID.
    // Folders are copied recursively. With a policy, existing files are
    // handled that way without asking.
    virtual int upload(const QStringList &localPaths, const QString &remoteDirectory,
                       std::optional<SftpConflictResolution> policy = std::nullopt) = 0;
    virtual int download(const QStringList &remotePaths, const QString &localDirectory,
                         std::optional<SftpConflictResolution> policy = std::nullopt) = 0;
    // Lists a remote folder with everything below it (for folder sync);
    // answered by treeScanned().
    virtual void scanTree(const QString &path) = 0;
    // Answers the last transferConflict() signal.
    virtual void resolveConflict(SftpConflictResolution resolution, bool applyToAll) = 0;
    // Cancels the running transfer and everything queued after it.
    virtual void cancelTransfers() = 0;
    // Asks for the folder a remote shell is in; answered by shellFolder().
    virtual void queryShellFolder() {}
    // See RemoteFileEngine::setTemporaryUploads().
    void setTemporaryUploads(qint64 minimumBytes);

signals:
    // The file connection is ready (homePath: where it logged on), or will
    // not be available.
    void sftpReady(const QString &homePath);
    void sftpUnavailable(const QString &error);
    // path is canonical; on error, entries is empty.
    void directoryListed(const QString &path, const QList<SftpEntry> &entries,
                         const QString &error);
    // Reports the end of makeDirectory(), renamePath(), or removePaths().
    void fileOperationFinished(const QString &error);
    void treeScanned(const QString &path, const QList<SftpTreeEntry> &entries, const QString &error);
    void shellFolder(const QString &path);
    void transferProgress(int jobId, const QString &currentFile, quint64 bytesDone,
                          quint64 bytesTotal, int filesDone, int filesTotal, int queuedJobs);
    void transferConflict(const QString &path, quint64 existingSize,
                          const QDateTime &existingModified, quint64 newSize,
                          const QDateTime &newModified, bool upload);
    // errors lists per-file failures; skipped counts files the user chose to skip.
    void transferFinished(int jobId, bool upload, const QString &destination,
                          int filesTransferred, int skipped, const QStringList &errors,
                          bool cancelled);

protected:
    // Relays an engine's signals (living on another thread) to this handle's.
    void relay(RemoteFileEngine *engine);

private:
    RemoteFileEngine *m_engine = nullptr;
};
