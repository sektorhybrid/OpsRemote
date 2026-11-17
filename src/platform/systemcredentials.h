// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

// The operating system's credential store: Windows Credential Manager, or on
// Linux the Secret Service (GNOME Keyring, KWallet, KeePassXC...) through
// libsecret, loaded when first used so the portable package does not need
// it. Entries are per user and per computer; they do not travel with the
// portable folder. Keys are the vault's (session ids, "key:<path>", ...).
namespace SystemCredentials {

// Whether there is a store to use; *why says what is missing.
bool isAvailable(QString *why = nullptr);
// The stored password, or a null QString when there is none.
QString read(const QString &key, QString *error = nullptr);
bool write(const QString &key, const QString &password, QString *error = nullptr);
// True also when there was nothing to remove.
bool remove(const QString &key, QString *error = nullptr);
// "Windows Credential Manager" or "the system keyring".
QString name();

}
