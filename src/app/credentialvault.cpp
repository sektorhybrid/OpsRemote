// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "credentialvault.h"
#include "systemcredentials.h"

#include "appdirs.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSaveFile>

#include <sodium.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>

namespace {
constexpr char VaultMagic[] = "PTVAULT1";
constexpr qsizetype VaultMagicSize = sizeof(VaultMagic) - 1;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}
}

CredentialVault::CredentialVault(QString filePath)
    : m_filePath(std::move(filePath))
{
    const int sodiumStatus = sodium_init();
    if (sodiumStatus < 0)
        m_initializationError = true;
}

CredentialVault::~CredentialVault()
{
    lock();
}

bool CredentialVault::exists() const
{
    return QFile::exists(m_filePath);
}

bool CredentialVault::isUnlocked() const
{
    return m_unlocked;
}

bool CredentialVault::initialize(const QString &masterPassword, QString *error)
{
    if (m_initializationError) {
        setError(error, QStringLiteral("Could not initialize the cryptographic library."));
        return false;
    }
    if (exists()) {
        setError(error, QStringLiteral("A credential vault already exists."));
        return false;
    }
    if (masterPassword.size() < MinimumPasswordLength) {
        setError(error, QStringLiteral("Use a master password with at least %1 characters.")
                            .arg(MinimumPasswordLength));
        return false;
    }

    randombytes_buf(m_salt.data(), m_salt.size());
    if (!deriveKey(masterPassword, error))
        return false;
    m_passwords.clear();
    m_unlocked = true;
    if (!saveEncryptedData(error)) {
        lock();
        return false;
    }
    return true;
}

bool CredentialVault::unlock(const QString &masterPassword, QString *error)
{
    if (m_initializationError) {
        setError(error, QStringLiteral("Could not initialize the cryptographic library."));
        return false;
    }
    if (!exists()) {
        setError(error, QStringLiteral("No credential vault exists yet."));
        return false;
    }
    lock();
    return loadEncryptedData(masterPassword, error);
}

void CredentialVault::setStoreRule(std::function<PasswordStore(const QString &key)> rule)
{
    m_storeRule = std::move(rule);
}

PasswordStore CredentialVault::storeFor(const QString &key) const
{
    return m_storeRule ? m_storeRule(key) : PasswordStore::Vault;
}

bool CredentialVault::needsUnlock(const QString &key) const
{
    return needsVault(key) && exists() && !m_unlocked;
}

QStringList CredentialVault::vaultKeys() const
{
    return m_unlocked ? m_passwords.keys() : QStringList();
}

QString CredentialVault::vaultPassword(const QString &key) const
{
    return m_unlocked ? m_passwords.value(key) : QString();
}

bool CredentialVault::removePassword(const QString &sessionId, QString *error)
{
    // Gone from the system store too, when it was used for this entry.
    if (storeFor(sessionId) != PasswordStore::Vault && SystemCredentials::isAvailable()
        && !SystemCredentials::remove(sessionId, error))
        return false;
    if (!needsVault(sessionId) && !m_unlocked)
        return true;
    if (!m_unlocked) {
        setError(error, QStringLiteral("Unlock the credential vault first."));
        return false;
    }
    if (!m_passwords.contains(sessionId))
        return true;
    const auto previous = m_passwords.take(sessionId);
    if (!saveEncryptedData(error)) {
        m_passwords.insert(sessionId, previous);
        return false;
    }
    return true;
}

bool CredentialVault::setPassword(const QString &sessionId, const QString &password,
                                  QString *error)
{
    const PasswordStore store = storeFor(sessionId);
    if (store != PasswordStore::Vault && !SystemCredentials::write(sessionId, password, error))
        return false;
    if (store == PasswordStore::System) {
        // Only one copy: an older one in the vault goes.
        if (m_unlocked && m_passwords.contains(sessionId)) {
            m_passwords.remove(sessionId);
            saveEncryptedData(nullptr);
        }
        return true;
    }
    return setVaultPassword(sessionId, password, error);
}

