// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sshfiles.h"

#include <QDir>
#include <QElapsedTimer>
#include <QSet>
#include <QRegularExpression>

#include <fcntl.h>
#ifndef _WIN32
#include <sys/time.h>
#endif

#include <algorithm>

namespace {
constexpr size_t MaximumSftpChunk = 256 * 1024;

QDateTime fromUnixTime(quint64 seconds)
{
    return seconds == 0 ? QDateTime() : QDateTime::fromSecsSinceEpoch(qint64(seconds));
}

}

// Whether an ls line has a month name in its date columns.
static bool monthLike(const QString &line)
{
    static const QRegularExpression month(
        QStringLiteral("\\s(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)\\s+\\d"));
    return month.match(line).hasMatch();
}

// ---- SFTP ----

SftpBackend::SftpBackend(ssh_session session, sftp_session sftp, std::function<void()> idle)
    : m_session(session), m_sftp(sftp), m_idle(std::move(idle))
{
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 11, 0)
    // libssh 0.10 has no limits query; it keeps the conservative default.
    if (sftp_limits_t limits = sftp_limits(m_sftp)) {
        const auto clampChunk = [](uint64_t length) {
            return length == 0 ? qint64(32 * 1024)
                               : qint64(std::clamp<uint64_t>(length, 4096, MaximumSftpChunk));
        };
        m_readChunk = clampChunk(limits->max_read_length);
        m_writeChunk = clampChunk(limits->max_write_length);
        sftp_limits_free(limits);
    }
#endif
}

SftpBackend::~SftpBackend()
{
    if (m_file)
        sftp_close(m_file);
    sftp_free(m_sftp);
}

QString SftpBackend::failure(const QString &action, const QString &path) const
{
    QString reason;
    switch (sftp_get_error(m_sftp)) {
    case SSH_FX_NO_SUCH_FILE:
    case SSH_FX_NO_SUCH_PATH:
        reason = QStringLiteral("no such file or folder");
        break;
    case SSH_FX_PERMISSION_DENIED:
        reason = QStringLiteral("permission denied");
        break;
    case SSH_FX_FILE_ALREADY_EXISTS:
        reason = QStringLiteral("it already exists");
        break;
    case SSH_FX_WRITE_PROTECT:
        reason = QStringLiteral("the file system is read-only");
        break;
    case SSH_FX_NO_CONNECTION:
    case SSH_FX_CONNECTION_LOST:
        reason = QStringLiteral("the connection was lost");
        break;
    default:
        reason = QString::fromUtf8(ssh_get_error(m_session));
        if (reason.isEmpty())
            reason = QStringLiteral("the server reported a failure");
        break;
    }
    return QStringLiteral("Could not %1 %2: %3").arg(action, path, reason);
}

QString SftpBackend::home()
{
    QString home = QStringLiteral("/");
    if (char *path = sftp_canonicalize_path(m_sftp, ".")) {
        home = QString::fromUtf8(path);
        ssh_string_free_char(path);
    }
    return home;
}

QString SftpBackend::canonicalPath(const QString &path, QString *error)
{
    if (char *canonical = sftp_canonicalize_path(m_sftp, path.toUtf8().constData())) {
        const auto result = QString::fromUtf8(canonical);
        ssh_string_free_char(canonical);
        return result;
    }
    *error = failure(QStringLiteral("open"), path);
    return {};
}

bool SftpBackend::list(const QString &path, bool resolveLinks, QList<SftpEntry> *entries,
                       QString *error)
{
    sftp_dir directory = sftp_opendir(m_sftp, path.toUtf8().constData());
    if (!directory) {
        *error = failure(QStringLiteral("open"), path);
        return false;
    }
    while (sftp_attributes attributes = sftp_readdir(m_sftp, directory)) {
        SftpEntry entry;
        entry.name = QString::fromUtf8(attributes->name);
        if (entry.name != QStringLiteral(".") && entry.name != QStringLiteral("..")) {
            entry.isDirectory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
            entry.isSymlink = attributes->type == SSH_FILEXFER_TYPE_SYMLINK;
            entry.isOther = !entry.isDirectory && !entry.isSymlink
                && attributes->type != SSH_FILEXFER_TYPE_REGULAR;
            entry.size = attributes->size;
            entry.modified = fromUnixTime(attributes->mtime);
            entry.permissions = attributes->permissions;
            if (entry.isSymlink && resolveLinks) {
                // Show links to folders as folders so they can be opened.
                const auto target = RemotePaths::join(path, entry.name).toUtf8();
                if (sftp_attributes resolved = sftp_stat(m_sftp, target.constData())) {
                    entry.isDirectory = resolved->type == SSH_FILEXFER_TYPE_DIRECTORY;
                    entry.size = resolved->size;
                    sftp_attributes_free(resolved);
                }
            }
            entries->append(entry);
        }
        sftp_attributes_free(attributes);
    }
    const bool complete = sftp_dir_eof(directory);
    sftp_closedir(directory);
    if (!complete)
        *error = failure(QStringLiteral("list"), path);
    return complete;
}

