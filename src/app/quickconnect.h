// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "session.h"

#include <QStringList>

#include <optional>

// Connection targets typed in the quick-connect bar or given on the command
// line: [protocol://][user@]host[:port], for example "admin@10.0.0.5",
// "ssh://root@server:2222", "telnet://switch", "mosh://me@laptop",
// "rdp://DOMAIN\user@desktop" or "vnc://host:1"; serial lines as
// "serial://COM3:9600" or "serial:///dev/ttyUSB0:115200". Without a protocol
// it is SSH. IPv6 addresses go in brackets: "[::1]:22".
namespace QuickConnect {

// A one-off session (not saved) for the target, or nothing with *error set.
std::optional<SessionDefinition> parse(const QString &target, QString *error = nullptr);

// Recently used targets, newest first.
QStringList history();
void remember(const QString &target);

}
