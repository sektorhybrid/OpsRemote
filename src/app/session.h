// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "rdpoptions.h"
#include "sshoptions.h"
#include "vncoptions.h"
#include "terminalappearance.h"

#include <QString>

struct SessionDefinition
{
    QString id;
    QString name;
    QString protocol;
    QString host;
    quint16 port = 0;
    QString username;
    // Per-session terminal colors and font; empty fields use the default.
    TerminalAppearance terminal;
    // Per-session SSH connection settings; empty fields use the defaults.
    SshOptions ssh;
    // Per-session remote desktop options; empty fields use the defaults.
    RdpOptions rdp;
    // Per-session VNC options; empty fields use the defaults.
    VncOptions vnc;
};
