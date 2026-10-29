// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "preferences.h"

#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
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
    int autoReconnect = -1;
    int reconnectDelay = 0; // seconds
    int reconnectAttempts = -1;
    QString terminalType;
    int followTerminalFolder = -1;
    int forwardX11 = -1;
    QString fileTransfer; // auto, sftp, scp

    bool isEmpty() const { return *this == SshOptions(); }
    SshPreferences appliedTo(SshPreferences preferences) const;
    bool operator==(const SshOptions &) const = default;
};

SshOptions sshOptionsFromYaml(const YAML::Node &node);
void sshOptionsToYaml(const SshOptions &options, YAML::Node &node);

// Edits a session's SshOptions; every field starts at "Default (…)".
class SshOptionsEditor final : public QWidget
{
    Q_OBJECT

public:
    SshOptionsEditor(const SshOptions &value, const SshPreferences &defaults,
                     QWidget *parent = nullptr);

    SshOptions value() const;

private:
    SshPreferences m_defaults;
    QSpinBox *m_connectTimeout = nullptr;
    QSpinBox *m_keepalive = nullptr;
    QCheckBox *m_compression = nullptr;
    QCheckBox *m_tryPublicKeys = nullptr;
    QCheckBox *m_forwardAgent = nullptr;
    QLineEdit *m_identityFile = nullptr;
    QCheckBox *m_autoReconnect = nullptr;
    QSpinBox *m_reconnectDelay = nullptr;
    QSpinBox *m_reconnectAttempts = nullptr;
    QComboBox *m_terminalType = nullptr;
    QCheckBox *m_followTerminalFolder = nullptr;
    QCheckBox *m_forwardX11 = nullptr;
    QComboBox *m_fileTransfer = nullptr;
};

// The file browser's protocols for SSH: value and label.
QList<QPair<QString, QString>> sshFileTransferChoices();