bool CredentialVault::setVaultPassword(const QString &sessionId, const QString &password,
                                       QString *error)
{
    if (!m_unlocked) {
        setError(error, QStringLiteral("Unlock the credential vault first."));
        return false;
    }
    const bool hadPrevious = m_passwords.contains(sessionId);
    const auto previous = m_passwords.value(sessionId);
    m_passwords.insert(sessionId, password);
    if (!saveEncryptedData(error)) {
        if (hadPrevious)
            m_passwords.insert(sessionId, previous);
        else
            m_passwords.remove(sessionId);
        return false;
    }
    return true;
}

QString CredentialVault::passwordForSession(const QString &sessionId) const
{
    const PasswordStore store = storeFor(sessionId);
    if (store != PasswordStore::Vault) {
        const QString stored = SystemCredentials::read(sessionId);
        if (!stored.isNull() || store == PasswordStore::System)
            return stored;
    }
    return vaultPassword(sessionId);
}

bool CredentialVault::changeMasterPassword(const QString &currentPassword,
                                           const QString &newPassword, QString *error)
{
    if (newPassword.size() < MinimumPasswordLength) {
        setError(error, QStringLiteral("Use a master password with at least %1 characters.")
                            .arg(MinimumPasswordLength));
        return false;
    }
    if (!unlock(currentPassword, error))
        return false;
    const auto previousSalt = m_salt;
    const auto previousKey = m_key;
    randombytes_buf(m_salt.data(), m_salt.size());
    if (!deriveKey(newPassword, error) || !saveEncryptedData(error)) {
        m_salt = previousSalt;
        m_key = previousKey;
        return false;
    }
    return true;
}

void CredentialVault::lock()
{
    sodium_memzero(m_key.data(), m_key.size());
    for (auto iterator = m_passwords.begin(); iterator != m_passwords.end(); ++iterator)
        iterator.value().fill(QChar(0));
    m_passwords.clear();
    m_unlocked = false;
}

bool CredentialVault::deriveKey(const QString &masterPassword, QString *error)
{
    QByteArray passwordBytes = masterPassword.toUtf8();
    const int result = crypto_pwhash(m_key.data(), m_key.size(),
                                     passwordBytes.constData(),
                                     static_cast<unsigned long long>(passwordBytes.size()),
                                     m_salt.data(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                     crypto_pwhash_MEMLIMIT_INTERACTIVE,
                                     crypto_pwhash_ALG_ARGON2ID13);
    if (!passwordBytes.isEmpty())
        sodium_memzero(passwordBytes.data(), static_cast<size_t>(passwordBytes.size()));
    if (result != 0) {
        sodium_memzero(m_key.data(), m_key.size());
        setError(error, QStringLiteral("Could not derive the vault encryption key."));
        return false;
    }
    return true;
}

bool CredentialVault::loadEncryptedData(const QString &masterPassword, QString *error)
{
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return false;
    }
    const QByteArray data = file.readAll();
    const qsizetype headerSize = VaultMagicSize + crypto_pwhash_SALTBYTES
                                 + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    if (data.size() < headerSize + crypto_aead_xchacha20poly1305_ietf_ABYTES
        || data.first(VaultMagicSize) != QByteArray(VaultMagic, VaultMagicSize)) {
        setError(error, QStringLiteral("The credential vault file is invalid."));
        return false;
    }

    qsizetype offset = VaultMagicSize;
    std::copy_n(reinterpret_cast<const unsigned char *>(data.constData() + offset),
                m_salt.size(), m_salt.data());
    offset += m_salt.size();
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    std::copy_n(reinterpret_cast<const unsigned char *>(data.constData() + offset),
                nonce.size(), nonce.data());
    offset += nonce.size();

    if (!deriveKey(masterPassword, error))
        return false;

    const auto *ciphertext = reinterpret_cast<const unsigned char *>(data.constData() + offset);
    const auto ciphertextSize = static_cast<unsigned long long>(data.size() - offset);
    QByteArray plaintext(static_cast<qsizetype>(ciphertextSize
                              - crypto_aead_xchacha20poly1305_ietf_ABYTES), Qt::Uninitialized);
    unsigned long long plaintextSize = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plaintext.data()), &plaintextSize, nullptr,
            ciphertext, ciphertextSize,
            reinterpret_cast<const unsigned char *>(VaultMagic), VaultMagicSize,
            nonce.data(), m_key.data()) != 0) {
        if (!plaintext.isEmpty())
            sodium_memzero(plaintext.data(), static_cast<size_t>(plaintext.size()));
        sodium_memzero(m_key.data(), m_key.size());
        setError(error, QStringLiteral("The master password is incorrect or the vault is damaged."));
        return false;
    }

    try {
        const auto document = YAML::Load(std::string(plaintext.constData(),
                                                     static_cast<size_t>(plaintextSize)));
        if (!document.IsMap())
            throw YAML::Exception(YAML::Mark::null_mark(), "expected a credential map");
        for (const auto &entry : document) {
            const auto id = QString::fromStdString(entry.first.as<std::string>());
            const auto secret = QString::fromStdString(entry.second.as<std::string>());
            m_passwords.insert(id, secret);
        }
    } catch (const YAML::Exception &) {
        if (!plaintext.isEmpty())
            sodium_memzero(plaintext.data(), static_cast<size_t>(plaintext.size()));
        sodium_memzero(m_key.data(), m_key.size());
        m_passwords.clear();
        setError(error, QStringLiteral("The decrypted credential data is invalid."));
        return false;
    }
    if (!plaintext.isEmpty())
        sodium_memzero(plaintext.data(), static_cast<size_t>(plaintext.size()));
    m_unlocked = true;
    return true;
}

