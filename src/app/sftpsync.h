// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "remotefiles.h"

#include <QDialog>
#include <QPointer>
#include <QSet>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// SFTP → Sync Folders: makes a server folder like a local one, or the other
// way round, copying only files that are new or changed (by size and
// modification time) and, if chosen, deleting what exists only on the
// target. Like rsync, but over the session's own SFTP connection, so it
// needs no rsync on either side and uses the session's log-on, jump hosts,
// and proxy. Transfers keep modification times, so a second run copies
// nothing that has not changed.
class SftpSyncDialog final : public QDialog
{
    Q_OBJECT

public:
    SftpSyncDialog(RemoteFileSession *session, const QString &remoteFolder, QWidget *parent = nullptr);

private:
    struct Plan
    {
        QStringList folders; // to create on the target, parents first
        QStringList copies; // files, relative paths
        QStringList deletions; // relative paths (files and whole folders)
        quint64 bytes = 0;
    };

    bool uploading() const;
    void preview();
    void compare(const QList<SftpTreeEntry> &remote);
    void sync();
    void advance();
    void setBusy(bool busy, const QString &status);

    QPointer<RemoteFileSession> m_session;
    QLineEdit *m_local = nullptr;
    QLineEdit *m_remote = nullptr;
    QComboBox *m_direction = nullptr;
    QCheckBox *m_deleteExtra = nullptr;
    QPushButton *m_previewButton = nullptr;
    QPushButton *m_syncButton = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_status = nullptr;
    Plan m_plan;
    bool m_planReady = false;
    // A sync: copies (and folder creations), then deletions.
    enum class Phase { Idle, Copying, Deleting };
    Phase m_phase = Phase::Idle;
    QSet<int> m_jobs;
    int m_pendingOperations = 0;
    QString m_localRoot;
    QString m_remoteRoot;
    bool m_upload = true;
    QStringList m_errors;
};
