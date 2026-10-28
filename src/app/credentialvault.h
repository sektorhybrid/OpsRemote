// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QHash>
#include <QString>

#include <array>

class CredentialVault final
{
public:
    static constexpr int MinimumPasswordLength = 8;

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
    bool m_initializationError = false;
};
