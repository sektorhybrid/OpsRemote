// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"

#include <QDir>
#include <QFileInfo>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>

#include <memory>
#include <string>

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

// QFile::setPermissions only sets the read-only flag on Windows: an access
// list for this user alone, protected from the folder's, does the job.
bool restrictToOwner(const QString &path)
{
    const auto warn = [&path](unsigned long code) {
        qWarning("Portable folder: could not restrict %s to its owner (error %lu); this file system may not "
                 "support permissions.", qUtf8Printable(QDir::toNativeSeparators(path)), code);
        return false;
    };
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return warn(GetLastError());
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::unique_ptr<unsigned char[]> user(new unsigned char[size ? size : 1]);
    const bool known = size && GetTokenInformation(token, TokenUser, user.get(), size, &size);
    CloseHandle(token);
    if (!known)
        return warn(GetLastError());
    const bool folder = QFileInfo(path).isDir();
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = folder ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(reinterpret_cast<TOKEN_USER *>(user.get())->User.Sid);
    PACL list = nullptr;
    if (const DWORD made = SetEntriesInAclW(1, &access, nullptr, &list); made != ERROR_SUCCESS)
        return warn(made);
    std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD set = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                            nullptr, nullptr, list, nullptr);
    LocalFree(list);
    return set == ERROR_SUCCESS ? true : warn(set);
}

QString ownerOnlyReport(const QString &path)
{
    std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    PACL list = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &list,
                              nullptr, &descriptor) != ERROR_SUCCESS)
        return QStringLiteral("unknown");
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    GetSecurityDescriptorControl(descriptor, &control, &revision);
    HANDLE token = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::unique_ptr<unsigned char[]> user(new unsigned char[size ? size : 1]);
    GetTokenInformation(token, TokenUser, user.get(), size, &size);
    CloseHandle(token);
    const PSID self = reinterpret_cast<TOKEN_USER *>(user.get())->User.Sid;
    int others = 0;
    for (DWORD index = 0; list && index < list->AceCount; ++index) {
        void *entry = nullptr;
        if (GetAce(list, index, &entry)
            && static_cast<ACE_HEADER *>(entry)->AceType == ACCESS_ALLOWED_ACE_TYPE
            && !EqualSid(&static_cast<ACCESS_ALLOWED_ACE *>(entry)->SidStart, self))
            ++others;
    }
    LocalFree(descriptor);
    return QStringLiteral("protected %1, others %2").arg((control & SE_DACL_PROTECTED) ? 1 : 0).arg(others);
}
