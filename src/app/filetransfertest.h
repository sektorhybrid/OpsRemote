// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

// Testing: OPSREMOTE_TEST_FILES=url runs every file operation against a
// server and prints the results; url is sftp://, scp:// (add ?auto for SFTP
// with the SCP fallback, ?only for a file session without a shell), ftp://, or ftps:// with user:password, and for FTP
// optional ?security=none|auto|explicit|implicit&mode=active&listing=list.
int runFileTransferTest(const QString &spec);

// Testing: OPSREMOTE_TEST_SERVERS=folder runs the HTTP (18080) and TFTP
// (16969) servers on 127.0.0.1 for that folder, uploads on and at most 1 MB,
// with the iperf3 server on 15201 (tests up to 5 s), for
// OPSREMOTE_TEST_SECONDS, logging to standard output.
int runLocalServersTest(const QString &folder);

// Testing: OPSREMOTE_TEST_TUNNEL=ssh://user:password@host:port opens one of
// each forward (17001 local to :2121, 17002 to /tmp/test.sock, 17003 SOCKS,
// 17004 HTTP proxy, server 17005 to a web server here on 18080, server
// 17006 reverse SOCKS; later 17010 added and removed), probes, and prints
// the forwards' state and counters.
int runTunnelTest(const QString &spec);

// Testing: OPSREMOTE_TEST_TUNNEL_RUNNER=ssh://user:password@host:port?known_hosts=file
// runs a tunnel (local 17101 to :2121, socket 17102 to /tmp/test.sock) over
// its own connection, then over another connection that then closes.
int runTunnelRunnerTest(const QString &spec);

// Testing: OPSREMOTE_TEST_SSHCONFIG=file prints the tunnels an ssh config gives.
int runSshConfigTest(const QString &path);

// Testing: OPSREMOTE_TEST_RUNCOMMAND=ssh://user:password@host:port?known_hosts=file&second=port&down=port
// drives a Run on Hosts window on that server, the second port and a port
// that refuses connections: output on both streams, an exit status, the
// time limit, Stop, output past the limit, with and without a terminal.
int runRunCommandTest(const QString &spec);

// Testing: OPSREMOTE_TEST_STREAM=rlogin://user:password@host[:port] logs in
// (answering the password prompt), sets the window to 100x30, runs
// "stty size" and exits; rsh://user@host[:port]?command=...&input=line runs
// the command, types the line, and prints what came back. ?source=any|reserved
// chooses the source port.
int runStreamTest(const QString &spec);

// Testing: OPSREMOTE_TEST_XDMCP=host opens an XDMCP tab for host, prints its
// state and the X server's messages after OPSREMOTE_TEST_SECONDS (10), saves
// the X server's screen to OPSREMOTE_TEST_SCREENSHOT (with xwd and magick),
// then stops it.
int runXdmcpTest(const QString &host);

// Testing: OPSREMOTE_TEST_HTTP=1 sends requests to the test server of the
// HTTP Request tool (plain on 127.0.0.1:18480, TLS on 18443 with a.test and
// b.test certificates chosen by SNI) and prints what came back.
int runHttpTest();

// Testing: OPSREMOTE_TEST_FOLLOW=ssh://user:password@host:port?known_hosts=file
// opens an SSH terminal tab without following, runs "cd /tmp", then turns
// following on as the right-click menu does, and prints the Files pane's
// folder after each step.
int runFollowTest(const QString &spec);
