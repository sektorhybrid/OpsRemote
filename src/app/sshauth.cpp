// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sshauth.h"

#include <QFile>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
// Reads the SSH wire format (RFC 4251): uint32, uint64, and strings with a
// uint32 length; ok turns false past the end.
class WireReader
{
public:
    explicit WireReader(const QByteArray &data) : m_data(data) {}

    bool ok() const { return m_ok; }

    quint32 uint32()
    {
        if (!need(4))
            return 0;
        const auto value = qFromBigEndian<quint32>(m_data.constData() + m_position);
        m_position += 4;
        return value;
    }

    quint64 uint64()
    {
        if (!need(8))
            return 0;
        const auto value = qFromBigEndian<quint64>(m_data.constData() + m_position);
        m_position += 8;
        return value;
    }

    QByteArray string()
    {
        const quint32 length = uint32();
        if (!m_ok || !need(length))
            return {};
        const auto value = m_data.mid(m_position, length);
        m_position += length;
        return value;
    }

private:
    bool need(qsizetype count)
    {
        if (m_data.size() - m_position < count)
            m_ok = false;
        return m_ok;
    }

    QByteArray m_data;
    qsizetype m_position = 0;
    bool m_ok = true;
};

// How many strings (or mpints) the public key has between the nonce and the
// serial number, by certificate type (OpenSSH PROTOCOL.certkeys).
int publicKeyFields(const QByteArray &type)
{
    if (type.startsWith("ssh-rsa-"))
        return 2; // e, n
    if (type.startsWith("ssh-dss-"))
        return 4; // p, q, g, y
    if (type.startsWith("ecdsa-sha2-"))
        return 2; // curve, point
    if (type.startsWith("ssh-ed25519-"))
        return 1; // key
    if (type.startsWith("sk-ecdsa-sha2-"))
        return 3; // curve, point, application
    if (type.startsWith("sk-ssh-ed25519-"))
        return 2; // key, application
    return -1;
}

QDateTime certificateTime(quint64 seconds)
{
    if (seconds == 0 || seconds == ~quint64(0))
        return {};
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(qMin<quint64>(seconds, 253402300799)));
}
}

bool SshCertificateInfo::expired() const
{
    return validBefore.isValid() && validBefore < QDateTime::currentDateTime();
}

bool SshCertificateInfo::notYetValid() const
{
    return validAfter.isValid() && validAfter > QDateTime::currentDateTime();
}

QString SshCertificateInfo::summary() const
{
    if (!valid)
        return error;
    const QLocale locale;
    QStringList parts;
    parts.append(principals.isEmpty() ? QStringLiteral("for any user")
                                      : QStringLiteral("for %1").arg(principals.join(QStringLiteral(", "))));
    if (expired())
        parts.append(QStringLiteral("expired on %1").arg(locale.toString(validBefore, QLocale::ShortFormat)));
    else if (notYetValid())
        parts.append(QStringLiteral("valid from %1").arg(locale.toString(validAfter, QLocale::ShortFormat)));
    else if (validBefore.isValid())
        parts.append(QStringLiteral("valid until %1").arg(locale.toString(validBefore, QLocale::ShortFormat)));
    else
        parts.append(QStringLiteral("does not expire"));
    if (!keyId.isEmpty())
        parts.append(QStringLiteral("ID %1").arg(keyId));
    return parts.join(QStringLiteral("; "));
}

SshCertificateInfo readSshCertificate(const QString &path)
{
    SshCertificateInfo info;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        info.error = QStringLiteral("Cannot read %1.").arg(path);
        return info;
    }
    // "type base64 comment", as in a .pub file.
    const auto fields = file.read(64 * 1024).simplified().split(' ');
    const auto blob = fields.size() >= 2 ? QByteArray::fromBase64(fields[1]) : QByteArray();
    WireReader reader(blob);
    const auto type = reader.string();
    const int keyFields = publicKeyFields(type);
    if (!reader.ok() || !type.contains("-cert-v01@openssh.com") || keyFields < 0) {
        info.error = QStringLiteral("Not an SSH certificate.");
        return info;
    }
    reader.string(); // nonce
    for (int field = 0; field < keyFields; ++field)
        reader.string();
    info.serial = reader.uint64();
    info.userCertificate = reader.uint32() == 1;
    info.keyId = QString::fromUtf8(reader.string());
    WireReader principals(reader.string());
    info.validAfter = certificateTime(reader.uint64());
    info.validBefore = certificateTime(reader.uint64());
    if (!reader.ok()) {
        info.error = QStringLiteral("The certificate is damaged.");
        return info;
    }
    for (;;) {
        const auto name = principals.string();
        if (!principals.ok())
            break;
        info.principals.append(QString::fromUtf8(name));
    }
    info.keyType = QString::fromLatin1(type);
    info.valid = true;
    return info;
}

