// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

// Features that exist only on some operating systems, asked for by the code
// that offers them instead of a #ifdef there. Settings that turn a feature
// off, and programs it needs, are checked where it is used, not here.
namespace Platform {

enum class Feature {
    LocalTerminals,         // a pseudo-terminal (Windows: ConPTY, 10 1809 or newer)
    AdministratorTerminals, // local terminals run as administrator or root
    LocalFileDrop,          // files dropped on a local terminal go to its program's folder
    LocalScripts,           // saved scripts run in local terminals
    WslSessions,            // Windows Subsystem for Linux
    PageantAgent,           // the SSH agent can be PuTTY's Pageant
    WaylandActivation,      // raising windows needs a Wayland activation token
    SelectionClipboard,     // text only selected in other programs (X11/Wayland primary selection)
};

// Why the feature is missing on this computer, or an empty string when it
// is there.
QString problem(Feature feature);
inline bool has(Feature feature)
{
    return problem(feature).isEmpty();
}

}
