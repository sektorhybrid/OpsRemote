// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "rdpoptions.h" // RdpChoices

#include <QString>
#include <QWidget>

class QComboBox;
class QSpinBox;
namespace YAML { class Node; }

// FTP options. The Preferences window keeps the defaults (settings.yaml
// "ftp:"); a session may override any of them, and empty fields (or -1)
// mean "use the default".
struct FtpOptions
{
    QString security; // auto, explicit, implicit, none
    QString mode; // passive, active
    QString listing; // auto (MLSD when offered), list
    int connectTimeout = -1; // seconds
    int keepaliveSeconds = -1; // 0: off

    bool isEmpty() const { return *this == FtpOptions(); }
    FtpOptions overriddenBy(const FtpOptions &session) const;
    bool operator==(const FtpOptions &) const = default;

    static FtpOptions builtIn();
};

RdpChoices ftpSecurityChoices();
RdpChoices ftpModeChoices();
RdpChoices ftpListingChoices();

FtpOptions ftpOptionsFromYaml(const YAML::Node &node);
void ftpOptionsToYaml(const FtpOptions &options, YAML::Node &node);
FtpOptions loadFtpDefaults();
void saveFtpDefaults(const FtpOptions &options);

// Edits FtpOptions for a session: every field starts at "Default (…)".
class FtpOptionsEditor final : public QWidget
{
    Q_OBJECT

public:
    FtpOptionsEditor(const FtpOptions &value, const FtpOptions &defaults, QWidget *parent = nullptr);

    FtpOptions value() const;

private:
    QComboBox *m_security = nullptr;
    QComboBox *m_mode = nullptr;
    QComboBox *m_listing = nullptr;
    QSpinBox *m_connectTimeout = nullptr;
    QSpinBox *m_keepalive = nullptr;
};
