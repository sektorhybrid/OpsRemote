// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <libssh/libssh.h>

// An OpenSSH certificate (key-cert.pub), as signed by a certificate
// authority: who it is for and how long it is valid.
struct SshCertificateInfo
{
    bool valid = false; // false: not a certificate (see error)
    QString error;
    QString keyType; // e.g. ssh-ed25519-cert-v01@openssh.com
    bool userCertificate = true; // false: a host certificate
    QString keyId;
    quint64 serial = 0;
    QStringList principals; // empty: any user name
    QDateTime validAfter; // invalid: always
    QDateTime validBefore; // invalid: forever

    bool expired() const;
    bool notYetValid() const;
    // "for root, admin; valid until 5 Nov 2026 14:00" and the like.
    QString summary() const;
};

SshCertificateInfo readSshCertificate(const QString &path);

// The certificate OpenSSH looks for next to a private key: key-cert.pub.
QString defaultCertificateFor(const QString &identityFile);

// Why FIDO2 security keys cannot be used here; empty when they can.
QString securityKeyProblem();
// libssh's ssh_sk_resident_keys_load, which only libssh built with FIDO2
// has; null without it.
using ResidentKeysLoader = int (*)(const struct ssh_pki_ctx_struct *, ssh_key **, size_t *);
ResidentKeysLoader residentKeysLoader();
// Why Kerberos (GSSAPI) logons cannot be used here; empty when they can.
QString kerberosProblem();
// Kerberos tickets are listed by klist and obtained by kinit (MIT or
// Heimdal); empty when the program is not installed.
QString klistProgram();
QString kinitProgram();
// What klist printed, as "user@REALM, valid until …", or why there is no
// ticket. ok is set when there is a ticket.
QString kerberosTicketSummary(const QByteArray &klistOutput, int exitCode, bool *ok = nullptr);
