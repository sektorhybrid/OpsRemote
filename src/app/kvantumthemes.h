// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QStringList>

class QStyle;

// The built-in Kvantum style engine (third_party/kvantum). Its themes come
// from data/Kvantum/ (bundled) and settings/Kvantum/ (the user's own, the
// way Kvantum keeps them: <name>/<name>.kvconfig and <name>.svg).
namespace KvantumThemes {
// Style keys look like "kvantum:KvArc".
inline const QString Prefix = QStringLiteral("kvantum:");
QStringList available();
bool isKvantumKey(const QString &styleKey);
// The style for one theme, or nullptr if the theme is not there.
QStyle *create(const QString &styleKey);
}
