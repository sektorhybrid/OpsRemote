// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

class MainWindow;

// Test hooks: OPSREMOTE_TEST_* variables with which private/ci, the picture
// steps and tests by hand drive OpsRemote without a person. Nothing happens
// unless one is set. Each is described where it is read (or, for the tests
// in filetransfertest.cpp, in filetransfertest.h).

// The tests that need no window (settings snapshot, credentials, file
// transfers, tunnels, links, private files…): their exit status, or -1 when
// none was asked for. Right after QApplication starts.
int runHeadlessTest();

// The hooks that drive the main window once it is shown (pictures, drags,
// transfers, keys, opening sessions, closing after a while).
void installWindowTestHooks(MainWindow &window);