std::optional<RemoteFileBackend::Info> SftpBackend::stat(const QString &path, bool followLinks,
                                                         QString *error)
{
    const auto bytes = path.toUtf8();
    sftp_attributes attributes = followLinks ? sftp_stat(m_sftp, bytes.constData())
                                             : sftp_lstat(m_sftp, bytes.constData());
    if (!attributes) {
        *error = failure(QStringLiteral("read"), path);
        return std::nullopt;
    }
    Info info;
    info.isDirectory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
    info.isRegular = attributes->type == SSH_FILEXFER_TYPE_REGULAR;
    info.isSymlink = attributes->type == SSH_FILEXFER_TYPE_SYMLINK;
    info.size = attributes->size;
    info.modified = fromUnixTime(attributes->mtime);
    info.permissions = attributes->permissions;
    sftp_attributes_free(attributes);
    return info;
}

bool SftpBackend::makeDirectory(const QString &path, QString *error)
{
    if (sftp_mkdir(m_sftp, path.toUtf8().constData(), 0755) < 0) {
        *error = failure(QStringLiteral("create folder"), path);
        return false;
    }
    return true;
}

bool SftpBackend::rename(const QString &from, const QString &to, QString *error)
{
    // libssh uses posix-rename where offered, which replaces an existing
    // entry silently; a rename never should.
    if (sftp_attributes existing = sftp_lstat(m_sftp, to.toUtf8().constData())) {
        sftp_attributes_free(existing);
        *error = QStringLiteral("Could not rename %1: %2 already exists.").arg(from, to);
        return false;
    }
    if (sftp_rename(m_sftp, from.toUtf8().constData(), to.toUtf8().constData()) < 0) {
        *error = failure(QStringLiteral("rename"), from);
        return false;
    }
    return true;
}

bool SftpBackend::removeFile(const QString &path, QString *error)
{
    if (sftp_unlink(m_sftp, path.toUtf8().constData()) < 0) {
        *error = failure(QStringLiteral("delete"), path);
        return false;
    }
    return true;
}

bool SftpBackend::removeDirectory(const QString &path, QString *error)
{
    if (sftp_rmdir(m_sftp, path.toUtf8().constData()) < 0) {
        *error = failure(QStringLiteral("delete"), path);
        return false;
    }
    return true;
}

bool SftpBackend::replace(const QString &from, const QString &to, QString *error)
{
    // libssh uses posix-rename where the server offers it, which replaces
    // in one step; plain SFTP rename refuses an existing target.
    const auto fromBytes = from.toUtf8();
    const auto toBytes = to.toUtf8();
    if (sftp_rename(m_sftp, fromBytes.constData(), toBytes.constData()) == 0)
        return true;
    if (sftp_unlink(m_sftp, toBytes.constData()) == 0
        && sftp_rename(m_sftp, fromBytes.constData(), toBytes.constData()) == 0)
        return true;
    *error = failure(QStringLiteral("replace"), to);
    return false;
}

bool SftpBackend::openRead(const QString &path, QString *error)
{
    m_file = sftp_open(m_sftp, path.toUtf8().constData(), O_RDONLY, 0);
    m_filePath = path;
    if (!m_file)
        *error = failure(QStringLiteral("open"), path);
    return m_file;
}

qint64 SftpBackend::read(char *data, qint64 size, QString *error)
{
    const auto count = sftp_read(m_file, data, size_t(size));
    if (count < 0)
        *error = failure(QStringLiteral("read"), m_filePath);
    return count;
}

