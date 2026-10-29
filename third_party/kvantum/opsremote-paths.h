// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
// OpsRemote: where the built-in Kvantum style finds its settings and themes.
// Included into every Kvantum source file by the build (-include).
#pragma once
#include <QString>
QString opsremoteKvantumConfigHome(); // holds Kvantum/kvantum.kvconfig and user themes
QString opsremoteKvantumDataDir();    // holds Kvantum/<theme> (the bundled themes)
