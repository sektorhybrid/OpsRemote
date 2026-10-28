// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

QString portableHome()
{
    static const QString home = [] {
        // Set by the launchers of the combined package.
        const auto fromLauncher = qEnvironmentVariable("OPSREMOTE_HOME");
        if (!fromLauncher.isEmpty() && QFileInfo(fromLauncher).isDir())
            return QDir(fromLauncher).absolutePath();
        // Otherwise: the executable's folder, or its parent when the
        // executable was started directly from inside the combined package.
        QDir directory(QCoreApplication::applicationDirPath());
        if (directory.exists(QStringLiteral("data")))
            return directory.absolutePath();
        if (directory.cdUp() && directory.exists(QStringLiteral("data")))
            return directory.absolutePath();
        return QCoreApplication::applicationDirPath();
    }();
    return home;
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
