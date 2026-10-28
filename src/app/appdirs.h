// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

// The folder holding data/ (shipped resources) and settings/ (user state).
// In a single-platform bundle that is the executable's folder; in the
// combined Linux + Windows package it is the folder above each platform's
// executable, so both platforms share one settings/ folder.
QString portableHome();
// portableHome()/name, for example portableFilePath("data/icons").
QString portableFilePath(const QString &name);
// Makes a file readable and writable by its owner only (vault, known hosts).
// Returns false, and logs a warning, where the file system cannot do that,
// for example on FAT-formatted USB sticks.
bool restrictToOwner(const QString &path);
