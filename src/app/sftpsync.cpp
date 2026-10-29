// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sftpsync.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

struct Item
{
    bool isDirectory = false;
    quint64 size = 0;
    QDateTime modified;
};

// The same file: equal size and modification time (within 2 s, as FAT and
// some servers keep only even seconds).
bool same(const Item &a, const Item &b)
{
    return a.size == b.size && a.modified.isValid() && b.modified.isValid()
           && qAbs(a.modified.secsTo(b.modified)) <= 2;
}

QString joinRemote(const QString &folder, const QString &relative)
{
    if (relative.isEmpty())
        return folder;
    return folder.endsWith(QLatin1Char('/')) ? folder + relative : folder + QLatin1Char('/') + relative;
}

QString parentOf(const QString &relative)
{
    const int slash = relative.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : relative.left(slash);
}

QString sizeText(quint64 bytes)
{
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg((bytes + 1023) / 1024);
    return QStringLiteral("%1 MB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
}

}

SftpSyncDialog::SftpSyncDialog(RemoteFileSession *session, const QString &remoteFolder, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("Sync Folders"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 540);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_local = new QLineEdit(this);
    m_local->setPlaceholderText(QStringLiteral("a folder on this computer"));
    auto *browse = new QPushButton(QStringLiteral("Browse…"), this);
    auto *localRow = new QHBoxLayout;
    localRow->addWidget(m_local, 1);
    localRow->addWidget(browse);
    m_remote = new QLineEdit(remoteFolder, this);
    m_direction = new QComboBox(this);
    m_direction->addItem(QStringLiteral("Upload: make the server folder like this computer's"), true);
    m_direction->addItem(QStringLiteral("Download: make this computer's folder like the server's"), false);
    m_deleteExtra = new QCheckBox(QStringLiteral("Delete files that exist only in the target folder"), this);
    m_deleteExtra->setToolTip(QStringLiteral("Like rsync --delete: the target becomes an exact copy."));
    form->addRow(QStringLiteral("Local folder:"), localRow);
    form->addRow(QStringLiteral("Server folder:"), m_remote);
    form->addRow(QStringLiteral("Direction:"), m_direction);
    form->addRow(QString(), m_deleteExtra);
    layout->addLayout(form);
    m_list = new QTreeWidget(this);
    m_list->setRootIsDecorated(false);
    m_list->setHeaderLabels({QStringLiteral("Action"), QStringLiteral("Path"), QStringLiteral("Size")});
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(m_list, 1);
    m_status = new QLabel(QStringLiteral("Choose the folders, then Preview to see what would change."), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *buttons = new QDialogButtonBox(this);
    m_previewButton = buttons->addButton(QStringLiteral("Preview"), QDialogButtonBox::ActionRole);
    m_syncButton = buttons->addButton(QStringLiteral("Sync"), QDialogButtonBox::AcceptRole);
    m_syncButton->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    // Any change of the folders or options asks for a new preview.
    const auto invalidate = [this] {
        m_planReady = false;
        m_syncButton->setEnabled(false);
    };
    connect(m_local, &QLineEdit::textChanged, this, invalidate);
    connect(m_remote, &QLineEdit::textChanged, this, invalidate);
    connect(m_direction, &QComboBox::currentIndexChanged, this, invalidate);
    connect(m_deleteExtra, &QCheckBox::toggled, this, invalidate);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Local Folder"), m_local->text());
        if (!folder.isEmpty())
            m_local->setText(QDir::toNativeSeparators(folder));
    });
    connect(m_previewButton, &QPushButton::clicked, this, &SftpSyncDialog::preview);
    connect(m_syncButton, &QPushButton::clicked, this, &SftpSyncDialog::sync);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    if (m_session) {
        connect(m_session, &RemoteFileSession::treeScanned, this,
                [this](const QString &path, const QList<SftpTreeEntry> &entries, const QString &error) {
                    if (path != m_remote->text().trimmed())
                        return; // another dialog's scan
                    if (!error.isEmpty())
                        return setBusy(false, QStringLiteral("Cannot read the server folder: %1").arg(error));
                    compare(entries);
                });
        connect(m_session, &RemoteFileSession::transferFinished, this,
                [this](int jobId, bool, const QString &, int, int, const QStringList &errors, bool cancelled) {
                    if (!m_jobs.remove(jobId))
                        return;
                    m_errors += errors;
                    if (cancelled)
                        m_errors.append(QStringLiteral("Cancelled."));
                    advance();
                });
        // The results of this dialog's folder creations and deletions.
        connect(m_session, &RemoteFileSession::fileOperationFinished, this, [this](const QString &error) {
            if (m_pendingOperations <= 0)
                return;
            --m_pendingOperations;
            if (!error.isEmpty())
                m_errors.append(error);
            advance();
        });
        connect(m_session, &QObject::destroyed, this, [this] {
            m_phase = Phase::Idle;
            setBusy(false, QStringLiteral("The connection closed; nothing more was changed."));
            m_previewButton->setEnabled(false);
        });
    }
}