QString defaultCertificateFor(const QString &identityFile)
{
    if (identityFile.isEmpty())
        return {};
    auto base = identityFile;
    if (base.endsWith(QStringLiteral(".pub")))
        base.chop(4);
    return base + QStringLiteral("-cert.pub");
}

QString securityKeyProblem()
{
    // Without FIDO2 support in libssh, every security key call fails.
    ssh_pki_ctx context = ssh_pki_ctx_new();
    const bool supported = context && ssh_pki_ctx_set_sk_pin_callback(context, nullptr, nullptr) == SSH_OK;
    ssh_pki_ctx_free(context);
    if (!supported || !residentKeysLoader())
        return QStringLiteral("This copy of OpsRemote was built with an SSH library (libssh) "
                              "without FIDO2 support, so security keys cannot be used.");
    return {};
}

ResidentKeysLoader residentKeysLoader()
{
#ifdef Q_OS_WIN
    const HMODULE libssh = GetModuleHandleW(L"libssh.dll");
    return libssh ? reinterpret_cast<ResidentKeysLoader>(
                        reinterpret_cast<void *>(GetProcAddress(libssh, "ssh_sk_resident_keys_load")))
                  : nullptr;
#else
    return reinterpret_cast<ResidentKeysLoader>(dlsym(RTLD_DEFAULT, "ssh_sk_resident_keys_load"));
#endif
}

QString kerberosProblem()
{
#ifdef Q_OS_WIN
    return QStringLiteral("Kerberos logons are not available on Windows yet: the SSH library "
                          "(libssh) needs MIT Kerberos, which has no build for OpsRemote's "
                          "Windows package.");
#else
    // Only libssh built with GSSAPI knows this option (another library in
    // the process may load GSSAPI, so its symbols prove nothing).
    ssh_session session = ssh_new();
    const bool off = false;
    const bool supported = session && ssh_options_set(session, SSH_OPTIONS_GSSAPI_KEY_EXCHANGE, &off) == 0;
    ssh_free(session);
    if (!supported)
        return QStringLiteral("This copy of OpsRemote was built with an SSH library (libssh) "
                              "without Kerberos (GSSAPI) support.");
    return {};
#endif
}

QString klistProgram()
{
    return QStandardPaths::findExecutable(QStringLiteral("klist"));
}

QString kinitProgram()
{
    return QStandardPaths::findExecutable(QStringLiteral("kinit"));
}

QString kerberosTicketSummary(const QByteArray &klistOutput, int exitCode, bool *ok)
{
    if (ok)
        *ok = false;
    const auto text = QString::fromLocal8Bit(klistOutput);
    static const QRegularExpression principalLine(QStringLiteral("^\\s*(?:Default p|P)rincipal:\\s*(\\S+)"),
                                                  QRegularExpression::MultilineOption);
    const auto principal = principalLine.match(text);
    if (exitCode != 0 || !principal.hasMatch())
        return QStringLiteral("No Kerberos ticket. Get one with Get Ticket…, or log on to the "
                              "domain.");
    // The ticket-granting ticket's line: "start  expiry  krbtgt/REALM@REALM"
    // (MIT), or "start  expiry  krbtgt/REALM@REALM" after "Issued Expires
    // Principal" (Heimdal); the dates are in the system's format.
    QString expires;
    for (const auto &line : text.split(QLatin1Char('\n'))) {
        if (!line.contains(QStringLiteral("krbtgt/")))
            continue;
        const auto columns = line.trimmed().split(QRegularExpression(QStringLiteral("\\s{2,}")));
        if (columns.size() >= 3)
            expires = columns[1];
        break;
    }
    if (ok)
        *ok = true;
    return expires.isEmpty() ? principal.captured(1)
                             : QStringLiteral("%1, valid until %2").arg(principal.captured(1), expires);
}
