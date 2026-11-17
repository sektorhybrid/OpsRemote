// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "capabilities.h"

#ifdef Q_OS_WIN
#include "conpty.h"
#endif

namespace Platform {

QString problem(Feature feature)
{
    switch (feature) {
    case Feature::LocalTerminals:
    case Feature::AdministratorTerminals:
#ifdef Q_OS_WIN
        if (!ConPtyConnection::isAvailable())
            return QStringLiteral("Local terminals need Windows 10 version 1809 or newer");
#endif
        return {};
    case Feature::LocalFileDrop:
#ifdef Q_OS_LINUX
        return {};
#else
        return QStringLiteral("The folder of a local terminal is known only on Linux.");
#endif
    case Feature::LocalScripts:
#ifdef Q_OS_WIN
        return QStringLiteral("Saved scripts run on SSH servers; local terminals on Windows are not "
                              "supported yet.");
#else
        return {};
#endif
    case Feature::WslSessions:
#ifdef Q_OS_WIN
        return {};
#else
        return QStringLiteral("WSL sessions are for Windows.");
#endif
    case Feature::PageantAgent:
#ifdef Q_OS_WIN
        return {};
#else
        return QStringLiteral("Pageant is a Windows program.");
#endif
    case Feature::WaylandActivation:
#ifdef Q_OS_LINUX
        return {};
#else
        return QStringLiteral("Only Wayland desktops need it.");
#endif
    case Feature::SelectionClipboard:
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        return QStringLiteral("This system has no selection clipboard.");
#else
        return {};
#endif
    }
    return {};
}

}
