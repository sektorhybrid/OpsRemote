// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "remotefiles.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <functional>

// Files over an SSH connection's SFTP subsystem.
class SftpBackend final : public RemoteFileBackend
{
public:
    // Takes over sftp; idle lets the connection handle its other channels.
    SftpBackend(ssh_session session, sftp_session sftp, std::function<void()> idle);
    ~SftpBackend() override;

    QString home() override;
    QString canonicalPath(const QString &path, QString *error) override;
    bool list(const QString &path, bool resolveLinks, QList<SftpEntry> *entries,
              QString *error) override;
    std::optional<Info> stat(const QString &path, bool followLinks, QString *error) override;
    bool makeDirectory(const QString &path, QString *error) override;
    bool rename(const QString &from, const QString &to, QString *error) override;
    bool removeFile(const QString &path, QString *error) override;
    bool removeDirectory(const QString &path, QString *error) override;
    bool replace(const QString &from, const QString &to, QString *error) override;
    bool openRead(const QString &path, QString *error) override;
    qint64 read(char *data, qint64 size, QString *error) override;
    bool openWrite(const QString &path, quint64 size, const QDateTime &modified, bool executable,
                   QString *error) override;
    bool write(const char *data, qint64 size, QString *error) override;
    bool closeFile(bool success, QString *error) override;
    void setModified(const QString &path, const QDateTime &modified) override;
    qint64 readChunk() const override { return m_readChunk; }
    qint64 writeChunk() const override { return m_writeChunk; }
    void idle() override { m_idle(); }

private:
    QString failure(const QString &action, const QString &path) const;

    ssh_session m_session;
    sftp_session m_sftp;
    std::function<void()> m_idle;
    sftp_file m_file = nullptr;
    QString m_filePath;
    qint64 m_readChunk = 32 * 1024;
    qint64 m_writeChunk = 32 * 1024;
};

// Files over SCP, for servers without SFTP (many routers, switches, and
// appliances): transfers run "scp -t" or "scp -f" on the server, and
// listings and folder operations run POSIX shell commands (ls, mkdir, mv,
// rm), each in a channel of its own.
class ScpBackend final : public RemoteFileBackend
{
public:
    ScpBackend(ssh_session session, std::function<void()> idle);
    ~ScpBackend() override;

    // Whether the server runs commands at all; sets home().
    bool start(QString *error);

    QString home() override { return m_home; }
    QString canonicalPath(const QString &path, QString *error) override;
    bool list(const QString &path, bool resolveLinks, QList<SftpEntry> *entries,
              QString *error) override;
    std::optional<Info> stat(const QString &path, bool followLinks, QString *error) override;
    bool makeDirectory(const QString &path, QString *error) override;
    bool rename(const QString &from, const QString &to, QString *error) override;
    bool removeFile(const QString &path, QString *error) override;
    bool removeDirectory(const QString &path, QString *error) override;
    bool removeTree(const QString &path, QString *error) override;
    bool replace(const QString &from, const QString &to, QString *error) override;
    bool openRead(const QString &path, QString *error) override;
    qint64 read(char *data, qint64 size, QString *error) override;
    bool openWrite(const QString &path, quint64 size, const QDateTime &modified, bool executable,
                   QString *error) override;
    bool write(const char *data, qint64 size, QString *error) override;
    bool closeFile(bool success, QString *error) override;
    bool exactListingTimes() const override { return m_exactTimes; }
    void idle() override { m_idle(); }

    // Single-quoted for a POSIX shell.
    static QByteArray quote(const QString &text);

private:
    // Runs a command with sh; output is its standard output.
    bool run(const QByteArray &script, QByteArray *output, QString *error, int timeoutMs = 30000);
    bool readExactly(char *data, qint64 size, QString *error);
    bool readLine(QByteArray *line, QString *error);
    // Reads the one-byte reply of an SCP sink; false (with error) unless 0.
    bool readAck(QString *error);
    void closeChannel();

    ssh_session m_session;
    std::function<void()> m_idle;
    QString m_home = QStringLiteral("/");
    // GNU ls: listings give seconds (--time-style=+%s).
    bool m_exactTimes = true;
    ssh_channel m_channel = nullptr;
    QString m_filePath;
    bool m_reading = false;
    quint64 m_remaining = 0;
};
