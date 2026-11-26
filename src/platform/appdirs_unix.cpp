// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"

#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstdio>
#include <cstring>

QString portableHomeBeforeStart()
{
#ifdef Q_OS_LINUX
    return portableHomeFor(QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath().section(QLatin1Char('/'), 0, -2));
#else
    return portableHomeFor(QString());
#endif
}

bool replaceFile(const QString &from, const QString &to, QString *error)
{
    if (std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return true;
    if (error)
        *error = QString::fromLocal8Bit(std::strerror(errno));
    return false;
}

bool restrictToOwner(const QString &path)
{
    const auto owner = QFileInfo(path).isDir()
                           ? QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                           : QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (QFile::setPermissions(path, owner))
        return true;
    qWarning("Portable folder: could not restrict %s to its owner; this file system may not support "
             "permissions.", qUtf8Printable(path));
    return false;
}

QString ownerOnlyReport(const QString &path)
{
    const auto permissions = QFileInfo(path).permissions();
    int mode = 0;
    for (const auto &[flag, bit] : {std::pair{QFileDevice::ReadOwner, 0400}, {QFileDevice::WriteOwner, 0200},
                                    {QFileDevice::ExeOwner, 0100}, {QFileDevice::ReadGroup, 040},
                                    {QFileDevice::WriteGroup, 020}, {QFileDevice::ExeGroup, 010},
                                    {QFileDevice::ReadOther, 04}, {QFileDevice::WriteOther, 02},
                                    {QFileDevice::ExeOther, 01}})
        mode |= permissions.testFlag(flag) ? bit : 0;
    return QString::number(mode, 8);
}
