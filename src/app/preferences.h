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
    // Starting OpsRemote again brings the running copy forward instead.
    bool singleInstance = true;
    // Wayland: let OpsRemote ask the desktop to bring its windows forward
    // (xdg-activation). Off: GNOME shows its busy pointer for several
    // seconds after each request, that is after most menus and dialogs.
    bool waylandActivation = false;

    bool operator==(const StartupPreferences &) const = default;
};

// SSH connections (settings.yaml "ssh:").
struct SshPreferences
{
    int connectTimeout = 15; // seconds
    int keepaliveSeconds = 60; // 0 turns keepalives off
    bool compression = false;
    bool tryPublicKeys = true; // ~/.ssh keys and the SSH agent
    bool forwardAgent = false; // the server may use the local SSH agent (ssh -A)
    QString agent = QStringLiteral("auto"); // Windows: auto, openssh, pageant
    QString proxy = QStringLiteral("none"); // see ProxySettings
    QString identityFile; // an extra private key tried first
    bool autoReconnect = false;
    int reconnectDelay = 5; // seconds
    int reconnectAttempts = 5;
    QString terminalType = QStringLiteral("xterm-256color");
    bool followTerminalFolder = true; // the SFTP browser follows the shell's folder
    bool forwardX11 = false; // remote X11 programs show on this computer (ssh -Y)
    // The file browser's protocol: auto (SFTP, else SCP), sftp, or scp.
    QString fileTransfer = QStringLiteral("auto");

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
    // Uploads go to "name.filepart" and replace name when complete (as
    // WinSCP does): "above" (files of temporaryUploadKb or more), "all", or
    // "off".
    QString temporaryUpload = QStringLiteral("above");
    int temporaryUploadKb = 100;
    bool openFolderAfterDownload = false;
    // Program that edits remote files (with arguments); empty: the system's
    // program for the file type.
    QString editor;

    bool operator==(const SftpPreferences &) const = default;
};

// Credential vault (settings.yaml "security:").
struct SecurityPreferences
{
    int autoLockMinutes = 0; // 0: never; counts time without keyboard or mouse input
    bool lockOnMinimize = false;
    bool unlockAtStartup = false;
    // Where passwords are kept: vault, system, both (see PasswordStore).
    QString passwordStore = QStringLiteral("vault");

    bool operator==(const SecurityPreferences &) const = default;
};

// Session logging (settings.yaml "logging:").
struct LoggingPreferences
{
    bool enabled = false;
    QString folder; // empty: settings/logs
    bool timestamps = true;
    bool plainText = true; // strips colours and other escape sequences
    bool record = false; // also records every session as an asciinema .cast

    bool operator==(const LoggingPreferences &) const = default;
};

// The shell of local terminals (settings.yaml "local_shell:"), one choice per
// operating system, as both share the settings.
struct LocalShellPreferences
{
    QString windowsShell = QStringLiteral("powershell"); // powershell, pwsh, cmd, wsl, custom
    QString windowsCustom;
    bool useSudo = true; // run as administrator through Windows sudo when it is on (inline)
    QString linuxShell = QStringLiteral("default"); // default ($SHELL), custom
    QString linuxCustom;

    bool operator==(const LocalShellPreferences &) const = default;
};

// A local file server, as last set in Tools → Local Servers (settings.yaml
// "local_servers: tftp:" and "http:"). Servers only run while that window
// is open.
struct LocalServerSettings
{
    int port = 0;
    QString folder = QStringLiteral("shared"); // shared (the package's shared/), custom
    QString customFolder;
    bool uploads = false; // read-only unless on
    QString listen = QStringLiteral("all"); // see ListenAddress
    int maxUploadMb = 4096; // the largest file a device may upload

    bool operator==(const LocalServerSettings &) const = default;
};

struct LocalServersPreferences
{
    LocalServerSettings tftp{69};
    LocalServerSettings http{80};

    bool operator==(const LocalServersPreferences &) const = default;
};

// The session list (settings.yaml "session_list:").
struct SessionListPreferences
{
    bool showPinned = true;
    bool showRecent = true;
    int recentCount = 5;
    bool showHost = true; // columns; the status dot and the name always show
    bool showProtocol = false;
    bool checkStatus = true; // whether each host answers on its port
    int checkSeconds = 60;
    // Folder summaries: "name" (after the folder name), "column" (in the
    // column after the name), or "off".
    QString folderSummary = QStringLiteral("name");
    bool showSearch = true; // the search box above the list
    bool fitPanel = true; // the side panel widens to fit the list

    bool operator==(const SessionListPreferences &) const = default;
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

// Run a command on hosts (settings.yaml "run_command:"): the defaults each
// Run on Hosts window starts with, and the commands run lately.
struct RunCommandPreferences
{
    int parallel = 5; // hosts at once
    int timeoutSeconds = 60; // per host, for the command
    bool terminal = false; // a pseudo-terminal for the command
    int outputKb = 1024; // kept per host
    int keepRecent = 15;
    QStringList recent; // newest first

    bool operator==(const RunCommandPreferences &) const = default;
};

// Network Tools → HTTP Request (settings.yaml "http_request:"): what each
// request starts with; the tab changes them for itself.
struct HttpRequestPreferences
{
    int timeoutSeconds = 30;
    bool followRedirects = true;
    int maxRedirects = 10;
    bool verifyCertificates = true;
    QString userAgent = QStringLiteral("OpsRemote");
    int maxBodyMb = 50;

    bool operator==(const HttpRequestPreferences &) const = default;
};

HttpRequestPreferences loadHttpRequestPreferences();
void saveHttpRequestPreferences(const HttpRequestPreferences &preferences);

RunCommandPreferences loadRunCommandPreferences();
void saveRunCommandPreferences(const RunCommandPreferences &preferences);

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
// waylandActivation, read before QApplication exists (it decides how Qt
// talks to the desktop).
bool waylandActivationBeforeStart();
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
LocalServersPreferences loadLocalServersPreferences();
void saveLocalServersPreferences(const LocalServersPreferences &preferences);
LocalShellPreferences loadLocalShellPreferences();
void saveLocalShellPreferences(const LocalShellPreferences &preferences);
SessionListPreferences loadSessionListPreferences();
void saveSessionListPreferences(const SessionListPreferences &preferences);
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
