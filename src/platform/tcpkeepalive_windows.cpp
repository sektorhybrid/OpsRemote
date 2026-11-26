// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "tcpkeepalive.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

namespace TcpKeepalive {

bool detectDeadNetwork(qintptr socket, int seconds)
{
    const SOCKET handle = SOCKET(socket);
    // Windows 10 1709 and later; older ones refuse, and the connection
    // works as before.
    const DWORD on = 1;
    const DWORD idle = DWORD(qMax(1, seconds / 2));
    const DWORD interval = DWORD(qMax(1, seconds / 6));
    const DWORD count = 3;
    const auto set = [handle](int level, int name, DWORD value) {
        return setsockopt(handle, level, name, reinterpret_cast<const char *>(&value), sizeof value) == 0;
    };
    bool ok = set(SOL_SOCKET, SO_KEEPALIVE, on);
    ok = set(IPPROTO_TCP, TCP_KEEPIDLE, idle) && ok;
    ok = set(IPPROTO_TCP, TCP_KEEPINTVL, interval) && ok;
    ok = set(IPPROTO_TCP, TCP_KEEPCNT, count) && ok;
    // Probes are not sent while typed data waits for its acknowledgement:
    // give up on it after the same time (seconds).
    ok = set(IPPROTO_TCP, TCP_MAXRT, DWORD(seconds)) && ok;
    return ok;
}

}
