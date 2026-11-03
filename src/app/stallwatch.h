// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

class QObject;

// With OPSREMOTE_LOG (unless OPSREMOTE_LOG_STALLS=0): notes every time the GUI thread stops handling events
// for longer than a moment ("GUI stall"), with how long and where it was
// (its call stack, sampled while it is stuck), to find what blocks the window.
// Call on the GUI thread, after QApplication exists.
void watchGuiStalls(QObject *owner);

// With OPSREMOTE_LOG (Windows): every fault (access violation and the like)
// is logged with its stack as it happens, also those Windows catches itself
// and reports silently through Windows Error Reporting.
void logCrashes();