bool CredentialVault::saveEncryptedData(QString *error)
{
    if (!m_unlocked) {
        setError(error, QStringLiteral("The credential vault is locked."));
        return false;
    }

    YAML::Node document(YAML::NodeType::Map);
    for (auto iterator = m_passwords.cbegin(); iterator != m_passwords.cend(); ++iterator)
        document[iterator.key().toStdString()] = iterator.value().toStdString();
    YAML::Emitter emitter;
    emitter << document;
    QByteArray plaintext(emitter.c_str());

    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
    randombytes_buf(nonce.data(), nonce.size());
    QByteArray ciphertext(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES,
                          Qt::Uninitialized);
    unsigned long long ciphertextSize = 0;
    const int encrypted = crypto_aead_xchacha20poly1305_ietf_encrypt(
        reinterpret_cast<unsigned char *>(ciphertext.data()), &ciphertextSize,
        reinterpret_cast<const unsigned char *>(plaintext.constData()),
        static_cast<unsigned long long>(plaintext.size()),
        reinterpret_cast<const unsigned char *>(VaultMagic), VaultMagicSize,
        nullptr, nonce.data(), m_key.data());
    if (!plaintext.isEmpty())
        sodium_memzero(plaintext.data(), static_cast<size_t>(plaintext.size()));
    if (encrypted != 0) {
        setError(error, QStringLiteral("Could not encrypt the credential vault."));
        return false;
    }
    ciphertext.resize(static_cast<qsizetype>(ciphertextSize));

    QByteArray contents(VaultMagic, VaultMagicSize);
    contents.append(reinterpret_cast<const char *>(m_salt.data()), m_salt.size());
    contents.append(reinterpret_cast<const char *>(nonce.data()), nonce.size());
    contents.append(ciphertext);

    if (!QDir().mkpath(QFileInfo(m_filePath).absolutePath())) {
        setError(error, QStringLiteral("Could not create the credential vault directory."));
        return false;
    }
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(contents) != contents.size()
        || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    restrictToOwner(m_filePath);
    return true;
}
