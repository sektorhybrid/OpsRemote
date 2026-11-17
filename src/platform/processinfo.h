// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

// Other processes on this computer, as far as the system tells.
namespace ProcessInfo {

// The folder a process works in, or an empty string when it cannot be read
// (another user's process, or a system that does not say).
QString workingFolder(int pid);
// Its command line with the program's file name, not its path ("ssh web01"),
// or an empty string when unknown.
QString commandLine(int pid);
// Whether a process other than this one runs a program from inside folder;
// true when that cannot be found out, so callers stay on the safe side.
bool otherProcessRunsFrom(const QString &folder);

}
