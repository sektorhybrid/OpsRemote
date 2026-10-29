// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "portforward.h"
#include "rdpoptions.h"
#include "serialoptions.h"
#include "sshoptions.h"
#include "vncoptions.h"
#include "ftpoptions.h"
#include "rshoptions.h"
#include "xdmcp.h"
#include "terminalappearance.h"
#include "wakeonlan.h"

#include <QString>

struct SessionDefinition
{
    QString id;
    QString name;
    QString protocol;
    QString host;
    quint16 port = 0;
    QString username;
    // SSH server to connect through (see JumpHost in jumphost.h); empty:
    // the folder's.
    QString jumpHost;
    // Proxy for the first SSH connection (see ProxySettings); empty: the
    // folder's, then the Preferences'.
    QString proxy;
    WakeOnLan wakeOnLan;
    // Shown in the session list's Pinned folder.
    bool pinned = false;
    // Where its password is kept: empty (the Preferences' choice), vault,
    // system, or both.
    QString passwordStore;
    // SSH: port forwards opened with the session.
    QList<PortForward> forwards;
    // Per-session terminal colors and font; empty fields use the default.
    TerminalAppearance terminal;
    // Per-session SSH connection settings; empty fields use the defaults.
    SshOptions ssh;
    // Per-session remote desktop options; empty fields use the defaults.
    RdpOptions rdp;
    // Per-session VNC options; empty fields use the defaults.
    VncOptions vnc;
    // Per-session FTP options; empty fields use the defaults.
    FtpOptions ftp;
    // Serial sessions: the line settings; host is the port (COM3, /dev/ttyUSB0).
    SerialOptions serial;
    // Rlogin and RSH sessions (RSH: with its command); empty fields use the defaults.
    RshOptions rsh;
    // XDMCP sessions; empty fields use the defaults.
    XdmcpOptions xdmcp;
};
