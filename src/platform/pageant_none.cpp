// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// Systems without Pageant.

#include "pageant.h"

bool Pageant::isRunning()
{
    return false;
}

QByteArray Pageant::query(const QByteArray &)
{
    return {};
}
