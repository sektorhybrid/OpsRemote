// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "systemcredentials.h"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
#else
#include <dlfcn.h>
#endif

#include <string>

namespace {

void fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

}

#ifdef Q_OS_WIN

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

#else

namespace SystemCredentials {

namespace {

// libsecret's simple password API and the structures it takes (stable ABI).
struct SchemaAttribute
{
    const char *name;
    int type; // 0: string
};

struct Schema
{
    const char *name;
    int flags;
    SchemaAttribute attributes[32];
    int reserved;
    void *reserved1, *reserved2, *reserved3, *reserved4, *reserved5, *reserved6, *reserved7;
};

struct GlibError
{
    quint32 domain;
    int code;
    char *message;
};

using StoreFunction = int (*)(const Schema *, const char *, const char *, const char *, void *,
                              GlibError **, ...);
using LookupFunction = char *(*)(const Schema *, void *, GlibError **, ...);
using ClearFunction = int (*)(const Schema *, void *, GlibError **, ...);
using FreeFunction = void (*)(char *);
using ErrorFreeFunction = void (*)(GlibError *);

struct Library
{
    StoreFunction store = nullptr;
    LookupFunction lookup = nullptr;
    ClearFunction clear = nullptr;
    FreeFunction free = nullptr;
    ErrorFreeFunction errorFree = nullptr;
    bool loaded() const { return store && lookup && clear && free && errorFree; }
};

const Library &library()
{
    static const Library loaded = [] {
        Library result;
        void *secret = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        void *glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!secret || !glib)
            return result;
        result.store = reinterpret_cast<StoreFunction>(dlsym(secret, "secret_password_store_sync"));
        result.lookup = reinterpret_cast<LookupFunction>(dlsym(secret, "secret_password_lookup_sync"));
        result.clear = reinterpret_cast<ClearFunction>(dlsym(secret, "secret_password_clear_sync"));
        result.free = reinterpret_cast<FreeFunction>(dlsym(secret, "secret_password_free"));
        result.errorFree = reinterpret_cast<ErrorFreeFunction>(dlsym(glib, "g_error_free"));
        return result;
    }();
    return loaded;
}

const Schema *schema()
{
    static const Schema value{"org.opsremote.Password", 0, {{"key", 0}, {nullptr, 0}}, 0,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &value;
}

bool check(GlibError *glibError, QString *error)
{
    if (!glibError)
        return true;
    fail(error, QStringLiteral("System keyring: %1").arg(QString::fromUtf8(glibError->message)));
    library().errorFree(glibError);
    return false;
}

}

QString name()
{
    return QStringLiteral("the system keyring");
}

bool isAvailable(QString *why)
{
    if (library().loaded())
        return true;
    fail(why, QStringLiteral("libsecret is not installed (the package libsecret, with GNOME Keyring, "
                             "KWallet, or KeePassXC as the keyring)."));
    return false;
}

QString read(const QString &key, QString *error)
{
    if (!isAvailable(error))
        return {};
    GlibError *glibError = nullptr;
    const QByteArray name = key.toUtf8();
    char *secret = library().lookup(schema(), nullptr, &glibError, "key", name.constData(), nullptr);
    if (!check(glibError, error) || !secret)
        return {};
    const QString password = QString::fromUtf8(secret);
    library().free(secret); // also wipes it
    return password.isNull() ? QString(QLatin1String("")) : password;
}

bool write(const QString &key, const QString &password, QString *error)
{
    if (!isAvailable(error))
        return false;
    GlibError *glibError = nullptr;
    const QByteArray name = key.toUtf8();
    QByteArray secret = password.toUtf8();
    const QByteArray label = QStringLiteral("OpsRemote: %1").arg(key).toUtf8();
    const int stored = library().store(schema(), "default", label.constData(), secret.constData(),
                                       nullptr, &glibError, "key", name.constData(), nullptr);
    secret.fill('\0');
    return check(glibError, error) && stored;
}

bool remove(const QString &key, QString *error)
{
    if (!isAvailable(error))
        return false;
    GlibError *glibError = nullptr;
    const QByteArray name = key.toUtf8();
    library().clear(schema(), nullptr, &glibError, "key", name.constData(), nullptr);
    return check(glibError, error);
}

}

#endif
