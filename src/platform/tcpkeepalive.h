// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QtGlobal>

// Connections that have no way of their own to notice a dead network
// (Telnet, Rlogin): left alone, TCP reports one only when it gives up on
// typed data, about 15 minutes later, and never while idle.
namespace TcpKeepalive {

// Makes the connected TCP socket report a dead network as an error within
// about `seconds`: keepalive probes after half of it without traffic, and an
// error when sent data stays unacknowledged that long. False when the system
// refused (the connection then works as before).
bool detectDeadNetwork(qintptr socket, int seconds);

}