bool SftpSyncDialog::uploading() const
{
    return m_direction->currentData().toBool();
}

void SftpSyncDialog::setBusy(bool busy, const QString &status)
{
    m_previewButton->setEnabled(!busy && m_session);
    m_syncButton->setEnabled(!busy && m_planReady && m_session);
    // Nothing that changes the plan while it runs.
    for (QWidget *field : {static_cast<QWidget *>(m_local), static_cast<QWidget *>(m_remote),
                           static_cast<QWidget *>(m_direction), static_cast<QWidget *>(m_deleteExtra)})
        field->setEnabled(!busy);
    m_status->setText(status);
}

void SftpSyncDialog::preview()
{
    const QString local = m_local->text().trimmed();
    const QString remote = m_remote->text().trimmed();
    if (!QFileInfo(local).isDir())
        return setBusy(false, QStringLiteral("Choose an existing local folder."));
    if (remote.isEmpty() || !m_session)
        return setBusy(false, QStringLiteral("Enter the server folder (it must exist)."));
    m_list->clear();
    m_planReady = false;
    setBusy(true, QStringLiteral("Reading the server folder…"));
    m_session->scanTree(remote);
}

void SftpSyncDialog::compare(const QList<SftpTreeEntry> &remoteEntries)
{
    // Both sides as relative path -> item.
    QHash<QString, Item> local;
    const QDir root(m_local->text().trimmed());
    QDirIterator it(root.absolutePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info = it.nextFileInfo();
        if (info.isSymLink())
            continue; // as on the server: links are not followed
        local.insert(root.relativeFilePath(info.absoluteFilePath()),
                     {info.isDir(), quint64(info.size()), info.lastModified()});
    }
    QHash<QString, Item> remote;
    for (const auto &entry : remoteEntries)
        remote.insert(entry.path, {entry.isDirectory, entry.size, entry.modified});

    const bool up = uploading();
    const QHash<QString, Item> &source = up ? local : remote;
    const QHash<QString, Item> &target = up ? remote : local;
    m_plan = {};
    QStringList paths = source.keys();
    paths.sort();
    for (const auto &path : std::as_const(paths)) {
        const Item &item = source.value(path);
        const auto existing = target.constFind(path);
        if (item.isDirectory) {
            if (existing == target.cend())
                m_plan.folders.append(path);
            continue;
        }
        if (existing == target.cend() || existing->isDirectory || !same(item, *existing)) {
            m_plan.copies.append(path);
            m_plan.bytes += item.size;
        }
    }
    if (m_deleteExtra->isChecked()) {
        QStringList extra;
        for (auto it = target.cbegin(); it != target.cend(); ++it) {
            if (!source.contains(it.key()))
                extra.append(it.key());
        }
        extra.sort();
        // A folder that goes takes its contents with it.
        for (const auto &path : std::as_const(extra)) {
            const bool inside = std::any_of(m_plan.deletions.cbegin(), m_plan.deletions.cend(),
                                            [&path](const QString &folder) {
                                                return path.startsWith(folder + QLatin1Char('/'));
                                            });
            if (!inside)
                m_plan.deletions.append(path);
        }
    }

    for (const auto &path : std::as_const(m_plan.folders))
        new QTreeWidgetItem(m_list, {QStringLiteral("Create folder"), path, QString()});
    for (const auto &path : std::as_const(m_plan.copies)) {
        new QTreeWidgetItem(m_list, {target.contains(path) ? QStringLiteral("Update") : QStringLiteral("Copy"),
                                     path, sizeText(source.value(path).size)});
    }
    for (const auto &path : std::as_const(m_plan.deletions))
        new QTreeWidgetItem(m_list, {QStringLiteral("Delete"), path, QString()});
    m_planReady = !m_plan.folders.isEmpty() || !m_plan.copies.isEmpty() || !m_plan.deletions.isEmpty();
    setBusy(false, m_planReady
                       ? QStringLiteral("%1 file(s) to copy (%2), %3 folder(s) to create, %4 to delete. "
                                        "Sync to do it.")
                             .arg(m_plan.copies.size())
                             .arg(sizeText(m_plan.bytes))
                             .arg(m_plan.folders.size())
                             .arg(m_plan.deletions.size())
                       : QStringLiteral("Already in sync: nothing to do."));
}

