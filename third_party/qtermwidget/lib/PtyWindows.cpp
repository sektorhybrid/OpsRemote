/*
    SPDX-License-Identifier: LGPL-2.0-or-later
    SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

    OpsRemote addition to QTermWidget (LGPL-2.0-or-later).
    See PtyWindows.h.
*/

#include "Pty.h"

using namespace Konsole;

Pty::Pty(QObject *parent)
    : QProcess(parent)
{
}

Pty::Pty(int, QObject *parent)
    : QProcess(parent)
{
}

Pty::~Pty() = default;

int Pty::start(const QString &, const QStringList &, const QStringList &, ulong, bool)
{
    return -1;
}

void Pty::setWindowSize(int lines, int cols)
{
    _windowLines = lines;
    _windowColumns = cols;
}

void Pty::sendData(const char *, int)
{
}
