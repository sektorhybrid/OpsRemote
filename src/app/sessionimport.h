// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "session.h"

#include <QList>
#include <QString>
#include <QStringList>

// Reads saved sessions from other programs. Passwords are never imported
// (most of these programs store them encrypted with their own keys).
namespace SessionImport {

struct Imported
{
    QStringList folders; // folder path inside the source, outermost first
    SessionDefinition session;
};

struct Result
{
    QList<Imported> sessions;
    int skipped = 0; // entries of kinds OpsRemote has no protocol for
    QString error; // the source could not be read at all
};

// OpenSSH client configuration: Host entries without wildcards.
Result fromOpenSshConfig(const QString &path);
QString defaultOpenSshConfigPath();
// PuTTY: the registry on Windows, ~/.putty/sessions elsewhere.
Result fromPutty();
// MobaXterm: an exported .mxtsessions file or MobaXterm.ini.
Result fromMobaXterm(const QString &path);
// mRemoteNG: confCons.xml (not with full-file encryption).
Result fromMRemoteNg(const QString &path);
// Remmina: the .remmina files in a folder.
Result fromRemmina(const QString &folder);
QString defaultRemminaFolder();

}
