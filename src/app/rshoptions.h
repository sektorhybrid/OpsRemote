// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "rdpoptions.h" // RdpChoices

#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
namespace YAML { class Node; }

// Rlogin and RSH options. The Preferences window keeps the defaults
// (settings.yaml "rlogin:"); a session may override them, and empty fields
// mean "use the default". The command is the RSH session's own.
struct RshOptions
{
    // The name sent as the user on this computer (the server's .rhosts or
    // hosts.equiv decide whether it is trusted); empty: this account's.
    QString localUser;
    // auto: a reserved port (below 1024) when this computer allows it, any
    // port otherwise; reserved: only a reserved one; any: never try.
    QString sourcePort;
    QString command; // RSH: what to run

    bool isEmpty() const { return *this == RshOptions(); }
    RshOptions overriddenBy(const RshOptions &session) const;
    bool operator==(const RshOptions &) const = default;

    static RshOptions builtIn();
};

RdpChoices rshSourcePortChoices();
// This computer's user name.
QString currentUserName();

RshOptions rshOptionsFromYaml(const YAML::Node &node);
void rshOptionsToYaml(const RshOptions &options, YAML::Node &node);
RshOptions loadRshDefaults();
void saveRshDefaults(const RshOptions &options);

// Edits RshOptions for a session (the Connection tab of an Rlogin or RSH
// session): the fields start at "Default (…)"; the command row is shown
// for RSH only.
class RshOptionsEditor final : public QWidget
{
    Q_OBJECT

public:
    RshOptionsEditor(const RshOptions &value, const RshOptions &defaults, QWidget *parent = nullptr);

    RshOptions value() const;
    void setCommandShown(bool shown);

private:
    QLineEdit *m_localUser = nullptr;
    QComboBox *m_sourcePort = nullptr;
    QLineEdit *m_command = nullptr;
    QLabel *m_commandLabel = nullptr;
    bool m_commandShown = true;
};
