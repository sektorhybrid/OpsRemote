// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

// The folder holding data/ (shipped resources) and settings/ (user state).
// In a single-platform bundle that is the executable's folder; in the
// combined Linux + Windows package it is the folder above each platform's
// executable, so both platforms share one settings/ folder.
QString portableHome();
// The same, before QApplication exists (Linux; elsewhere only from the
// launcher's OPSREMOTE_HOME).
QString portableHomeBeforeStart();
// The home for an executable in executableFolder (OPSREMOTE_HOME first);
// shared by portableHome() and the per-system portableHomeBeforeStart().
QString portableHomeFor(const QString &executableFolder);
// portableHome()/name, for example portableFilePath("data/icons").
QString portableFilePath(const QString &name);
// Makes a file (or a folder) usable by its owner only: on Linux mode 0600
// (0700), on Windows an access list for this user alone that does not take
// the folder's (a folder's is passed on to files made in it later). Returns
// false, and logs a warning, where the file system cannot do that, for
// example on FAT-formatted USB sticks.
bool restrictToOwner(const QString &path);
// Writes a file with secrets in one step (never half written), owner-only:
// on Linux from before the first byte, on Windows right after (and from
// the start inside a folder restrictToOwner() was used on). The one way to
// write the vault, keys, credentials and trust files.
bool savePrivateFile(const QString &path, const QByteArray &data, QString *error = nullptr);
// Creates a folder for secrets (keys, credentials) and makes it owner-only,
// so files made in it by other code (libssh, copies) are private from the
// start. False when it could not be created.
bool makePrivateFolder(const QString &path);
// For tests: who may use path. Linux: its mode ("600"); Windows: whether its
// access list is protected from the folder's, and how many entries name
// someone other than this user ("protected 1, others 0").
QString ownerOnlyReport(const QString &path);
// Puts from in the place of to in one step (replacing an existing to), so
// to is never missing or half written. Both must be on one file system.
bool replaceFile(const QString &from, const QString &to, QString *error = nullptr);
