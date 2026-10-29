// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QString>

// A look: the appearance settings that belong together, applied in one go
// from Preferences → Appearance → Looks or View → Look. Built-in looks come
// with OpsRemote; the user can save up to MaximumUserLooks of their own
// (settings/looks.yaml). When the current settings match no look, the
// appearance is "Custom".
struct Look
{
    QString id; // "builtin:<name>" or "user:<uuid>"
    QString name;
    // Colour mode as in Preferences: "auto" (follows the OS), "light",
    // "dark", a preset such as "nord", "system", or "custom" with colors.
    QString colorTheme;
    QHash<QString, QColor> customColors;
    QString style; // "opsremote", "fusion", "windowsvista", "windows11", …
    QString iconTheme; // empty: built-in icons
    // The terminal colour scheme, and the one while the interface is dark
    // (empty: the same); with "auto" these are the light and dark variants.
    QString terminalScheme;
    QString darkTerminalScheme;
    QString terminalFontFamily;
    int terminalFontSize = 0;
    QString interfaceFontFamily; // empty: the system's
    int interfaceFontSize = 0;
    bool windowBorder = true;

    bool isBuiltIn() const { return id.startsWith(QStringLiteral("builtin:")); }
    // Same appearance (name and id are not compared).
    bool sameAppearance(const Look &other) const;
};

constexpr int MaximumUserLooks = 20;

// The looks that come with OpsRemote, for this platform.
QList<Look> builtInLooks();
QList<Look> loadUserLooks();
void saveUserLooks(const QList<Look> &looks);
