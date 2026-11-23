// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "remotefiles.h"

#include <functional>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStyle>
#include <QToolButton>
#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QStackedWidget;
class QToolButton;
class QFileSystemWatcher;
class QTreeWidgetItem;
class SftpFileView;

// Remote file browser for one SSH tab (SFTP or SCP) or FTP tab. It shows its own folder and does not
// follow the terminal's working directory. MainWindow shows the panel of the
// active tab in the left-hand SFTP tab.
class SftpPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SftpPanel(QWidget *parent = nullptr);
    ~SftpPanel() override;
    // Removes local copies of edited files left behind by a crash.
    static void removeStaleEditFolders();

    void setTitle(const QString &title);
    void showPlaceholder(const QString &title, const QString &message);
    // The tab page its transfers belong to (the panel itself shows elsewhere).
    void setOwnerPage(QWidget *page) { m_ownerPage = page; }
    // Shown while no connection is attached.
    void setDisconnectedText(const QString &text);
    // Re-applies icons after the application's icon theme changed.
    void refreshIcons();
    // Attaches the panel to a connection, or detaches it (nullptr) after a
    // disconnect. The current folder is kept for the next connection.
    void setSession(RemoteFileSession *session);
    // Shows this folder if it is not the one shown (following the terminal).
    void followFolder(const QString &path);
    // An SSH tab's panel: whether it follows the terminal's folder, and a way
    // to switch that for the tab (offered in the right-click menu).
    void setFollowControl(std::function<bool()> following, std::function<void(bool)> setFollowing);
    void openSync();
    QString currentPath() const { return m_currentPath; }
    // The width that shows every column of the files listed without cutting
    // them off (0: no files listed, nothing to fit).
    int contentsWidth() const;
    // Uploads local files and folders into a remote folder (files dropped
    // on the terminal); an empty answer means started, else why not.
    QString uploadInto(const QStringList &localPaths, const QString &remoteDirectory);

private:
    std::function<bool()> m_following;
    std::function<void(bool)> m_setFollowing;
    void showStatus(const QString &text);
    void setStatusText(const QString &text); // showStatus without the log line
    void showMessage(const QString &text, bool error);
    void navigate(const QString &path);
    void refresh();
    void goUp();
    void populate(const QString &path, const QList<SftpEntry> &entries, const QString &error);
    void showContextMenu(const QPoint &position);
    bool handleKey(int key, Qt::KeyboardModifiers modifiers, bool execute);

    QStringList selectedPaths() const;
    QTreeWidgetItem *singleSelectedDirectory() const;
    void uploadPaths(const QStringList &localPaths, const QString &remoteDirectory);
    void pasteInto(const QString &remoteDirectory);
    void uploadFilesDialog();
    void uploadFolderDialog();
    void downloadSelected();
    void movePaths(const QStringList &paths, const QString &folder);
    void moveSelectedDialog();
    void editFile(const QString &remotePath);
    void openEditor(const QString &localPath);
    void uploadEdited(const QString &localPath);
    void createFolder();
    void renameSelected();
    void deleteSelected();
    void copySelectedPaths();

    void updateProgress(int jobId, const QString &currentFile, quint64 bytesDone,
                        quint64 bytesTotal, int filesDone, int filesTotal, int queuedJobs);
    void askConflict(const QString &path, quint64 existingSize, const QDateTime &existingModified,
                     quint64 newSize, const QDateTime &newModified, bool upload);
    void finishTransfer(int jobId, bool upload, const QString &destination, int filesTransferred,
                        int skipped, const QStringList &errors, bool cancelled);
    void resetTransfers();
    // Starts job (started: with its ID); first asks what to do when another
    // transfer is writing the same file or folder now (see startWaiting()).
    void startChecked(const RemoteFileSession::Job &job, std::function<void(int)> started);
    // What job would write, as TransferList targets; folders: those of them
    // that are folders.
    QStringList targetsOf(const RemoteFileSession::Job &job, QStringList *folders = nullptr) const;
    // Starts the transfers that waited for others once those have ended.
    void startWaiting();
    void endWaiting();

    QPointer<RemoteFileSession> m_session;
    QString m_homePath;
    QString m_currentPath;
    QString m_lastDownloadDirectory;
    QString m_title;
    QString m_protocol = QStringLiteral("SFTP");
    QString m_disconnectedText = QStringLiteral("Disconnected.\nPress Enter in the terminal to reconnect.");
    bool m_ready = false;

    QLabel *m_titleLabel = nullptr;
    QToolButton *m_upButton = nullptr;
    QToolButton *m_syncButton = nullptr;
    QToolButton *m_homeButton = nullptr;
    QToolButton *m_refreshButton = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_messageLabel = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_statusLabel = nullptr;
    SftpFileView *m_view = nullptr;

    struct ButtonIcon
    {
        QPointer<QToolButton> button;
        QString name;
        QStyle::StandardPixmap fallback;
    };
    QList<ButtonIcon> m_buttonIcons;
    QHash<int, bool> m_jobUploads;
    // Remote files being edited locally: local copy -> remote path. Each
    // save of the local copy uploads it again.
    QHash<QString, QString> m_editedFiles;
    QHash<int, QString> m_editDownloads; // download job -> local copy
    QHash<int, QString> m_editUploads; // upload job after a save -> remote path
    // The server's copy as last seen (size, modified in seconds), so a save
    // does not overwrite changes made there meanwhile; and the copy being
    // uploaded, which becomes that once the upload succeeds.
    struct FileVersion
    {
        quint64 size = 0;
        qint64 modified = 0;
        bool operator==(const FileVersion &) const = default;
    };
    QHash<QString, FileVersion> m_editVersions;
    QHash<QString, FileVersion> m_editUploading;
    QFileSystemWatcher *m_watcher = nullptr;
    QString m_editDirectory;
    QPointer<QWidget> m_ownerPage;
    QHash<int, int> m_transferIds; // job → TransferList id
    // Transfers waiting for others writing the same file (their entries in
    // the transfer list show them waiting).
    struct Waiting
    {
        int listId = 0;
        RemoteFileSession::Job job;
        QList<int> others; // TransferList ids
        std::function<void(int)> started;
    };
    QList<Waiting> m_waiting;
    bool m_startingWaiting = false;
    int m_speedJobId = 0;
    quint64 m_speedBytes = 0;
    double m_bytesPerSecond = 0;
    QElapsedTimer m_speedTimer;
};
