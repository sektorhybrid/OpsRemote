// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QIcon>
#include <QList>
#include <QPair>
#include <QString>
#include <QStyle>

// Icon themes bundled in data/icons (subsets of freedesktop icon themes, see
// third_party/icon-themes). An empty theme name selects Qt's built-in icons.
void initializeIconThemes();
// (folder name, display name) of each bundled theme, sorted by display name.
QList<QPair<QString, QString>> availableIconThemes();
void setIconTheme(const QString &themeName);

// The current theme's icon, or the built-in fallback if the theme has none.
QIcon themedIcon(const QString &name, QStyle::StandardPixmap fallback);
// The icon centred on a transparent tile of exactly this size, so lists show
// their text in one column even when themes draw some icons smaller.
QIcon fixedSizeIcon(const QIcon &icon, const QSize &size);
// Icon for a remote file based on its name (via the MIME database).
QIcon fileTypeIcon(const QString &fileName, bool isDirectory, bool isSymlink);
