// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// Through /proc (Linux).

#include "processinfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace ProcessInfo {

QString workingFolder(int pid)
{
    return QFileInfo(QStringLiteral("/proc/%1/cwd").arg(pid)).symLinkTarget();
}

QString commandLine(int pid)
{
    QFile file(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    auto parts = QString::fromLocal8Bit(file.readAll()).split(QChar(0), Qt::SkipEmptyParts);
    if (!parts.isEmpty())
        parts.first() = QFileInfo(parts.first()).fileName();
    return parts.join(QLatin1Char(' '));
}

bool otherProcessRunsFrom(const QString &folder)
{
    const QString prefix = QDir::cleanPath(folder) + QLatin1Char('/');
    const auto own = QString::number(QCoreApplication::applicationPid());
    for (const auto &pid : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (pid == own || !pid.front().isDigit())
            continue;
        const auto image = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
        if (image.startsWith(prefix))
            return true;
    }
    return false;
}

}
