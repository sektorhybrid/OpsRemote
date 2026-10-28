// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "rdpclipboard.h"
#include "rdpsession.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QStorageInfo>

#include <freerdp/channels/cliprdr.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/utils/cliprdr_utils.h>
#include <winpr/file.h>
#include <winpr/shell.h>
#include <winpr/user.h>

#include <algorithm>
#include <cstring>

namespace {
// Our id for the file list we offer; registered formats are 0xC000 or more.
constexpr UINT32 LocalFilesFormat = 0xC0E0;
const char *const FileListFormatName = "FileGroupDescriptorW";
// One file contents request at a time, this many bytes each.
constexpr UINT32 ChunkSize = 1024 * 1024;
// A remote file list is ignored beyond this many entries.
constexpr UINT32 MaximumRemoteEntries = 100000;

RdpClipboard *clipboardOf(CliprdrClientContext *channel)
{
    return static_cast<RdpClipboard *>(channel->custom);
}

UINT respondFormatList(CliprdrClientContext *channel, bool ok)
{
    CLIPRDR_FORMAT_LIST_RESPONSE response{};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    return channel->ClientFormatListResponse(channel, &response);
}

UINT respondData(CliprdrClientContext *channel, const QByteArray *data)
{
    CLIPRDR_FORMAT_DATA_RESPONSE response{};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.common.dataLen = data ? static_cast<UINT32>(data->size()) : 0;
    response.requestedFormatData = data ? reinterpret_cast<const BYTE *>(data->constData()) : nullptr;
    return channel->ClientFormatDataResponse(channel, &response);
}

UINT respondContents(CliprdrClientContext *channel, UINT32 streamId, const QByteArray *data)
{
    CLIPRDR_FILE_CONTENTS_RESPONSE response{};
    response.common.msgType = CB_FILECONTENTS_RESPONSE;
    response.common.msgFlags = data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.streamId = streamId;
    response.cbRequested = data ? static_cast<UINT32>(data->size()) : 0;
    response.requestedData = data ? reinterpret_cast<const BYTE *>(data->constData()) : nullptr;
    return channel->ClientFileContentsResponse(channel, &response);
}

// A remote file name is untrusted: keep it inside the target folder.
QString safeRelativeName(const QString &name)
{
    QStringList parts;
    for (const auto &part : QString(name).replace(QLatin1Char('\\'), QLatin1Char('/'))
                                .split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".") || part == QStringLiteral("..") || part.contains(QLatin1Char(':')))
            return QString();
        parts.append(part);
    }
    return parts.join(QLatin1Char('/'));
}
}

// cliprdr callbacks; they run on FreeRDP's channel thread.
struct RdpClipboardCallbacks
{
    static UINT monitorReady(CliprdrClientContext *channel, const CLIPRDR_MONITOR_READY *)
    {
        auto *clipboard = clipboardOf(channel);
        CLIPRDR_GENERAL_CAPABILITY_SET general{};
        general.capabilitySetType = CB_CAPSTYPE_GENERAL;
        general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
        general.version = CB_CAPS_VERSION_2;
        general.generalFlags = CB_USE_LONG_FORMAT_NAMES;
        if (clipboard->m_shareFiles)
            general.generalFlags |= CB_STREAM_FILECLIP_ENABLED | CB_FILECLIP_NO_FILE_PATHS
                                    | CB_HUGE_FILE_SUPPORT_ENABLED;
        CLIPRDR_CAPABILITIES capabilities{};
        capabilities.cCapabilitiesSets = 1;
        capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET *>(&general);
        const UINT result = channel->ClientCapabilities(channel, &capabilities);
        {
            QMutexLocker locker(&clipboard->m_mutex);
            clipboard->m_ready = true;
        }
        clipboard->sendFormatList();
        return result;
    }

    static UINT serverCapabilities(CliprdrClientContext *, const CLIPRDR_CAPABILITIES *)
    {
        return CHANNEL_RC_OK;
    }

