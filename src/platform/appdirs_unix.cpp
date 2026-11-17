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
