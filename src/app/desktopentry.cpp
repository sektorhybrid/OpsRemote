// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "desktopentry.h"

#include "appdirs.h"
#include "capabilities.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace DesktopEntry {

namespace {

// Marks the files OpsRemote wrote, the only ones it changes or removes.
constexpr char Mark[] = "X-OpsRemote-Portable=true";

// A path in an Exec line: quoted, with the characters the desktop file
// specification reserves escaped.
QString quotedForExec(QString path)
{
    for (const char *reserved : {"\\", "\"", "`", "$"})
        path.replace(QLatin1String(reserved), QLatin1String("\\") + QLatin1String(reserved));
    return QLatin1Char('"') + path + QLatin1Char('"');
}

// $XDG_DATA_HOME/applications, else ~/.local/share/applications.
QString applicationsFolder()
{
    QString data = qEnvironmentVariable("XDG_DATA_HOME");
    if (data.isEmpty() || !QDir::isAbsolutePath(data))
        data = QDir::home().filePath(QStringLiteral(".local/share"));
    return QDir(data).filePath(QStringLiteral("applications"));
}

// This copy's launcher: the combined package's, else a single bundle's.
QString launcher()
{
    for (const auto *name : {"OpsRemote", "legacy/OpsRemote.sh", "opsremote.sh"}) {
        const QFileInfo file(portableFilePath(QLatin1String(name)));
        if (file.isFile() && file.isExecutable())
            return file.absoluteFilePath();
    }
    return {};
}

bool ours(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.readAll().contains(Mark);
}

}

QString update(bool wanted)
{
    if (!Platform::has(Platform::Feature::DesktopEntry))
        return {};
    const QString folder = applicationsFolder();
    const QString path = QDir(folder).filePath(QLatin1String(Id) + QStringLiteral(".desktop"));
    const bool exists = QFileInfo::exists(path);

    if (!wanted) {
        if (!exists)
            return QStringLiteral("no taskbar entry");
        if (!ours(path))
            return QStringLiteral("%1 was not written by OpsRemote: left alone").arg(path);
        return QFile::remove(path) ? QStringLiteral("removed %1").arg(path)
                                   : QStringLiteral("could not remove %1").arg(path);
    }

    const QFileInfo place(folder);
    if (!place.isDir() || !place.isWritable())
        return QStringLiteral("%1 is missing or read-only: no taskbar entry").arg(folder);
    const QString program = launcher();
    const QString icon = portableFilePath(QStringLiteral("data/opsremote.png"));
    if (program.isEmpty() || !QFileInfo(icon).isFile())
        return QStringLiteral("no launcher or icon in this copy (a developer build?): no taskbar entry");
    if (exists && !ours(path))
        return QStringLiteral("%1 was not written by OpsRemote: left alone").arg(path);

    const QByteArray content = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=OpsRemote\n"
        "Comment=Remote connections: SSH, RDP, VNC, files and more\n"
        "Exec=%1\n"
        "Icon=%2\n"
        "Terminal=false\n"
        // Only for the taskbar: not listed in the applications menu.
        "NoDisplay=true\n"
        "StartupWMClass=opsremote\n"
        "%3\n")
        .arg(quotedForExec(program), icon, QLatin1String(Mark))
        .toUtf8();
    if (exists) {
        QFile current(path);
        if (current.open(QIODevice::ReadOnly) && current.readAll() == content)
            return QStringLiteral("taskbar entry up to date");
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit())
        return QStringLiteral("could not write %1: %2").arg(path, file.errorString());
    return QStringLiteral("wrote %1 for %2").arg(path, program);
}

}
