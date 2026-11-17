// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"

#include <QDir>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

QString portableHomeBeforeStart()
{
    return portableHomeFor(QString());
}

bool replaceFile(const QString &from, const QString &to, QString *error)
{
    if (MoveFileExW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(from).utf16()),
                    reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(to).utf16()),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    if (error)
        *error = QStringLiteral("error %1").arg(GetLastError());
    return false;
}
