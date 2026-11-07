// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

class QSpinBox;
namespace YAML { class Node; }

// Reconnecting after a dropped connection works the same for every session
// type that does it (SSH, RDP, VNC): each type has its own "auto_reconnect"
// switch plus a wait and a number of tries, in its Preferences tab and in
// each session. This is the one code for the wait and the tries.

constexpr int DefaultReconnectDelay = 5; // seconds
constexpr int DefaultReconnectAttempts = 5; // 0: keep trying

// reconnect_delay (1–600 s) and reconnect_attempts (0–1000) of a session
// type's section or a session's options; missing or invalid keys leave the
// values as they are.
void readReconnectTiming(const YAML::Node &node, int &delay, int &attempts);
// Only set values: a session's 0 delay and -1 tries mean "the Preferences'".
void writeReconnectTiming(YAML::Node &node, int delay, int attempts);

// Whether another try is allowed after made ones.
inline bool mayReconnect(int made, int attempts)
{
    return attempts == 0 || made < attempts;
}
// "attempt 2 of 5", or "attempt 2" when it keeps trying.
QString reconnectAttemptText(int attempt, int attempts);

// The two fields. With inheritedDelay/inheritedAttempts given (a session's
// window), the lowest value is the inherited one: 0 s, and -1 tries.
QSpinBox *reconnectDelayBox(int value, int inheritedDelay = -1);
QSpinBox *reconnectAttemptsBox(int value, int inheritedAttempts = -2);