    // The server's clipboard changed: fetch text at once, or the file list.
    static UINT serverFormatList(CliprdrClientContext *channel, const CLIPRDR_FORMAT_LIST *list)
    {
        auto *clipboard = clipboardOf(channel);
        UINT32 text = 0;
        UINT32 files = 0;
        for (UINT32 index = 0; index < list->numFormats; ++index) {
            const auto &format = list->formats[index];
            if (format.formatId == CF_UNICODETEXT)
                text = format.formatId;
            else if (format.formatName && std::strcmp(format.formatName, FileListFormatName) == 0)
                files = format.formatId;
        }
        respondFormatList(channel, true);
        UINT32 wanted = 0;
        {
            QMutexLocker locker(&clipboard->m_mutex);
            clipboard->m_remoteTextFormat = text;
            clipboard->m_remoteFilesFormat = files;
            clipboard->m_remoteFiles.clear();
            if (files && clipboard->m_shareFiles)
                wanted = files;
            else if (text && clipboard->m_shareText)
                wanted = text;
            clipboard->m_pendingFormat = wanted;
        }
        if (!wanted)
            return CHANNEL_RC_OK;
        CLIPRDR_FORMAT_DATA_REQUEST request{};
        request.common.msgType = CB_FORMAT_DATA_REQUEST;
        request.requestedFormatId = wanted;
        return channel->ClientFormatDataRequest(channel, &request);
    }

    static UINT serverFormatListResponse(CliprdrClientContext *, const CLIPRDR_FORMAT_LIST_RESPONSE *)
    {
        return CHANNEL_RC_OK;
    }