void SftpSyncDialog::sync()
{
    if (!m_planReady || !m_session || m_phase != Phase::Idle)
        return;
    if (!m_plan.deletions.isEmpty()
        && QMessageBox::question(this, QStringLiteral("Sync Folders"),
                                 QStringLiteral("Delete %1 file(s) or folder(s) from the %2 that are "
                                                "not in the %3? They are deleted only after every "
                                                "copy succeeded.")
                                     .arg(m_plan.deletions.size())
                                     .arg(uploading() ? QStringLiteral("server") : QStringLiteral("this computer"),
                                          uploading() ? QStringLiteral("local folder") : QStringLiteral("server folder")))
               != QMessageBox::Yes)
        return;
    m_localRoot = QDir(m_local->text().trimmed()).absolutePath();
    m_remoteRoot = m_remote->text().trimmed();
    m_upload = uploading();
    m_errors.clear();
    m_jobs.clear();
    m_pendingOperations = 0;
    m_phase = Phase::Copying;
    m_planReady = false;
    setBusy(true, QStringLiteral("Copying…"));

    // Folders first (parents come first in the sorted plan), then the files
    // grouped by their folder, one transfer each, replacing older copies.
    // The server runs them in this order.
    QHash<QString, QStringList> byFolder;
    for (const auto &path : std::as_const(m_plan.copies))
        byFolder[parentOf(path)].append(path);
    if (m_upload) {
        for (const auto &folder : std::as_const(m_plan.folders)) {
            ++m_pendingOperations;
            m_session->makeDirectory(joinRemote(m_remoteRoot, folder));
        }
        for (auto it = byFolder.cbegin(); it != byFolder.cend(); ++it) {
            QStringList sources;
            for (const auto &path : it.value())
                sources.append(QDir(m_localRoot).filePath(path));
            m_jobs.insert(m_session->upload(sources, joinRemote(m_remoteRoot, it.key()),
                                            SftpConflictResolution::Overwrite));
        }
    } else {
        for (const auto &folder : std::as_const(m_plan.folders)) {
            if (!QDir().mkpath(QDir(m_localRoot).filePath(folder)))
                m_errors.append(QStringLiteral("Cannot create %1")
                                    .arg(QDir::toNativeSeparators(QDir(m_localRoot).filePath(folder))));
        }
        for (auto it = byFolder.cbegin(); it != byFolder.cend(); ++it) {
            QStringList sources;
            for (const auto &path : it.value())
                sources.append(joinRemote(m_remoteRoot, path));
            const QString folder = QDir(m_localRoot).filePath(it.key());
            QDir().mkpath(folder);
            m_jobs.insert(m_session->download(sources, folder, SftpConflictResolution::Overwrite));
        }
    }
    advance();
}

// After each finished copy or operation: deletions once every copy is done
// and none failed, then the result.
void SftpSyncDialog::advance()
{
    if (m_phase == Phase::Idle || !m_jobs.isEmpty() || m_pendingOperations > 0)
        return;
    const auto finish = [this](const QString &done) {
        m_phase = Phase::Idle;
        setBusy(false, m_errors.isEmpty() ? done
                                          : QStringLiteral("Finished with problems: %1")
                                                .arg(m_errors.mid(0, 5).join(QStringLiteral("; "))));
    };
    if (m_phase == Phase::Deleting || m_plan.deletions.isEmpty())
        return finish(QStringLiteral("Done: the folders are in sync."));
    if (!m_errors.isEmpty()) {
        m_errors.prepend(QStringLiteral("Nothing was deleted because a copy failed"));
        return finish(QString());
    }
    m_phase = Phase::Deleting;
    setBusy(true, QStringLiteral("Deleting what is only in the target…"));
    if (m_upload) {
        QStringList remove;
        for (const auto &path : std::as_const(m_plan.deletions)) {
            // Relative paths from the scan, never leaving the folder.
            if (path.isEmpty() || path.split(QLatin1Char('/')).contains(QStringLiteral("..")))
                continue;
            remove.append(joinRemote(m_remoteRoot, path));
        }
        if (!remove.isEmpty()) {
            ++m_pendingOperations;
            m_session->removePaths(remove);
            return;
        }
        return advance();
    }
    // This computer: each path is checked again just before it goes, as
    // the folder may have changed since the preview.
    const QString root = QFileInfo(m_localRoot).canonicalFilePath();
    for (const auto &path : std::as_const(m_plan.deletions)) {
        const QString full = QDir(m_localRoot).filePath(path);
        const QFileInfo info(full);
        if (!info.exists() && !info.isSymLink())
            continue; // already gone
        const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
        if (root.isEmpty() || (parent != root && !parent.startsWith(root + QLatin1Char('/')))) {
            m_errors.append(QStringLiteral("Not deleted (outside the folder now): %1")
                                .arg(QDir::toNativeSeparators(full)));
            continue;
        }
        // A link is removed itself, never what it points to.
        const bool removed = info.isSymLink() || !info.isDir() ? QFile::remove(full)
                                                               : QDir(full).removeRecursively();
        if (!removed)
            m_errors.append(QStringLiteral("Cannot delete %1").arg(QDir::toNativeSeparators(full)));
    }
    advance();
}
