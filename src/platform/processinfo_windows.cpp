// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "processinfo.h"

#include <QDir>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <iterator>

namespace ProcessInfo {

QString workingFolder(int)
{
    return {}; // Windows keeps it in the process's own memory.
}

QString commandLine(int)
{
    return {};
}

bool otherProcessRunsFrom(const QString &folder)
{
    const QString prefix = QDir::cleanPath(folder) + QLatin1Char('/');
    bool found = false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return true; // Unknown: be safe.
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found;
         more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId())
            continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        wchar_t path[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            const auto image = QDir::fromNativeSeparators(QString::fromWCharArray(path, int(size)));
            found = image.startsWith(prefix, Qt::CaseInsensitive);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}

}
