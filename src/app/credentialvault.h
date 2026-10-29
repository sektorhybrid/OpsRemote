// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <array>
#include <functional>

// The vault entry for a private key's passphrase; keyPath as stored in the
// settings (relative to the program's folder, or absolute).
inline QString keyPassphraseVaultKey(const QString &keyPath)
{
    return QStringLiteral("key:") + keyPath;
}

// Where a password lives: this encrypted vault file (portable), the
// operating system's credential store (this computer only), or both
// (written to both, read from the system first).
enum class PasswordStore { Vault, System, Both };

class CredentialVault final
{
public:
    static constexpr int MinimumPasswordLength = 8;

    // Which store each entry uses (by default the vault); set by MainWindow
    // from the Preferences and each session's own choice.
    void setStoreRule(std::function<PasswordStore(const QString &key)> rule);
    PasswordStore storeFor(const QString &key) const;
    // Whether using this entry needs the master password now.
    bool needsUnlock(const QString &key) const;
    bool needsVault(const QString &key) const { return storeFor(key) != PasswordStore::System; }
    // The vault's own entries, for copying between the stores.
    QStringList vaultKeys() const;
    QString vaultPassword(const QString &key) const;
    bool setVaultPassword(const QString &key, const QString &password, QString *error = nullptr);

    explicit CredentialVault(QString filePath);
    ~CredentialVault();

    CredentialVault(const CredentialVault &) = delete;
    CredentialVault &operator=(const CredentialVault &) = delete;

    bool exists() const;
    bool isUnlocked() const;
    bool initialize(const QString &masterPassword, QString *error = nullptr);
    bool unlock(const QString &masterPassword, QString *error = nullptr);
    bool setPassword(const QString &sessionId, const QString &password,
                     QString *error = nullptr);
    QString passwordForSession(const QString &sessionId) const;
    // Forgets an entry; true if there was none.
    bool removePassword(const QString &sessionId, QString *error = nullptr);
    // Re-encrypts the vault under a new master password (new salt and key).
    bool changeMasterPassword(const QString &currentPassword, const QString &newPassword,
                              QString *error = nullptr);
    void lock();

private:
    bool deriveKey(const QString &masterPassword, QString *error);
    bool loadEncryptedData(const QString &masterPassword, QString *error);
    bool saveEncryptedData(QString *error);

    QString m_filePath;
    std::array<unsigned char, 32> m_key{};
    std::array<unsigned char, 16> m_salt{};
    QHash<QString, QString> m_passwords;
    bool m_unlocked = false;
    std::function<PasswordStore(const QString &key)> m_storeRule;
    bool m_initializationError = false;
};
