// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "sshsession.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QStyle>
#include <QToolButton>
#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QStackedWidget;
class QToolButton;
class QTreeWidgetItem;
class SftpFileView;

// Remote file browser for one SSH tab. It shows its own folder and does not
// follow the terminal's working directory. MainWindow shows the panel of the
// active tab in the left-hand SFTP tab.
class SftpPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SftpPanel(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    // Re-applies icons after the application's icon theme changed.
    void refreshIcons();
    // Attaches the panel to a connection, or detaches it (nullptr) after a
    // disconnect. The current folder is kept for the next connection.
    void setSession(SshSession *session);

private:
    void showStatus(const QString &text);
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

    QPointer<SshSession> m_session;
    QString m_homePath;
    QString m_currentPath;
    QString m_lastDownloadDirectory;
    bool m_ready = false;

    QLabel *m_titleLabel = nullptr;
    QToolButton *m_upButton = nullptr;
    QToolButton *m_homeButton = nullptr;
    QToolButton *m_refreshButton = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_messageLabel = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_statusLabel = nullptr;
    SftpFileView *m_view = nullptr;

    QWidget *m_progressFooter = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_progressDetail = nullptr;
    struct ButtonIcon
    {
        QPointer<QToolButton> button;
        QString name;
        QStyle::StandardPixmap fallback;
    };
    QList<ButtonIcon> m_buttonIcons;
    QHash<int, bool> m_jobUploads;
    int m_speedJobId = 0;
    quint64 m_speedBytes = 0;
    double m_bytesPerSecond = 0;
    QElapsedTimer m_speedTimer;
};
