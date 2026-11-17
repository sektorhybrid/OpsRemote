// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// Windows Credential Manager.

#include "systemcredentials.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#include <string>

namespace {

void fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

}

namespace SystemCredentials {

namespace {

std::wstring target(const QString &key)
{
    return (QStringLiteral("OpsRemote/") + key).toStdWString();
}

}

QString name()
{
    return QStringLiteral("Windows Credential Manager");
}

bool isAvailable(QString *)
{
    return true;
}

QString read(const QString &key, QString *error)
{
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target(key).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        if (GetLastError() != ERROR_NOT_FOUND)
            fail(error, QStringLiteral("Windows Credential Manager error %1").arg(GetLastError()));
        return {};
    }
    const QString password = QString::fromUtf16(
        reinterpret_cast<const char16_t *>(credential->CredentialBlob),
        qsizetype(credential->CredentialBlobSize / sizeof(char16_t)));
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return password.isNull() ? QString(QLatin1String("")) : password;
}

bool write(const QString &key, const QString &password, QString *error)
{
    std::wstring name = target(key);
    std::wstring blob = password.toStdWString();
    std::wstring user = L"OpsRemote";
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = name.data();
    credential.CredentialBlobSize = DWORD(blob.size() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = user.data();
    const bool written = CredWriteW(&credential, 0);
    SecureZeroMemory(blob.data(), blob.size() * sizeof(wchar_t));
    if (!written)
        fail(error, QStringLiteral("Windows Credential Manager error %1").arg(GetLastError()));
    return written;
}

bool remove(const QString &key, QString *error)
{
    if (CredDeleteW(target(key).c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND)
        return true;
    fail(error, QStringLiteral("Windows Credential Manager error %1").arg(GetLastError()));
    return false;
}

}