bool SftpBackend::openWrite(const QString &path, quint64, const QDateTime &, bool executable,
                            QString *error)
{
    m_file = sftp_open(m_sftp, path.toUtf8().constData(), O_WRONLY | O_CREAT | O_TRUNC,
                       executable ? 0755 : 0644);
    m_filePath = path;
    if (!m_file)
        *error = failure(QStringLiteral("create"), path);
    return m_file;
}

bool SftpBackend::write(const char *data, qint64 size, QString *error)
{
    for (qint64 offset = 0; offset < size;) {
        const auto written = sftp_write(m_file, data + offset, size_t(size - offset));
        if (written <= 0) {
            *error = failure(QStringLiteral("write"), m_filePath);
            return false;
        }
        offset += written;
    }
    return true;
}

bool SftpBackend::closeFile(bool, QString *error)
{
    if (!m_file)
        return true;
    const bool closed = sftp_close(std::exchange(m_file, nullptr)) == SSH_OK;
    if (!closed)
        *error = failure(QStringLiteral("save"), m_filePath);
    return closed;
}

void SftpBackend::setModified(const QString &path, const QDateTime &modified)
{
    const auto seconds = modified.toSecsSinceEpoch();
    const struct timeval times[2] = {{static_cast<decltype(timeval::tv_sec)>(seconds), 0},
                                     {static_cast<decltype(timeval::tv_sec)>(seconds), 0}};
    sftp_utimes(m_sftp, path.toUtf8().constData(), times);
}

// ---- SCP ----

ScpBackend::ScpBackend(ssh_session session, std::function<void()> idle)
    : m_session(session), m_idle(std::move(idle))
{
}

ScpBackend::~ScpBackend()
{
    closeChannel();
}

QByteArray ScpBackend::quote(const QString &text)
{
    QByteArray quoted = text.toUtf8();
    quoted.replace('\'', "'\\''");
    return '\'' + quoted + '\'';
}

bool ScpBackend::run(const QByteArray &script, QByteArray *output, QString *error, int timeoutMs)
{
    ssh_channel channel = ssh_channel_new(m_session);
    if (!channel) {
        *error = QString::fromUtf8(ssh_get_error(m_session));
        return false;
    }
    // Through sh, whatever the account's login shell is.
    const QByteArray command = "sh -c " + quote(QString::fromUtf8(script));
    if (ssh_channel_open_session(channel) != SSH_OK
        || ssh_channel_request_exec(channel, command.constData()) != SSH_OK) {
        *error = QStringLiteral("The server refused to run a command: %1")
                     .arg(QString::fromUtf8(ssh_get_error(m_session)));
        ssh_channel_free(channel);
        return false;
    }
    QByteArray errors;
    QElapsedTimer timer;
    timer.start();
    char buffer[16384];
    bool ended = false;
    while (!ended && timer.elapsed() < timeoutMs && output->size() < 64 * 1024 * 1024) {
        ended = true;
        for (const int stream : {0, 1}) {
            const int available = ssh_channel_poll_timeout(channel, stream == 0 ? 100 : 0, stream);
            if (available == SSH_ERROR) {
                ended = true;
                break;
            }
            if (available == SSH_EOF)
                continue;
            ended = false;
            if (available <= 0)
                continue;
            const int count = ssh_channel_read_nonblocking(channel, buffer,
                                                           uint32_t(qMin<int>(available, sizeof buffer)),
                                                           stream);
            if (count > 0)
                (stream == 0 ? *output : errors).append(buffer, count);
        }
    }
    int status = -1;
    if (ended) {
        ssh_channel_send_eof(channel);
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 11, 0)
        uint32_t code = 0;
        char *signal = nullptr;
        int core = 0;
        if (ssh_channel_get_exit_state(channel, &code, &signal, &core) == SSH_OK)
            status = signal ? 128 : int(code);
        ssh_string_free_char(signal);
#else
        status = ssh_channel_get_exit_status(channel);
#endif
    }
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    if (!ended) {
        *error = QStringLiteral("The server did not answer in time.");
        return false;
    }
    if (status != 0) {
        *error = QString::fromUtf8(errors).trimmed().section(QLatin1Char('\n'), -1);
        if (error->isEmpty())
            *error = QStringLiteral("the command failed (exit status %1)").arg(status);
        return false;
    }
    return true;
}

bool ScpBackend::start(QString *error)
{
    QByteArray output;
    if (!run("pwd", &output, error))
        return false;
    const auto home = QString::fromUtf8(output).trimmed();
    if (!home.startsWith(QLatin1Char('/'))) {
        *error = QStringLiteral("The server's shell is not a POSIX shell.");
        return false;
    }
    m_home = home;
    return true;
}