    static UINT serverFormatDataResponse(CliprdrClientContext *channel,
                                         const CLIPRDR_FORMAT_DATA_RESPONSE *response)
    {
        auto *clipboard = clipboardOf(channel);
        UINT32 format = 0;
        {
            QMutexLocker locker(&clipboard->m_mutex);
            format = std::exchange(clipboard->m_pendingFormat, 0u);
        }
        if (!(response->common.msgFlags & CB_RESPONSE_OK) || !response->requestedFormatData)
            return CHANNEL_RC_OK;
        auto *owner = clipboard->m_owner;
        if (format == CF_UNICODETEXT) {
            const auto *characters = reinterpret_cast<const char16_t *>(response->requestedFormatData);
            const qsizetype length = response->common.dataLen / 2;
            QString text = QString::fromUtf16(characters, length);
            const auto end = text.indexOf(QChar(0));
            if (end >= 0)
                text.truncate(end);
            QMetaObject::invokeMethod(owner, [owner, text] { emit owner->remoteClipboardText(text); },
                                      Qt::QueuedConnection);
            return CHANNEL_RC_OK;
        }
        FILEDESCRIPTORW *descriptors = nullptr;
        UINT32 count = 0;
        if (cliprdr_parse_file_list(response->requestedFormatData, response->common.dataLen,
                                    &descriptors, &count) != CHANNEL_RC_OK)
            return CHANNEL_RC_OK;
        if (count > MaximumRemoteEntries) {
            free(descriptors);
            const auto message = QStringLiteral("The remote computer copied more than %1 items; "
                                                "copy smaller parts.").arg(MaximumRemoteEntries);
            QMetaObject::invokeMethod(owner, [owner, message] {
                emit owner->remoteFilesSaved({}, message);
            }, Qt::QueuedConnection);
            return CHANNEL_RC_OK;
        }
        QList<RdpClipboard::RemoteFile> remote;
        QStringList topLevel;
        quint64 total = 0;
        for (UINT32 index = 0; index < count; ++index) {
            const auto &descriptor = descriptors[index];
            const auto name = safeRelativeName(QString::fromUtf16(
                reinterpret_cast<const char16_t *>(descriptor.cFileName)));
            const bool directory = (descriptor.dwFlags & FD_ATTRIBUTES)
                                   && (descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            const quint64 size = (quint64(descriptor.nFileSizeHigh) << 32) | descriptor.nFileSizeLow;
            // Entries with unsafe names keep their place, as indexes matter.
            remote.append({name, directory ? 0 : size, directory});
            total += directory ? 0 : size;
            if (!name.isEmpty() && !name.contains(QLatin1Char('/')))
                topLevel.append(name);
        }
        free(descriptors);
        {
            QMutexLocker locker(&clipboard->m_mutex);
            clipboard->m_remoteFiles = remote;
        }
        QMetaObject::invokeMethod(owner, [owner, topLevel, total] {
            emit owner->remoteFilesAvailable(topLevel, total);
        }, Qt::QueuedConnection);
        return CHANNEL_RC_OK;
    }

    // The server pastes: send what this computer offered.
    static UINT serverFormatDataRequest(CliprdrClientContext *channel,
                                        const CLIPRDR_FORMAT_DATA_REQUEST *request)
    {
        auto *clipboard = clipboardOf(channel);
        QByteArray data;
        bool ok = false;
        {
            QMutexLocker locker(&clipboard->m_mutex);
            if (request->requestedFormatId == CF_UNICODETEXT && clipboard->m_shareText) {
                // UTF-16LE with a terminating NUL, Windows line ends.
                QString text = clipboard->m_localText;
                text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
                    .replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
                data = QByteArray(reinterpret_cast<const char *>(text.utf16()),
                                  (text.size() + 1) * 2);
                ok = true;
            } else if (request->requestedFormatId == LocalFilesFormat && clipboard->m_shareFiles) {
                QList<FILEDESCRIPTORW> descriptors;
                for (const auto &file : std::as_const(clipboard->m_localFiles)) {
                    FILEDESCRIPTORW descriptor{};
                    descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_PROGRESSUI;
                    descriptor.dwFileAttributes = file.directory ? FILE_ATTRIBUTE_DIRECTORY
                                                                 : FILE_ATTRIBUTE_NORMAL;
                    descriptor.nFileSizeHigh = static_cast<DWORD>(file.size >> 32);
                    descriptor.nFileSizeLow = static_cast<DWORD>(file.size & 0xffffffffu);
                    const auto name = file.name.left(259);
                    std::memcpy(descriptor.cFileName, name.utf16(),
                                static_cast<size_t>(name.size()) * 2);
                    descriptors.append(descriptor);
                }
                BYTE *serialized = nullptr;
                UINT32 length = 0;
                if (cliprdr_serialize_file_list_ex(CB_STREAM_FILECLIP_ENABLED | CB_HUGE_FILE_SUPPORT_ENABLED,
                                                   descriptors.constData(),
                                                   static_cast<UINT32>(descriptors.size()),
                                                   &serialized, &length)
                    == CHANNEL_RC_OK) {
                    data = QByteArray(reinterpret_cast<const char *>(serialized), int(length));
                    ok = true;
                }
                free(serialized);
            }
        }
        return respondData(channel, ok ? &data : nullptr);
    }

    // The server reads one of the offered files: its size or a byte range.
    static UINT serverFileContentsRequest(CliprdrClientContext *channel,
                                          const CLIPRDR_FILE_CONTENTS_REQUEST *request)
    {
        auto *clipboard = clipboardOf(channel);
        RdpClipboard::LocalFile file;
        {
            QMutexLocker locker(&clipboard->m_mutex);
            if (!clipboard->m_shareFiles || request->listIndex >= quint32(clipboard->m_localFiles.size()))
                return respondContents(channel, request->streamId, nullptr);
            file = clipboard->m_localFiles[request->listIndex];
        }
        if (request->dwFlags & FILECONTENTS_SIZE) {
            QByteArray size(8, Qt::Uninitialized);
            const quint64 value = file.size;
            std::memcpy(size.data(), &value, 8); // little-endian UINT64
            return respondContents(channel, request->streamId, &size);
        }
        QFile input(file.path);
        const quint64 position = (quint64(request->nPositionHigh) << 32) | request->nPositionLow;
        if (file.directory || !input.open(QIODevice::ReadOnly) || !input.seek(qint64(position)))
            return respondContents(channel, request->streamId, nullptr);
        const QByteArray data = input.read(qMin<qint64>(request->cbRequested, 8 * ChunkSize));
        // Progress of the paste on the remote side: the furthest position read
        // of every file, over the size of all of them.
        quint64 sent = 0;
        quint64 total = 0;
        {
            QMutexLocker locker(&clipboard->m_mutex);
            if (request->listIndex < quint32(clipboard->m_sentOffsets.size())) {
                // Reading a finished file from the start again: a new paste.
                if (position == 0 && clipboard->m_sentOffsets[request->listIndex] >= file.size
                    && file.size > 0)
                    std::fill(clipboard->m_sentOffsets.begin(), clipboard->m_sentOffsets.end(), 0);
                auto &offset = clipboard->m_sentOffsets[request->listIndex];
                offset = qMax(offset, position + quint64(data.size()));
            }
            for (const auto offset : std::as_const(clipboard->m_sentOffsets))
                sent += offset;
            total = clipboard->m_sendTotal;
        }
        auto *owner = clipboard->m_owner;
        const QString name = file.name.section(QLatin1Char('\\'), -1);
        QMetaObject::invokeMethod(owner, [owner, sent, total, name] {
            emit owner->localFilesProgress(sent, total, name);
        }, Qt::QueuedConnection);
        return respondContents(channel, request->streamId, &data);
    }

    // A chunk of a remote file we are saving.
    static UINT serverFileContentsResponse(CliprdrClientContext *channel,
                                           const CLIPRDR_FILE_CONTENTS_RESPONSE *response)
    {
        auto *clipboard = clipboardOf(channel);
        QString error;
        bool done = false;
        quint64 progressDone = 0;
        quint64 progressTotal = 0;
        QString current;
        {
            QMutexLocker locker(&clipboard->m_mutex);
            if (!clipboard->m_saving || response->streamId != clipboard->m_streamId)
                return CHANNEL_RC_OK;
            if (!(response->common.msgFlags & CB_RESPONSE_OK)) {
                error = QStringLiteral("The remote computer stopped sending the files (its "
                                       "clipboard may have changed).");
            } else {
                const auto &file = clipboard->m_remoteFiles[clipboard->m_saveIndex];
                if (response->cbRequested > 0
                    && clipboard->m_saveFile->write(reinterpret_cast<const char *>(response->requestedData),
                                                    response->cbRequested)
                           != qint64(response->cbRequested)) {
                    error = QStringLiteral("Could not write %1.").arg(clipboard->m_saveFile->fileName());
                } else {
                    clipboard->m_saveOffset += response->cbRequested;
                    clipboard->m_saveDone += response->cbRequested;
                    if (clipboard->m_saveOffset >= file.size || response->cbRequested == 0) {
                        clipboard->m_saveFile->close();
                        clipboard->m_saveFile.reset();
                        ++clipboard->m_saveIndex;
                        clipboard->m_saveOffset = 0;
                    }
                }
                progressDone = clipboard->m_saveDone;
                progressTotal = clipboard->m_saveTotal;
                current = file.name;
            }
        }
        if (!error.isEmpty()) {
            clipboard->finishSave(error);
            return CHANNEL_RC_OK;
        }
        auto *owner = clipboard->m_owner;
        QMetaObject::invokeMethod(owner, [owner, progressDone, progressTotal, current] {
            emit owner->remoteFilesProgress(progressDone, progressTotal, current);
        }, Qt::QueuedConnection);
        Q_UNUSED(done);
        clipboard->requestNextChunk();
        return CHANNEL_RC_OK;
    }
};

RdpClipboard::RdpClipboard(RdpSession *owner, bool shareText, bool shareFiles)
    : m_owner(owner), m_shareText(shareText), m_shareFiles(shareFiles)
{
}

RdpClipboard::~RdpClipboard() = default;

void RdpClipboard::attach(CliprdrClientContext *channel)
{
    QMutexLocker channelLocker(&m_channelMutex);
    QMutexLocker locker(&m_mutex);
    m_channel = channel;
    m_ready = false;
    if (!channel)
        return;
    channel->custom = this;
    channel->MonitorReady = RdpClipboardCallbacks::monitorReady;
    channel->ServerCapabilities = RdpClipboardCallbacks::serverCapabilities;
    channel->ServerFormatList = RdpClipboardCallbacks::serverFormatList;
    channel->ServerFormatListResponse = RdpClipboardCallbacks::serverFormatListResponse;
    channel->ServerFormatDataRequest = RdpClipboardCallbacks::serverFormatDataRequest;
    channel->ServerFormatDataResponse = RdpClipboardCallbacks::serverFormatDataResponse;
    channel->ServerFileContentsRequest = RdpClipboardCallbacks::serverFileContentsRequest;
    channel->ServerFileContentsResponse = RdpClipboardCallbacks::serverFileContentsResponse;
}

void RdpClipboard::setLocal(const QString &text, const QStringList &files)
{
    QList<LocalFile> list;
    if (m_shareFiles) {
        // Folders are sent with everything inside them.
        for (const auto &path : files) {
            const QFileInfo info(path);
            if (!info.exists())
                continue;
            const QString base = info.fileName();
            list.append({info.absoluteFilePath(), base, quint64(info.isDir() ? 0 : info.size()),
                         info.isDir()});
            if (!info.isDir())
                continue;
            QDirIterator it(info.absoluteFilePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QFileInfo entry(it.next());
                const auto relative = QDir(info.absoluteFilePath()).relativeFilePath(entry.absoluteFilePath());
                list.append({entry.absoluteFilePath(),
                             base + QLatin1Char('\\') + QString(relative).replace(QLatin1Char('/'), QLatin1Char('\\')),
                             quint64(entry.isDir() ? 0 : entry.size()), entry.isDir()});
            }
        }
    }
    {
        QMutexLocker locker(&m_mutex);
        m_localText = m_shareText ? text : QString();
        m_localFiles = list;
        m_sentOffsets = QList<quint64>(list.size(), 0);
        m_sendTotal = 0;
        for (const auto &file : std::as_const(list))
            m_sendTotal += file.size;
    }
    sendFormatList();
}

void RdpClipboard::sendFormatList()
{
    QMutexLocker channelLocker(&m_channelMutex);
    QMutexLocker locker(&m_mutex);
    if (!m_channel || !m_ready)
        return;
    QList<CLIPRDR_FORMAT> formats;
    if (!m_localFiles.isEmpty())
        formats.append({LocalFilesFormat, const_cast<char *>(FileListFormatName)});
    else if (!m_localText.isEmpty())
        formats.append({CF_UNICODETEXT, nullptr});
    CLIPRDR_FORMAT_LIST list{};
    list.common.msgType = CB_FORMAT_LIST;
    list.numFormats = static_cast<UINT32>(formats.size());
    list.formats = formats.data();
    auto *channel = m_channel;
    locker.unlock();
    // m_channelMutex keeps the channel attached during the call.
    (void)channel->ClientFormatList(channel, &list);
}

void RdpClipboard::saveRemoteFiles(const QString &folder)
{
    quint64 total = 0;
    {
        QMutexLocker locker(&m_mutex);
        for (const auto &file : std::as_const(m_remoteFiles))
            total += file.size;
    }
    // Do not start what cannot fit.
    const QStorageInfo storage(folder);
    if (storage.isValid() && quint64(storage.bytesAvailable()) < total) {
        auto *owner = m_owner;
        const auto message = QStringLiteral("Not enough free space in %1.")
                                 .arg(QDir::toNativeSeparators(folder));
        QMetaObject::invokeMethod(owner, [owner, message] { emit owner->remoteFilesSaved({}, message); },
                                  Qt::QueuedConnection);
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        if (m_saving || m_remoteFiles.isEmpty() || !m_channel)
            return;
        m_saving = true;
        m_saveFolder = folder;
        m_saveIndex = 0;
        m_saveOffset = 0;
        m_saveDone = 0;
        m_saveTotal = 0;
        m_saved.clear();
        for (const auto &file : std::as_const(m_remoteFiles))
            m_saveTotal += file.size;
        ++m_streamId;
    }
    requestNextChunk();
}

void RdpClipboard::cancelSave()
{
    finishSave(QStringLiteral("Cancelled."));
}

// Opens the next file (creating folders on the way) and asks for its next
// chunk; finishes when every entry is done.
void RdpClipboard::requestNextChunk()
{
    // Keeps the channel attached until the request below has been sent.
    QMutexLocker channelLocker(&m_channelMutex);
    CLIPRDR_FILE_CONTENTS_REQUEST request{};
    CliprdrClientContext *channel = nullptr;
    QString error;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_saving)
            return;
        while (!m_saveFile && m_saveIndex < m_remoteFiles.size()) {
            const auto &file = m_remoteFiles[m_saveIndex];
            if (file.name.isEmpty()) { // unsafe name, skipped
                ++m_saveIndex;
                continue;
            }
            const QString target = QDir(m_saveFolder).filePath(file.name);
            if (!file.name.contains(QLatin1Char('/')))
                m_saved.append(target);
            if (file.directory) {
                if (!QDir().mkpath(target)) {
                    error = QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(target));
                    break;
                }
                ++m_saveIndex;
                continue;
            }
            QDir().mkpath(QFileInfo(target).absolutePath());
            m_saveFile = std::make_unique<QFile>(target);
            if (!m_saveFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                error = QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(target));
                m_saveFile.reset();
                break;
            }
            if (file.size == 0) { // nothing to request
                m_saveFile.reset();
                ++m_saveIndex;
            }
        }
        if (error.isEmpty() && m_saveFile) {
            const auto &file = m_remoteFiles[m_saveIndex];
            request.common.msgType = CB_FILECONTENTS_REQUEST;
            request.streamId = m_streamId;
            request.listIndex = static_cast<UINT32>(m_saveIndex);
            request.dwFlags = FILECONTENTS_RANGE;
            request.nPositionLow = static_cast<UINT32>(m_saveOffset & 0xffffffffu);
            request.nPositionHigh = static_cast<UINT32>(m_saveOffset >> 32);
            request.cbRequested = static_cast<UINT32>(qMin<quint64>(ChunkSize, file.size - m_saveOffset));
            channel = m_channel;
        }
    }
    if (!error.isEmpty()) {
        finishSave(error);
        return;
    }
    if (!channel) { // all entries done, or the channel closed
        bool closed = false;
        {
            QMutexLocker locker(&m_mutex);
            closed = !m_channel && m_saving;
        }
        finishSave(closed ? QStringLiteral("The connection closed.") : QString());
        return;
    }
    (void)channel->ClientFileContentsRequest(channel, &request);
}

void RdpClipboard::finishSave(const QString &error)
{
    QStringList saved;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_saving)
            return;
        m_saving = false;
        if (m_saveFile) {
            m_saveFile->close();
            if (!error.isEmpty())
                m_saveFile->remove(); // never leave a half-written file
            m_saveFile.reset();
        }
        saved = m_saved;
        ++m_streamId;
    }
    auto *owner = m_owner;
    QMetaObject::invokeMethod(owner, [owner, saved, error] {
        emit owner->remoteFilesSaved(saved, error);
    }, Qt::QueuedConnection);
}
