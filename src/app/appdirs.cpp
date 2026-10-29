// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstring>
#endif

namespace {

QString homeFor(const QString &executableFolder)
{
    // Set by the launchers of the combined package.
    const auto fromLauncher = qEnvironmentVariable("OPSREMOTE_HOME");
    if (!fromLauncher.isEmpty() && QFileInfo(fromLauncher).isDir())
        return QDir(fromLauncher).absolutePath();
    // Otherwise: the executable's folder, or its parent when the
    // executable was started directly from inside the combined package.
    QDir directory(executableFolder);
    if (directory.exists(QStringLiteral("data")))
        return directory.absolutePath();
    if (directory.cdUp() && directory.exists(QStringLiteral("data")))
        return directory.absolutePath();
    return executableFolder;
}

}

QString portableHome()
{
    static const QString home = homeFor(QCoreApplication::applicationDirPath());
    return home;
}

QString portableHomeBeforeStart()
{
#ifdef Q_OS_LINUX
    return homeFor(QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath().section(QLatin1Char('/'), 0, -2));
#else
    return homeFor(QString());
#endif
}

QString portableFilePath(const QString &name)
{
    return QDir(portableHome()).filePath(name);
}

bool restrictToOwner(const QString &path)
{
    if (QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return true;
    qWarning("Could not restrict %s to its owner; this file system may not support "
             "permissions.", qUtf8Printable(QDir::toNativeSeparators(path)));
    return false;
}

bool replaceFile(const QString &from, const QString &to, QString *error)
{
#ifdef Q_OS_WIN
    if (MoveFileExW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(from).utf16()),
                    reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(to).utf16()),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    if (error)
        *error = QStringLiteral("error %1").arg(GetLastError());
    return false;
#else
    if (std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return true;
    if (error)
        *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
#endif
}
