// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QList>
#include <QMutex>
#include <QRecursiveMutex>
#include <QString>
#include <QStringList>

#include <memory>

class QFile;
class RdpSession;
struct s_cliprdr_client_context;

// The RDP clipboard channel (cliprdr) for one connection: text both ways,
// and files both ways through FreeRDP's file streaming. Callbacks arrive on
// FreeRDP's channel thread, requests from the session's worker thread; the
// state is guarded by a mutex. Results reach the GUI through RdpSession's
// signals.
class RdpClipboard final
{
public:
    RdpClipboard(RdpSession *owner, bool shareText, bool shareFiles);
    ~RdpClipboard();

    // The channel opened (or closed: nullptr).
    void attach(s_cliprdr_client_context *channel);

    // What this computer's clipboard holds now; offered to the server.
    void setLocal(const QString &text, const QStringList &files);
    // Downloads the files last copied on the server into folder.
    void saveRemoteFiles(const QString &folder);
    void cancelSave();

private:
    struct LocalFile
    {
        QString path;
        QString name; // relative, with backslashes
        quint64 size = 0;
        bool directory = false;
    };
    struct RemoteFile
    {
        QString name; // relative, with forward slashes
        quint64 size = 0;
        bool directory = false;
    };

    friend struct RdpClipboardCallbacks;

    void sendFormatList();
    void requestNextChunk();
    void finishSave(const QString &error);

    RdpSession *m_owner;
    const bool m_shareText;
    const bool m_shareFiles;
    QMutex m_mutex;
    // Held while this class calls into the FreeRDP channel on its own
    // initiative and while the channel is attached or detached, so the
    // channel cannot go away during such a call. Callbacks answer through the
    // channel FreeRDP passes them. Always taken before m_mutex.
    QRecursiveMutex m_channelMutex;
    s_cliprdr_client_context *m_channel = nullptr;
    bool m_ready = false;

    QString m_localText;
    QList<LocalFile> m_localFiles;
    // How far the remote computer has read each offered file (pasting).
    QList<quint64> m_sentOffsets;
    quint64 m_sendTotal = 0;

    quint32 m_remoteTextFormat = 0;
    quint32 m_remoteFilesFormat = 0;
    quint32 m_pendingFormat = 0;
    QList<RemoteFile> m_remoteFiles;

    // A running download of m_remoteFiles.
    bool m_saving = false;
    QString m_saveFolder;
    qsizetype m_saveIndex = 0;
    quint64 m_saveOffset = 0;
    quint64 m_saveDone = 0;
    quint64 m_saveTotal = 0;
    quint32 m_streamId = 0;
    std::unique_ptr<QFile> m_saveFile;
    QStringList m_saved; // top-level paths, for the local clipboard
};