QString ScpBackend::canonicalPath(const QString &path, QString *error)
{
    QByteArray output;
    QString reason;
    if (!run("cd -- " + quote(path) + " && pwd -P", &output, &reason)) {
        *error = QStringLiteral("Could not open %1: %2").arg(path, reason);
        return {};
    }
    return QString::fromUtf8(output).trimmed();
}

bool ScpBackend::list(const QString &path, bool resolveLinks, QList<SftpEntry> *entries,
                      QString *error)
{
    // GNU ls gives exact times with --time-style; others the usual columns.
    QByteArray script = "cd -- " + quote(path) + " || exit 1\n"
                        "LC_ALL=C ls -lan --time-style=+%s 2>/dev/null || LC_ALL=C ls -lan || exit 1\n";
    if (resolveLinks)
        script += "echo //\n"
                  "for f in .* *; do [ -L \"$f\" ] && [ -d \"$f\" ] && printf '%s\\n' \"$f\"; done\n"
                  "exit 0\n";
    QByteArray output;
    QString reason;
    if (!run(script, &output, &reason)) {
        *error = QStringLiteral("Could not list %1: %2").arg(path, reason);
        return false;
    }
    const auto lines = QString::fromUtf8(output).split(QLatin1Char('\n'));
    QSet<QString> folderLinks;
    qsizetype marker = lines.indexOf(QStringLiteral("//"));
    if (marker >= 0) {
        for (qsizetype index = marker + 1; index < lines.size(); ++index) {
            if (!lines[index].isEmpty())
                folderLinks.insert(lines[index]);
        }
    } else {
        marker = lines.size();
    }
    for (qsizetype index = 0; index < marker; ++index) {
        SftpEntry entry;
        // GNU ls gives "perms links owner group size seconds name".
        if (monthLike(lines[index]))
            m_exactTimes = false;
        if (!RemotePaths::parseLsLine(lines[index], &entry) || entry.name == QStringLiteral(".")
            || entry.name == QStringLiteral(".."))
            continue;
        if (entry.isSymlink && folderLinks.contains(entry.name))
            entry.isDirectory = true;
        entries->append(entry);
    }
    return true;
}

std::optional<RemoteFileBackend::Info> ScpBackend::stat(const QString &path, bool followLinks,
                                                        QString *error)
{
    const QByteArray flags = followLinks ? "-ldnL" : "-ldn";
    const QByteArray script = "LC_ALL=C ls " + flags + " --time-style=+%s -- " + quote(path)
                              + " 2>/dev/null || LC_ALL=C ls " + flags + " -- " + quote(path);
    QByteArray output;
    QString reason;
    if (!run(script, &output, &reason)) {
        *error = QStringLiteral("Could not read %1: %2").arg(path, reason);
        return std::nullopt;
    }
    SftpEntry entry;
    if (!RemotePaths::parseLsLine(QString::fromUtf8(output).section(QLatin1Char('\n'), 0, 0), &entry)) {
        *error = QStringLiteral("Could not read %1: the server's answer was not understood.").arg(path);
        return std::nullopt;
    }
    Info info;
    info.isDirectory = entry.isDirectory;
    info.isSymlink = entry.isSymlink;
    info.isRegular = !entry.isDirectory && !entry.isSymlink && !entry.isOther;
    info.size = entry.size;
    info.modified = entry.modified;
    info.permissions = entry.permissions;
    return info;
}

bool ScpBackend::makeDirectory(const QString &path, QString *error)
{
    QByteArray output;
    QString reason;
    if (run("mkdir -- " + quote(path), &output, &reason))
        return true;
    *error = QStringLiteral("Could not create folder %1: %2").arg(path, reason);
    return false;
}

bool ScpBackend::rename(const QString &from, const QString &to, QString *error)
{
    // Like SFTP, never replaces an existing entry.
    const QByteArray script = "if [ -e " + quote(to) + " ] || [ -L " + quote(to)
                              + " ]; then echo 'it already exists' >&2; exit 1; fi\nmv -- "
                              + quote(from) + ' ' + quote(to);
    QByteArray output;
    QString reason;
    if (run(script, &output, &reason))
        return true;
    *error = QStringLiteral("Could not rename %1: %2").arg(from, reason);
    return false;
}

