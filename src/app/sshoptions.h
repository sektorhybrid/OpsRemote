// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "preferences.h"

#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
namespace YAML { class Node; }

// A session's own SSH connection settings, overriding the Preferences
// (SshPreferences). Empty fields, 0, or -1 mean "use the default".
struct SshOptions
{
    int connectTimeout = 0; // seconds
    int keepaliveSeconds = -1; // 0 turns keepalives off
    int compression = -1;
    int tryPublicKeys = -1;
    int forwardAgent = -1;
    QString identityFile;
    // Empty: the key's own certificate (key-cert.pub), if any; "none": no
    // certificate; otherwise a certificate file.
    QString certificateFile;
    int kerberos = -1;
    int kerberosDelegate = -1;
    QString kerberosOrder; // empty: the Preferences'
    // The server's Kerberos name when it differs from the host name.
    QString kerberosServer;
    int autoReconnect = -1;
    int reconnectDelay = 0; // seconds
    int reconnectAttempts = -1;
    QString terminalType;
    QString backspace; // empty or "del": DEL; "ctrl-h": BS
    int followTerminalFolder = -1;
    int forwardX11 = -1;
    QString fileTransfer; // auto, sftp, scp

    bool isEmpty() const { return *this == SshOptions(); }
    SshPreferences appliedTo(SshPreferences preferences) const;
    bool operator==(const SshOptions &) const = default;
};

SshOptions sshOptionsFromYaml(const YAML::Node &node);
void sshOptionsToYaml(const SshOptions &options, YAML::Node &node);

// Edits a session's SshOptions; every field starts at the inherited value.
class SshOptionsEditor final : public QWidget
{
    Q_OBJECT

public:
    SshOptionsEditor(const SshOptions &value, const SshPreferences &defaults,
                     QWidget *parent = nullptr);

    SshOptions value() const;
    // SFTP and SCP sessions have no terminal: its settings are hidden.
    void setFileOnly(bool fileOnly);

private:
    class QFormLayout *m_generalForm = nullptr;
    QList<int> m_terminalRows; // rows of m_generalForm only a terminal uses
    QWidget *m_terminalCard = nullptr;
    SshPreferences m_defaults;
    QSpinBox *m_connectTimeout = nullptr;
    QSpinBox *m_keepalive = nullptr;
    QCheckBox *m_compression = nullptr;
    QCheckBox *m_tryPublicKeys = nullptr;
    QCheckBox *m_forwardAgent = nullptr;
    QLineEdit *m_identityFile = nullptr;
    QComboBox *m_certificate = nullptr;
    QLabel *m_certificateInfo = nullptr;
    QCheckBox *m_kerberos = nullptr;
    QCheckBox *m_kerberosDelegate = nullptr;
    QLineEdit *m_kerberosServer = nullptr;
    void showCertificate();
    QCheckBox *m_autoReconnect = nullptr;
    QSpinBox *m_reconnectDelay = nullptr; // reconnect.h
    QSpinBox *m_reconnectAttempts = nullptr;
    QComboBox *m_terminalType = nullptr;
    QComboBox *m_backspace = nullptr;
    QComboBox *m_followTerminalFolder = nullptr;
    QCheckBox *m_forwardX11 = nullptr;
    QComboBox *m_fileTransfer = nullptr;
    QComboBox *m_kerberosOrder = nullptr;
};

// The file browser's protocols for SSH: value and label.
QList<QPair<QString, QString>> sshFileTransferChoices();
