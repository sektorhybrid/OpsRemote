// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

// Whether this computer can offer a remote desktop its sound, printers, and
// smart cards, and other features that need something outside OpsRemote.
// The checks are quick (a looked-up file, library, or service; at most a
// 0.3 s local connection for X11 on Windows); each returns an empty string
// when the resource can be used, otherwise what is missing, in words.
namespace LocalResources {

QString soundProblem(); // also the microphone
QString printerProblem();
QString smartCardProblem();
// A local X server for X11 forwarding (DISPLAY, or :0 on Windows).
QString x11Problem();
// The mosh program, for Mosh sessions.
QString moshProblem();

}
