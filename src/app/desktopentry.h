// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

// Linux taskbars and docks show an app's icon only when a desktop file
// matches its window (Wayland's app id, which OpsRemote sets to Id). A
// portable OpsRemote has none installed, so it keeps one of its own, hidden
// from menus (NoDisplay), pointing at this copy's launcher and icon
// (data/opsremote.png). Gentle: nothing happens unless the applications
// folder already exists and is writable; a file OpsRemote did not write is
// never touched; any problem is only a log line.
// Preferences → Workspace → Startup & exit → Show the icon in the taskbar.
namespace DesktopEntry {

inline constexpr char Id[] = "opsremote-portable";

// Writes the file (when it is missing or names another folder), or with
// wanted false removes it if OpsRemote wrote it. Returns what happened, for
// the log and the Preferences row.
QString update(bool wanted);

}