bool ScpBackend::removeFile(const QString &path, QString *error)
{
    QByteArray output;
    QString reason;
    if (run("rm -f -- " + quote(path), &output, &reason))
        return true;
    *error = QStringLiteral("Could not delete %1: %2").arg(path, reason);
    return false;
}

bool ScpBackend::removeDirectory(const QString &path, QString *error)
{
    QByteArray output;
    QString reason;
    if (run("rmdir -- " + quote(path), &output, &reason))
        return true;
    *error = QStringLiteral("Could not delete %1: %2").arg(path, reason);
    return false;
}

bool ScpBackend::removeTree(const QString &path, QString *error)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty() || clean == QStringLiteral("/") || !clean.startsWith(QLatin1Char('/'))) {
        *error = QStringLiteral("Could not delete %1: not a file or folder below /.").arg(path);
        return false;
    }
    QByteArray output;
    QString reason;
    if (run("rm -rf -- " + quote(clean), &output, &reason, 600000))
        return true;
    *error = QStringLiteral("Could not delete %1: %2").arg(path, reason);
    return false;
}

bool ScpBackend::replace(const QString &from, const QString &to, QString *error)
{
    QByteArray output;
    QString reason;
    if (run("mv -f -- " + quote(from) + ' ' + quote(to), &output, &reason))
        return true;
    *error = QStringLiteral("Could not replace %1: %2").arg(to, reason);
    return false;
}

void ScpBackend::closeChannel()
{
    if (!m_channel)
        return;
    if (ssh_channel_is_open(m_channel)) {
        ssh_channel_send_eof(m_channel);
        ssh_channel_close(m_channel);
    }
    ssh_channel_free(m_channel);
    m_channel = nullptr;
}

bool ScpBackend::readExactly(char *data, qint64 size, QString *error)
{
    while (size > 0) {
        const int count = ssh_channel_read_timeout(m_channel, data, uint32_t(qMin<qint64>(size, 65536)),
                                                   0, 30000);
        if (count <= 0) {
            *error = count == 0 && !ssh_channel_is_eof(m_channel)
                ? QStringLiteral("the server did not answer in time")
                : QStringLiteral("the connection closed");
            return false;
        }
        data += count;
        size -= count;
    }
    return true;
}

bool ScpBackend::readLine(QByteArray *line, QString *error)
{
    line->clear();
    char c = 0;
    while (line->size() < 4096) {
        if (!readExactly(&c, 1, error))
            return false;
        if (c == '\n')
            return true;
        line->append(c);
    }
    *error = QStringLiteral("the server's answer was not understood");
    return false;
}

bool ScpBackend::readAck(QString *error)
{
    char code = 0;
    if (!readExactly(&code, 1, error))
        return false;
    if (code == 0)
        return true;
    QByteArray message;
    QString ignored;
    readLine(&message, &ignored);
    *error = code == 1 || code == 2 ? QString::fromUtf8(message).trimmed()
                                    : QStringLiteral("the server's answer was not understood");
    if (error->startsWith(QStringLiteral("scp: ")))
        error->remove(0, 5);
    return false;
}

bool ScpBackend::openRead(const QString &path, QString *error)
{
    m_filePath = path;
    m_reading = true;
    m_channel = ssh_channel_new(m_session);
    const QByteArray command = "scp -p -f " + quote(path);
    QString reason;
    const auto fail = [&](const QString &why) {
        *error = QStringLiteral("Could not read %1: %2").arg(path, why);
        closeChannel();
        return false;
    };
    if (!m_channel || ssh_channel_open_session(m_channel) != SSH_OK
        || ssh_channel_request_exec(m_channel, command.constData()) != SSH_OK)
        return fail(QStringLiteral("the server refused to run scp"));
    // The source waits for a zero byte before each message.
    for (int round = 0; round < 3; ++round) {
        if (ssh_channel_write(m_channel, "", 1) != 1)
            return fail(QStringLiteral("the connection closed"));
        QByteArray line;
        if (!readLine(&line, &reason))
            return fail(reason.isEmpty() ? QStringLiteral("scp is not installed on the server")
                                         : reason);
        if (line.startsWith('T'))
            continue; // times; the listing already has them
        if (line.startsWith('\1') || line.startsWith('\2')) {
            QString message = QString::fromUtf8(line.mid(1)).trimmed();
            if (message.startsWith(QStringLiteral("scp: ")))
                message.remove(0, 5);
            return fail(message);
        }
        if (!line.startsWith('C'))
            return fail(QStringLiteral("it is not a file"));
        // C<mode> <size> <name>
        const auto parts = line.mid(1).split(' ');
        bool ok = false;
        m_remaining = parts.size() >= 3 ? parts[1].toULongLong(&ok) : 0;
        if (!ok)
            return fail(QStringLiteral("the server's answer was not understood"));
        if (ssh_channel_write(m_channel, "", 1) != 1)
            return fail(QStringLiteral("the connection closed"));
        return true;
    }
    return fail(QStringLiteral("the server's answer was not understood"));
}

