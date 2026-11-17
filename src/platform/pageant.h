// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QByteArray>

// PuTTY's Pageant on Windows, reached the way PuTTY does: a WM_COPYDATA
// message to its window naming a shared memory block that holds the request
// and receives the answer. Elsewhere Pageant is never available.
namespace Pageant {
bool isRunning();
// One SSH agent message (a 4-byte big-endian length, then the body); the
// answer in the same form, or empty if Pageant did not answer.
QByteArray query(const QByteArray &request);
}
