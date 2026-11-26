// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "reconnect.h"

#include <QWidget>

#include <memory>

namespace YAML { class Node; }

// Telnet options. The Preferences window keeps the defaults (settings.yaml
// "telnet:"); a session may override them (empty fields: the default).
struct TelnetOptions
{
    // Telnet cannot tell a logout from a reset (both close the connection):
    // only errors reconnect, and keepalive probes make a dead network one.
    ReconnectOptions reconnect;

    bool isEmpty() const { return *this == TelnetOptions(); }
    TelnetOptions overriddenBy(const TelnetOptions &session) const;
    bool operator==(const TelnetOptions &) const = default;

    static TelnetOptions builtIn();
};

TelnetOptions telnetOptionsFromYaml(const YAML::Node &node);
void telnetOptionsToYaml(const TelnetOptions &options, YAML::Node &node);
TelnetOptions loadTelnetDefaults();
void saveTelnetDefaults(const TelnetOptions &options);

// Edits TelnetOptions for a session (the Connection tab of a Telnet session).
class TelnetOptionsEditor final : public QWidget
{
    Q_OBJECT

public:
    TelnetOptionsEditor(const TelnetOptions &value, const TelnetOptions &defaults, QWidget *parent = nullptr);
    ~TelnetOptionsEditor() override;

    TelnetOptions value() const;

private:
    std::unique_ptr<ReconnectEditorRows> m_reconnect;
};
