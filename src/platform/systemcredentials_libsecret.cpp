// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// The Secret Service through libsecret, loaded when first used.

#include "systemcredentials.h"

#include <dlfcn.h>

namespace {

void fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

}

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
    QString problem; // why it could not be loaded (dlerror), if so
};

const Library &library()
{
    static const Library loaded = [] {
        Library result;
        void *secret = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!secret)
            result.problem = QString::fromLocal8Bit(dlerror());
        void *glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!glib && result.problem.isEmpty())
            result.problem = QString::fromLocal8Bit(dlerror());
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
    // Not found at all, or found but not loadable (the reason, e.g. a
    // library version mismatch, helps more than "not installed").
    const auto &problem = library().problem;
    fail(why, problem.contains(QStringLiteral("cannot open shared object file"))
                  ? QStringLiteral("libsecret is not installed (the package libsecret, with GNOME "
                                   "Keyring, KWallet, or KeePassXC as the keyring).")
                  : QStringLiteral("libsecret could not be loaded: %1").arg(problem));
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
