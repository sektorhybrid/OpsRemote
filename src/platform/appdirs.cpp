// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

QString portableHomeFor(const QString &executableFolder)
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

QString portableHome()
{
    static const QString home = portableHomeFor(QCoreApplication::applicationDirPath());
    return home;
}

QString portableFilePath(const QString &name)
{
    return QDir(portableHome()).filePath(name);
}

bool makePrivateFolder(const QString &path)
{
    if (!QDir().mkpath(path))
        return false;
    restrictToOwner(path);
    return true;
}

bool savePrivateFile(const QString &path, const QByteArray &data, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("Could not create the folder %1.").arg(QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    // The temporary file (renamed at commit) is owner-only before any byte
    // is in it; Windows ignores this but restrictToOwner() below.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    restrictToOwner(path);
    return true;
}
