// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

// A thin outline inside every window and dialog, so overlapping windows of
// the same colour can be told apart. Qt draws no shadow for its own title
// bars on Wayland (GNOME), so without it windows blend into each other.
// Menus, tooltips, and other popups are left alone.
void setWindowBordersEnabled(bool enabled);
bool windowBordersEnabled();
// On by default except on Windows, which draws its own border and shadow.
bool defaultWindowBorders();