qint64 ScpBackend::read(char *data, qint64 size, QString *error)
{
    if (!m_channel)
        return -1;
    if (m_remaining == 0) {
        // The end: the source's status byte, then our acknowledgement.
        QString reason;
        if (!readAck(&reason)) {
            *error = QStringLiteral("Could not read %1: %2").arg(m_filePath, reason);
            return -1;
        }
        ssh_channel_write(m_channel, "", 1);
        return 0;
    }
    const int count = ssh_channel_read_timeout(m_channel, data,
                                               uint32_t(qMin<quint64>(quint64(size), m_remaining)),
                                               0, 30000);
    if (count <= 0) {
        *error = QStringLiteral("Could not read %1: the connection closed").arg(m_filePath);
        return -1;
    }
    m_remaining -= quint64(count);
    return count;
}

bool ScpBackend::openWrite(const QString &path, quint64 size, const QDateTime &modified,
                           bool executable, QString *error)
{
    m_filePath = path;
    m_reading = false;
    m_remaining = size;
    const QString name = RemotePaths::fileName(path);
    QString reason;
    const auto fail = [&](const QString &why) {
        *error = QStringLiteral("Could not create %1: %2").arg(path, why);
        closeChannel();
        return false;
    };
    if (name.contains(QLatin1Char('\n')))
        return fail(QStringLiteral("SCP cannot send names with line breaks"));
    m_channel = ssh_channel_new(m_session);
    const QByteArray command = "scp -p -t " + quote(path);
    if (!m_channel || ssh_channel_open_session(m_channel) != SSH_OK
        || ssh_channel_request_exec(m_channel, command.constData()) != SSH_OK)
        return fail(QStringLiteral("the server refused to run scp"));
    if (!readAck(&reason))
        return fail(reason.isEmpty() ? QStringLiteral("scp is not installed on the server") : reason);
    const auto send = [this](const QByteArray &message) {
        return ssh_channel_write(m_channel, message.constData(), uint32_t(message.size()))
            == int(message.size());
    };
    if (modified.isValid()) {
        const auto seconds = QByteArray::number(modified.toSecsSinceEpoch());
        if (!send('T' + seconds + " 0 " + seconds + " 0\n") || !readAck(&reason))
            return fail(reason);
    }
    const QByteArray header = QByteArray(executable ? "C0755 " : "C0644 ") + QByteArray::number(size)
                              + ' ' + name.toUtf8() + '\n';
    if (!send(header) || !readAck(&reason))
        return fail(reason);
    return true;
}

bool ScpBackend::write(const char *data, qint64 size, QString *error)
{
    // A file that grew since the transfer began is sent at its first size.
    size = qMin<qint64>(size, qint64(m_remaining));
    while (size > 0) {
        const int written = ssh_channel_write(m_channel, data, uint32_t(qMin<qint64>(size, 65536)));
        if (written <= 0) {
            *error = QStringLiteral("Could not write %1: the connection closed").arg(m_filePath);
            return false;
        }
        data += written;
        size -= written;
        m_remaining -= quint64(written);
    }
    return true;
}

bool ScpBackend::closeFile(bool success, QString *error)
{
    if (!m_channel)
        return true;
    bool closed = true;
    if (success && !m_reading) {
        QString reason;
        if (m_remaining != 0) {
            closed = false;
            reason = QStringLiteral("the local file changed while it was sent");
        } else if (ssh_channel_write(m_channel, "", 1) != 1 || !readAck(&reason)) {
            closed = false;
        }
        if (!closed)
            *error = QStringLiteral("Could not save %1: %2").arg(m_filePath, reason);
    }
    closeChannel();
    return closed;
}
