// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>

// How terminals behave, shared by every tab (settings.yaml "behaviour:").
struct TerminalBehaviour
{
    int scrollbackLines = 10000; // 0 keeps everything.
    QString cursorShape = QStringLiteral("block"); // block, underline, ibeam
    bool cursorBlinks = false;
    QString scrollBar = QStringLiteral("right"); // left, right, hidden
    int padding = 2;
    bool copyOnSelect = false;
    QString rightClick = QStringLiteral("menu"); // menu, paste
    bool confirmMultilinePaste = true;
    bool trimPastedNewlines = false;
    QString wordCharacters = QStringLiteral(":@-./_~?&=%+#");
    QString bell = QStringLiteral("visual"); // none, visual, sound
    bool ctrlClickOpensLinks = true;
    bool markActivity = true; // Marks background tabs that print output.

    bool operator==(const TerminalBehaviour &) const = default;
};

// What happens at startup and how tabs behave (settings.yaml "startup:").
struct StartupPreferences
{
    QString onStartup = QStringLiteral("nothing"); // nothing, local, restore
    bool confirmCloseConnected = true;
    bool confirmExit = true;
    bool newTabNextToCurrent = false;
    bool middleClickCloses = true;
    QString tabTitle = QStringLiteral("name"); // name, userhost
    bool rememberWindow = true;
    // Session list entries read "Name (host)".
    bool showHostInList = false;

    bool operator==(const StartupPreferences &) const = default;
};

// SSH connections (settings.yaml "ssh:").
struct SshPreferences
{
    int connectTimeout = 15; // seconds
    int keepaliveSeconds = 60; // 0 turns keepalives off
    bool compression = false;
    bool tryPublicKeys = true; // ~/.ssh keys and the SSH agent
    QString identityFile; // an extra private key tried first
    bool autoReconnect = false;
    int reconnectDelay = 5; // seconds
    int reconnectAttempts = 5;
    QString terminalType = QStringLiteral("xterm-256color");

    bool operator==(const SshPreferences &) const = default;
};

// The SFTP side panel (settings.yaml "sftp:").
struct SftpPreferences
{
    bool showHidden = true;
    bool askDownloadFolder = true;
    QString downloadFolder; // empty: the system's Downloads folder
    QString conflict = QStringLiteral("ask"); // ask, overwrite, rename, skip
    bool confirmDelete = true;
    bool openFolderAfterDownload = false;

    bool operator==(const SftpPreferences &) const = default;
};

// Credential vault (settings.yaml "security:").
struct SecurityPreferences
{
    int autoLockMinutes = 0; // 0: never; counts time without keyboard or mouse input
    bool lockOnMinimize = false;
    bool unlockAtStartup = false;

    bool operator==(const SecurityPreferences &) const = default;
};

// Session logging (settings.yaml "logging:").
struct LoggingPreferences
{
    bool enabled = false;
    QString folder; // empty: settings/logs
    bool timestamps = true;
    bool plainText = true; // strips colours and other escape sequences

    bool operator==(const LoggingPreferences &) const = default;
};

// Presentation mode (settings.yaml "presentation:").
struct PresentationPreferences
{
    int zoomSteps = 4;
    bool fullScreen = true;
    bool hidePanels = true; // side panel, toolbar, and status bar

    bool operator==(const PresentationPreferences &) const = default;
};

// Portable data backups (settings.yaml "backup:").
struct BackupPreferences
{
    bool backupOnStart = true;
    int keep = 10;
    // Delete the unpacked program (runtime/...) when OpsRemote
    // closes; the next start unpacks it again.
    bool removeRuntimeOnExit = false;

    bool operator==(const BackupPreferences &) const = default;
};

struct Snippet
{
    QString name;
    QString text;
    bool pressEnter = true;

    bool operator==(const Snippet &) const = default;
};

// Colours matching text in SSH output.
struct HighlightRule
{
    QString pattern; // a regular expression
    QColor color;
    bool bold = false;
    bool caseSensitive = false;
    bool enabled = true;

    bool operator==(const HighlightRule &) const = default;
};

TerminalBehaviour loadTerminalBehaviour();
void saveTerminalBehaviour(const TerminalBehaviour &behaviour);
StartupPreferences loadStartupPreferences();
void saveStartupPreferences(const StartupPreferences &preferences);

SshPreferences loadSshPreferences();
void saveSshPreferences(const SshPreferences &preferences);
SftpPreferences loadSftpPreferences();
void saveSftpPreferences(const SftpPreferences &preferences);
SecurityPreferences loadSecurityPreferences();
void saveSecurityPreferences(const SecurityPreferences &preferences);
LoggingPreferences loadLoggingPreferences();
void saveLoggingPreferences(const LoggingPreferences &preferences);
QString sessionLogFolder(const LoggingPreferences &preferences);
PresentationPreferences loadPresentationPreferences();
void savePresentationPreferences(const PresentationPreferences &preferences);
BackupPreferences loadBackupPreferences();
void saveBackupPreferences(const BackupPreferences &preferences);
QList<Snippet> loadSnippets();
void saveSnippets(const QList<Snippet> &snippets);
QList<HighlightRule> loadHighlightRules();
void saveHighlightRules(const QList<HighlightRule> &rules);

// Backups of the settings folder, in backups/<yyyy-MM-dd_HH-mm-ss>/ next to
// settings/. createBackup() returns the new folder, or an empty string.
QString backupsFolder();
QString createBackup(QString *error = nullptr);
QStringList listBackups(); // newest first, folder names
void pruneBackups(int keep);
// Copies a backup over settings/ (after backing up the current state).
bool restoreBackup(const QString &name, QString *error = nullptr);

// Removes the unpacked program folder this process runs from, if it runs
// from <portable home>/runtime/<name> and no other process uses that copy.
// Call when exiting; on Windows a hidden helper deletes it shortly after.
void scheduleRuntimeRemoval();

// Saved session IDs reopened by onStartup == "restore", and the window geometry.
QStringList loadOpenSessions();
void saveOpenSessions(const QStringList &sessionIds);
QByteArray loadWindowGeometry();
void saveWindowGeometry(const QByteArray &geometry);

// Configurable keyboard shortcuts (settings.yaml "shortcuts:", only the ones
// that differ from the default).
struct ShortcutDefinition
{
    QString id;
    QString label;
    QString category;
    QKeySequence defaultKey;
};

const QList<ShortcutDefinition> &shortcutDefinitions();
// Current shortcut of every definition, by ID.
QHash<QString, QKeySequence> loadShortcuts();
void saveShortcuts(const QHash<QString, QKeySequence> &shortcuts);
