// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "tcpkeepalive.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace TcpKeepalive {

bool detectDeadNetwork(qintptr socket, int seconds)
{
    const int fd = int(socket);
    const int on = 1;
    const int idle = qMax(1, seconds / 2);
    const int interval = qMax(1, seconds / 6);
    const int count = 3;
    bool ok = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on) == 0;
    ok = setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle) == 0 && ok;
    ok = setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval) == 0 && ok;
    ok = setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count) == 0 && ok;
#ifdef TCP_USER_TIMEOUT
    // Probes are not sent while typed data waits for its acknowledgement.
    const unsigned int timeoutMs = unsigned(seconds) * 1000;
    ok = setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeoutMs, sizeof timeoutMs) == 0 && ok;
#endif
    return ok;
}

}
