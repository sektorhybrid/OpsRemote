// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

// Files dropped on a terminal go into the folder of the program running in
// it, found when they are dropped (Preferences → Terminals → Behaviour →
// Upload files dropped on a terminal). This holds what every kind of
// terminal shares: where they go, asking first, and copying.
namespace FileDrop {

// Where the terminal is, found at the drop.
struct Folder
{
    QString path; // empty: unknown (error says why)
    QString where; // "this computer", "admin@web01", "container web"…
    // The program in front of the shell, when it is not the shell itself
    // ("ssh other-server", "vim notes.txt"): the files would not go where
    // that program is.
    QString program;
    bool nested = false; // that program is another machine or user (ssh, su, sudo, docker exec…)
    QString error;
    QString note; // how the folder was found, when it is not certain
};

// A local terminal on Linux: the folder of the program in front, from /proc.
Folder localFolder(int shellPid, int foregroundPid);

// A remote shell, from the server's answer to SshSession::queryDropFolder()
// (or a container's); reported: the folder the shell last told, used when
// the server could not be asked.
Folder remoteFolder(const QString &answer, const QString &where, const QString &reported);

// Whether a command line is another machine or user (ssh, su, sudo -i, …).
bool isNestedProgram(const QString &commandLine);

enum class Choice { Upload, PastePaths, Cancel };

// Asks before copying: names the files and the folder, warns when another
// program is in front, offers another folder (chooseFolder: empty when not
// possible or cancelled) or pasting the paths. folder->path may change.
Choice confirm(QWidget *parent, const QStringList &files, Folder *folder,
               const std::function<QString()> &chooseFolder);

// What to do with files that already exist there (the "When a file already
// exists" setting; "ask" asks once for all of them). False: cancelled.
enum class Existing { Overwrite, Rename, Skip };
bool existingAction(QWidget *parent, const QStringList &clashing, Existing *action);

// Copies into a local folder in the background, then calls done with what
// went wrong (empty: all copied).
void copyLocal(QWidget *context, const QStringList &files, const QString &folder, Existing action,
               const std::function<void(const QStringList &problems)> &done);

// "name (2).txt": a free name in folder.
QString freeName(const QString &folder, const QString &name, const std::function<bool(const QString &)> &exists);

// Shows what went wrong, or that it worked, near the terminal.
void report(QWidget *parent, int count, const QString &target, const QStringList &problems);

} // namespace FileDrop
