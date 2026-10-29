// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "preferences.h"
#include "appdirs.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QProcess>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <yaml-cpp/yaml.h>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {
QString settingsPath()
{
    return portableFilePath(QStringLiteral("settings/settings.yaml"));
}

YAML::Node loadSection(const char *section)
{
    try {
        const auto node = YAML::LoadFile(settingsPath().toStdString())[section];
        if (node && node.IsMap())
            return node;
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the defaults.
    }
    return YAML::Node(YAML::NodeType::Map);
}

// Replaces one top-level section and keeps the rest of settings.yaml.
void saveSection(const char *section, const YAML::Node &node)
{
    const auto path = settingsPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;
    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    if (!settings.IsMap())
        settings = YAML::Node(YAML::NodeType::Map);
    settings[section] = node;

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) == contents.size())
        file.commit();
    else
        file.cancelWriting();
}

template <typename T>
void read(const YAML::Node &node, const char *key, T &value)
{
    try {
        if (node[key])
            value = node[key].as<T>();
    } catch (const YAML::Exception &) {
        // Keep the default for a value of the wrong type.
    }
}

void read(const YAML::Node &node, const char *key, QString &value)
{
    std::string text = value.toStdString();
    read(node, key, text);
    value = QString::fromStdString(text);
}

void readChoice(const YAML::Node &node, const char *key, QString &value,
                const QStringList &choices)
{
    QString text = value;
    read(node, key, text);
    if (choices.contains(text))
        value = text;
}
}

namespace {
// Copies a folder tree; existing files are replaced.
bool copyTree(const QString &from, const QString &to, QString *error)
{
    QDir source(from);
    if (!QDir().mkpath(to)) {
        if (error)
            *error = QStringLiteral("Cannot create %1").arg(QDir::toNativeSeparators(to));
        return false;
    }
    for (const auto &info : source.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
        // Links are not followed: one could loop, or reach outside settings/.
        if (info.isSymLink())
            continue;
        const auto target = QDir(to).filePath(info.fileName());
        if (info.isDir()) {
            if (!copyTree(info.filePath(), target, error))
                return false;
            continue;
        }
        QFile::remove(target);
        if (!QFile::copy(info.filePath(), target)) {
            if (error)
                *error = QStringLiteral("Cannot copy %1").arg(QDir::toNativeSeparators(info.filePath()));
            return false;
        }
        QFile::setPermissions(target, info.permissions());
    }
    return true;
}
}

SshPreferences loadSshPreferences()
{
    const auto node = loadSection("ssh");
    SshPreferences value;
    read(node, "connect_timeout", value.connectTimeout);
    value.connectTimeout = qBound(3, value.connectTimeout, 300);
    read(node, "keepalive_seconds", value.keepaliveSeconds);
    value.keepaliveSeconds = qBound(0, value.keepaliveSeconds, 3600);
    read(node, "compression", value.compression);
    read(node, "try_public_keys", value.tryPublicKeys);
    read(node, "forward_agent", value.forwardAgent);
    read(node, "follow_terminal_folder", value.followTerminalFolder);
    read(node, "forward_x11", value.forwardX11);
    readChoice(node, "file_transfer", value.fileTransfer,
               {QStringLiteral("auto"), QStringLiteral("sftp"), QStringLiteral("scp")});
    read(node, "agent", value.agent);
    read(node, "proxy", value.proxy);
    read(node, "identity_file", value.identityFile);
    read(node, "auto_reconnect", value.autoReconnect);
    read(node, "reconnect_delay", value.reconnectDelay);
    value.reconnectDelay = qBound(1, value.reconnectDelay, 600);
    read(node, "reconnect_attempts", value.reconnectAttempts);
    value.reconnectAttempts = qBound(1, value.reconnectAttempts, 100);
    readChoice(node, "terminal_type", value.terminalType,
               {QStringLiteral("xterm-256color"), QStringLiteral("xterm"),
                QStringLiteral("vt100"), QStringLiteral("linux")});
    return value;
}

void saveSshPreferences(const SshPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["connect_timeout"] = value.connectTimeout;
    node["keepalive_seconds"] = value.keepaliveSeconds;
    node["compression"] = value.compression;
    node["try_public_keys"] = value.tryPublicKeys;
    node["forward_agent"] = value.forwardAgent;
    node["follow_terminal_folder"] = value.followTerminalFolder;
    node["forward_x11"] = value.forwardX11;
    node["file_transfer"] = value.fileTransfer.toStdString();
    node["agent"] = value.agent.toStdString();
    node["proxy"] = value.proxy.toStdString();
    node["identity_file"] = value.identityFile.toStdString();
    node["auto_reconnect"] = value.autoReconnect;
    node["reconnect_delay"] = value.reconnectDelay;
    node["reconnect_attempts"] = value.reconnectAttempts;
    node["terminal_type"] = value.terminalType.toStdString();
    saveSection("ssh", node);
}

SftpPreferences loadSftpPreferences()
{
    const auto node = loadSection("sftp");
    SftpPreferences value;
    read(node, "show_hidden", value.showHidden);
    read(node, "ask_download_folder", value.askDownloadFolder);
    read(node, "download_folder", value.downloadFolder);
    readChoice(node, "conflict", value.conflict,
               {QStringLiteral("ask"), QStringLiteral("overwrite"), QStringLiteral("rename"),
                QStringLiteral("skip")});
    read(node, "confirm_delete", value.confirmDelete);
    readChoice(node, "temporary_upload", value.temporaryUpload,
               {QStringLiteral("above"), QStringLiteral("all"), QStringLiteral("off")});
    read(node, "temporary_upload_kb", value.temporaryUploadKb);
    value.temporaryUploadKb = qBound(0, value.temporaryUploadKb, 1024 * 1024);
    read(node, "open_folder_after_download", value.openFolderAfterDownload);
    read(node, "editor", value.editor);
    return value;
}

void saveSftpPreferences(const SftpPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["show_hidden"] = value.showHidden;
    node["ask_download_folder"] = value.askDownloadFolder;
    node["download_folder"] = value.downloadFolder.toStdString();
    node["conflict"] = value.conflict.toStdString();
    node["confirm_delete"] = value.confirmDelete;
    node["temporary_upload"] = value.temporaryUpload.toStdString();
    node["temporary_upload_kb"] = value.temporaryUploadKb;
    node["open_folder_after_download"] = value.openFolderAfterDownload;
    node["editor"] = value.editor.toStdString();
    saveSection("sftp", node);
}

SecurityPreferences loadSecurityPreferences()
{
    const auto node = loadSection("security");
    SecurityPreferences value;
    read(node, "auto_lock_minutes", value.autoLockMinutes);
    value.autoLockMinutes = qBound(0, value.autoLockMinutes, 24 * 60);
    read(node, "lock_on_minimize", value.lockOnMinimize);
    read(node, "unlock_at_startup", value.unlockAtStartup);
    readChoice(node, "password_store", value.passwordStore,
               {QStringLiteral("vault"), QStringLiteral("system"), QStringLiteral("both")});
    return value;
}

void saveSecurityPreferences(const SecurityPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["auto_lock_minutes"] = value.autoLockMinutes;
    node["lock_on_minimize"] = value.lockOnMinimize;
    node["unlock_at_startup"] = value.unlockAtStartup;
    node["password_store"] = value.passwordStore.toStdString();
    saveSection("security", node);
}

LoggingPreferences loadLoggingPreferences()
{
    const auto node = loadSection("logging");
    LoggingPreferences value;
    read(node, "enabled", value.enabled);
    read(node, "folder", value.folder);
    read(node, "timestamps", value.timestamps);
    read(node, "plain_text", value.plainText);
    read(node, "record", value.record);
    return value;
}

void saveLoggingPreferences(const LoggingPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["enabled"] = value.enabled;
    node["folder"] = value.folder.toStdString();
    node["timestamps"] = value.timestamps;
    node["plain_text"] = value.plainText;
    node["record"] = value.record;
    saveSection("logging", node);
}

QString sessionLogFolder(const LoggingPreferences &preferences)
{
    if (preferences.folder.isEmpty())
        return portableFilePath(QStringLiteral("settings/logs"));
    // A relative folder is relative to the portable folder, so it travels with it.
    return QDir(portableHome()).absoluteFilePath(preferences.folder);
}

LocalServersPreferences loadLocalServersPreferences()
{
    const auto node = loadSection("local_servers");
    LocalServersPreferences value;
    const auto readServer = [&node](const char *key, LocalServerSettings &server) {
        const auto section = node[key];
        if (!section || !section.IsMap())
            return;
        read(section, "port", server.port);
        server.port = qBound(1, server.port, 65535);
        readChoice(section, "folder", server.folder, {QStringLiteral("shared"), QStringLiteral("custom")});
        read(section, "custom_folder", server.customFolder);
        read(section, "uploads", server.uploads);
        read(section, "listen", server.listen);
        read(section, "max_upload_mb", server.maxUploadMb);
        server.maxUploadMb = qBound(1, server.maxUploadMb, 1024 * 1024);
    };
    readServer("tftp", value.tftp);
    readServer("http", value.http);
    return value;
}

void saveLocalServersPreferences(const LocalServersPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    for (const auto &[key, server] : {std::pair{"tftp", value.tftp}, std::pair{"http", value.http}}) {
        YAML::Node section(YAML::NodeType::Map);
        section["port"] = server.port;
        section["folder"] = server.folder.toStdString();
        section["custom_folder"] = server.customFolder.toStdString();
        section["uploads"] = server.uploads;
        section["listen"] = server.listen.toStdString();
        section["max_upload_mb"] = server.maxUploadMb;
        node[key] = section;
    }
    saveSection("local_servers", node);
}

LocalShellPreferences loadLocalShellPreferences()
{
    const auto node = loadSection("local_shell");
    LocalShellPreferences value;
    readChoice(node, "windows", value.windowsShell,
               {QStringLiteral("powershell"), QStringLiteral("pwsh"), QStringLiteral("cmd"),
                QStringLiteral("wsl"), QStringLiteral("custom")});
    read(node, "windows_custom", value.windowsCustom);
    read(node, "use_sudo", value.useSudo);
    readChoice(node, "linux", value.linuxShell, {QStringLiteral("default"), QStringLiteral("custom")});
    read(node, "linux_custom", value.linuxCustom);
    return value;
}

void saveLocalShellPreferences(const LocalShellPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["windows"] = value.windowsShell.toStdString();
    node["windows_custom"] = value.windowsCustom.toStdString();
    node["use_sudo"] = value.useSudo;
    node["linux"] = value.linuxShell.toStdString();
    node["linux_custom"] = value.linuxCustom.toStdString();
    saveSection("local_shell", node);
}

SessionListPreferences loadSessionListPreferences()
{
    const auto node = loadSection("session_list");
    SessionListPreferences value;
    read(node, "show_pinned", value.showPinned);
    read(node, "show_recent", value.showRecent);
    read(node, "recent_count", value.recentCount);
    value.recentCount = qBound(1, value.recentCount, 20);
    read(node, "show_host", value.showHost);
    read(node, "show_protocol", value.showProtocol);
    read(node, "check_status", value.checkStatus);
    read(node, "check_seconds", value.checkSeconds);
    value.checkSeconds = qBound(10, value.checkSeconds, 3600);
    // Earlier versions had summary_in_column: true/false.
    bool inColumn = false;
    read(node, "summary_in_column", inColumn);
    if (inColumn)
        value.folderSummary = QStringLiteral("column");
    readChoice(node, "folder_summary", value.folderSummary,
               {QStringLiteral("name"), QStringLiteral("column"), QStringLiteral("off")});
    read(node, "show_search", value.showSearch);
    read(node, "fit_panel", value.fitPanel);
    return value;
}

void saveSessionListPreferences(const SessionListPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["show_pinned"] = value.showPinned;
    node["show_recent"] = value.showRecent;
    node["recent_count"] = value.recentCount;
    node["show_host"] = value.showHost;
    node["show_protocol"] = value.showProtocol;
    node["check_status"] = value.checkStatus;
    node["check_seconds"] = value.checkSeconds;
    node["folder_summary"] = value.folderSummary.toStdString();
    node["show_search"] = value.showSearch;
    node["fit_panel"] = value.fitPanel;
    saveSection("session_list", node);
}

PresentationPreferences loadPresentationPreferences()
{
    const auto node = loadSection("presentation");
    PresentationPreferences value;
    read(node, "zoom_steps", value.zoomSteps);
    value.zoomSteps = qBound(0, value.zoomSteps, 20);
    read(node, "full_screen", value.fullScreen);
    read(node, "hide_panels", value.hidePanels);
    return value;
}

void savePresentationPreferences(const PresentationPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["zoom_steps"] = value.zoomSteps;
    node["full_screen"] = value.fullScreen;
    node["hide_panels"] = value.hidePanels;
    saveSection("presentation", node);
}

HttpRequestPreferences loadHttpRequestPreferences()
{
    const auto node = loadSection("http_request");
    HttpRequestPreferences value;
    read(node, "timeout_seconds", value.timeoutSeconds);
    read(node, "follow_redirects", value.followRedirects);
    read(node, "max_redirects", value.maxRedirects);
    read(node, "verify_certificates", value.verifyCertificates);
    read(node, "user_agent", value.userAgent);
    read(node, "max_body_mb", value.maxBodyMb);
    value.timeoutSeconds = qBound(1, value.timeoutSeconds, 3600);
    value.maxRedirects = qBound(0, value.maxRedirects, 50);
    value.maxBodyMb = qBound(1, value.maxBodyMb, 1024);
    return value;
}

void saveHttpRequestPreferences(const HttpRequestPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["timeout_seconds"] = value.timeoutSeconds;
    node["follow_redirects"] = value.followRedirects;
    node["max_redirects"] = value.maxRedirects;
    node["verify_certificates"] = value.verifyCertificates;
    node["user_agent"] = value.userAgent.toStdString();
    node["max_body_mb"] = value.maxBodyMb;
    saveSection("http_request", node);
}

RunCommandPreferences loadRunCommandPreferences()
{
    const auto node = loadSection("run_command");
    RunCommandPreferences value;
    read(node, "parallel", value.parallel);
    read(node, "timeout_seconds", value.timeoutSeconds);
    read(node, "terminal", value.terminal);
    read(node, "output_kb", value.outputKb);
    read(node, "keep_recent", value.keepRecent);
    value.parallel = qBound(1, value.parallel, 50);
    value.timeoutSeconds = qBound(1, value.timeoutSeconds, 24 * 3600);
    value.outputKb = qBound(16, value.outputKb, 64 * 1024);
    value.keepRecent = qBound(0, value.keepRecent, 100);
    if (const auto recent = node["recent"]; recent && recent.IsSequence()) {
        for (const auto &entry : recent) {
            if (entry.IsScalar() && value.recent.size() < value.keepRecent)
                value.recent << QString::fromStdString(entry.as<std::string>());
        }
    }
    return value;
}

void saveRunCommandPreferences(const RunCommandPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["parallel"] = value.parallel;
    node["timeout_seconds"] = value.timeoutSeconds;
    node["terminal"] = value.terminal;
    node["output_kb"] = value.outputKb;
    node["keep_recent"] = value.keepRecent;
    YAML::Node recent(YAML::NodeType::Sequence);
    for (const auto &command : value.recent.mid(0, value.keepRecent))
        recent.push_back(command.toStdString());
    node["recent"] = recent;
    saveSection("run_command", node);
}

BackupPreferences loadBackupPreferences()
{
    const auto node = loadSection("backup");
    BackupPreferences value;
    read(node, "backup_on_start", value.backupOnStart);
    read(node, "keep", value.keep);
    read(node, "remove_runtime_on_exit", value.removeRuntimeOnExit);
    value.keep = qBound(1, value.keep, 100);
    return value;
}

void saveBackupPreferences(const BackupPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["backup_on_start"] = value.backupOnStart;
    node["keep"] = value.keep;
    node["remove_runtime_on_exit"] = value.removeRuntimeOnExit;
    saveSection("backup", node);
}

QList<Snippet> loadSnippets()
{
    QList<Snippet> snippets;
    try {
        const auto node = YAML::LoadFile(settingsPath().toStdString())["snippets"];
        if (node && node.IsSequence()) {
            for (const auto &item : node) {
                Snippet snippet;
                read(item, "name", snippet.name);
                read(item, "text", snippet.text);
                read(item, "press_enter", snippet.pressEnter);
                if (!snippet.name.isEmpty())
                    snippets.append(snippet);
            }
        }
    } catch (const YAML::Exception &) {
    }
    return snippets;
}

void saveSnippets(const QList<Snippet> &snippets)
{
    YAML::Node node(YAML::NodeType::Sequence);
    for (const auto &snippet : snippets) {
        YAML::Node item(YAML::NodeType::Map);
        item["name"] = snippet.name.toStdString();
        item["text"] = snippet.text.toStdString();
        item["press_enter"] = snippet.pressEnter;
        node.push_back(item);
    }
    saveSection("snippets", node);
}

QList<HighlightRule> loadHighlightRules()
{
    QList<HighlightRule> rules;
    try {
        const auto node = YAML::LoadFile(settingsPath().toStdString())["highlights"];
        if (node && node.IsSequence()) {
            for (const auto &item : node) {
                HighlightRule rule;
                QString color;
                read(item, "pattern", rule.pattern);
                read(item, "color", color);
                read(item, "bold", rule.bold);
                read(item, "case_sensitive", rule.caseSensitive);
                read(item, "enabled", rule.enabled);
                rule.color = QColor::fromString(color);
                if (!rule.pattern.isEmpty() && rule.color.isValid())
                    rules.append(rule);
            }
            return rules;
        }
    } catch (const YAML::Exception &) {
    }
    // First run: a few rules that are useful on most servers.
    return {{QStringLiteral("\\b(error|failed|failure|fatal|denied)\\b"), QColor(0xe0, 0x4f, 0x4f), true, false, true},
            {QStringLiteral("\\b(warning|warn|deprecated)\\b"), QColor(0xe5, 0xa5, 0x0a), false, false, true},
            {QStringLiteral("\\b(ok|success|succeeded|passed|done)\\b"), QColor(0x4c, 0xb0, 0x50), false, false, false}};
}

void saveHighlightRules(const QList<HighlightRule> &rules)
{
    YAML::Node node(YAML::NodeType::Sequence);
    for (const auto &rule : rules) {
        YAML::Node item(YAML::NodeType::Map);
        item["pattern"] = rule.pattern.toStdString();
        item["color"] = rule.color.name().toStdString();
        item["bold"] = rule.bold;
        item["case_sensitive"] = rule.caseSensitive;
        item["enabled"] = rule.enabled;
        node.push_back(item);
    }
    saveSection("highlights", node);
}

QString backupsFolder()
{
    return portableFilePath(QStringLiteral("backups"));
}

QString createBackup(QString *error)
{
    const QString settings = portableFilePath(QStringLiteral("settings"));
    if (!QDir(settings).exists()) {
        if (error)
            *error = QStringLiteral("There are no settings to back up yet.");
        return QString();
    }
    auto name = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    const auto target = QDir(backupsFolder()).filePath(name);
    if (QDir(target).exists())
        return target;
    if (!copyTree(settings, target, error)) {
        QDir(target).removeRecursively();
        return QString();
    }
    // Logs can be large and are not settings.
    QDir(QDir(target).filePath(QStringLiteral("logs"))).removeRecursively();
    return target;
}

QStringList listBackups()
{
    auto names = QDir(backupsFolder()).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    std::reverse(names.begin(), names.end());
    return names;
}

void pruneBackups(int keep)
{
    const auto names = listBackups();
    for (qsizetype index = keep; index < names.size(); ++index)
        QDir(QDir(backupsFolder()).filePath(names[index])).removeRecursively();
}

bool restoreBackup(const QString &name, QString *error)
{
    const auto source = QDir(backupsFolder()).filePath(name);
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || !QDir(source).exists()) {
        if (error)
            *error = QStringLiteral("That backup no longer exists.");
        return false;
    }
    createBackup();
    return copyTree(source, portableFilePath(QStringLiteral("settings")), error);
}

TerminalBehaviour loadTerminalBehaviour()
{
    const auto node = loadSection("behaviour");
    TerminalBehaviour value;
    read(node, "scrollback_lines", value.scrollbackLines);
    value.scrollbackLines = qMax(0, value.scrollbackLines);
    readChoice(node, "cursor_shape", value.cursorShape,
               {QStringLiteral("block"), QStringLiteral("underline"), QStringLiteral("ibeam")});
    read(node, "cursor_blinks", value.cursorBlinks);
    readChoice(node, "scroll_bar", value.scrollBar,
               {QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("hidden")});
    read(node, "padding", value.padding);
    value.padding = qBound(0, value.padding, 32);
    read(node, "copy_on_select", value.copyOnSelect);
    readChoice(node, "right_click", value.rightClick,
               {QStringLiteral("menu"), QStringLiteral("paste")});
    read(node, "confirm_multiline_paste", value.confirmMultilinePaste);
    read(node, "trim_pasted_newlines", value.trimPastedNewlines);
    read(node, "word_characters", value.wordCharacters);
    readChoice(node, "bell", value.bell,
               {QStringLiteral("none"), QStringLiteral("visual"), QStringLiteral("sound")});
    read(node, "ctrl_click_opens_links", value.ctrlClickOpensLinks);
    read(node, "mark_activity", value.markActivity);
    return value;
}

void saveTerminalBehaviour(const TerminalBehaviour &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["scrollback_lines"] = value.scrollbackLines;
    node["cursor_shape"] = value.cursorShape.toStdString();
    node["cursor_blinks"] = value.cursorBlinks;
    node["scroll_bar"] = value.scrollBar.toStdString();
    node["padding"] = value.padding;
    node["copy_on_select"] = value.copyOnSelect;
    node["right_click"] = value.rightClick.toStdString();
    node["confirm_multiline_paste"] = value.confirmMultilinePaste;
    node["trim_pasted_newlines"] = value.trimPastedNewlines;
    node["word_characters"] = value.wordCharacters.toStdString();
    node["bell"] = value.bell.toStdString();
    node["ctrl_click_opens_links"] = value.ctrlClickOpensLinks;
    node["mark_activity"] = value.markActivity;
    saveSection("behaviour", node);
}

StartupPreferences loadStartupPreferences()
{
    const auto node = loadSection("startup");
    StartupPreferences value;
    readChoice(node, "on_startup", value.onStartup,
               {QStringLiteral("nothing"), QStringLiteral("local"), QStringLiteral("restore")});
    read(node, "confirm_close_connected", value.confirmCloseConnected);
    read(node, "confirm_exit", value.confirmExit);
    read(node, "new_tab_next_to_current", value.newTabNextToCurrent);
    read(node, "middle_click_closes", value.middleClickCloses);
    readChoice(node, "tab_title", value.tabTitle,
               {QStringLiteral("name"), QStringLiteral("userhost")});
    read(node, "remember_window", value.rememberWindow);
    read(node, "single_instance", value.singleInstance);
    read(node, "wayland_activation", value.waylandActivation);
    return value;
}

bool waylandActivationBeforeStart()
{
    bool value = StartupPreferences().waylandActivation;
    try {
        const auto path = QDir(portableHomeBeforeStart()).filePath(QStringLiteral("settings/settings.yaml"));
        if (const auto node = YAML::LoadFile(path.toStdString())["startup"]; node && node.IsMap())
            read(node, "wayland_activation", value);
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the default.
    }
    return value;
}

void saveStartupPreferences(const StartupPreferences &value)
{
    // Keep the state stored next to the preferences.
    auto node = loadSection("startup");
    node["on_startup"] = value.onStartup.toStdString();
    node["confirm_close_connected"] = value.confirmCloseConnected;
    node["confirm_exit"] = value.confirmExit;
    node["new_tab_next_to_current"] = value.newTabNextToCurrent;
    node["middle_click_closes"] = value.middleClickCloses;
    node["tab_title"] = value.tabTitle.toStdString();
    node["remember_window"] = value.rememberWindow;
    node["single_instance"] = value.singleInstance;
    node["wayland_activation"] = value.waylandActivation;
    saveSection("startup", node);
}

QStringList loadOpenSessions()
{
    QStringList ids;
    const auto node = loadSection("startup")["open_sessions"];
    if (node && node.IsSequence()) {
        for (const auto &id : node) {
            try {
                ids.append(QString::fromStdString(id.as<std::string>()));
            } catch (const YAML::Exception &) {
            }
        }
    }
    return ids;
}

void saveOpenSessions(const QStringList &sessionIds)
{
    auto node = loadSection("startup");
    YAML::Node ids(YAML::NodeType::Sequence);
    for (const auto &id : sessionIds)
        ids.push_back(id.toStdString());
    node["open_sessions"] = ids;
    saveSection("startup", node);
}

QByteArray loadWindowGeometry()
{
    QString text;
    read(loadSection("startup"), "window_geometry", text);
    return QByteArray::fromBase64(text.toLatin1());
}

void saveWindowGeometry(const QByteArray &geometry)
{
    auto node = loadSection("startup");
    node["window_geometry"] = geometry.toBase64().toStdString();
    saveSection("startup", node);
}

const QList<ShortcutDefinition> &shortcutDefinitions()
{
    static const QList<ShortcutDefinition> definitions{
        {QStringLiteral("new_local_terminal"), QStringLiteral("New local terminal"),
         QStringLiteral("Sessions"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T)},
        {QStringLiteral("new_session"), QStringLiteral("New session"),
         QStringLiteral("Sessions"), QKeySequence(Qt::CTRL | Qt::Key_N)},
        {QStringLiteral("edit_session"), QStringLiteral("Edit selected session"),
         QStringLiteral("Sessions"), QKeySequence(Qt::CTRL | Qt::Key_E)},
        {QStringLiteral("open_session"), QStringLiteral("Open selected session"),
         QStringLiteral("Sessions"), QKeySequence(Qt::CTRL | Qt::Key_Return)},
        {QStringLiteral("delete_folder"), QStringLiteral("Delete selected folder or session"),
         QStringLiteral("Sessions"), QKeySequence(QKeySequence::Delete)},
        {QStringLiteral("search_sessions"), QStringLiteral("Search sessions"),
         QStringLiteral("Sessions"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S)},
        {QStringLiteral("next_tab"), QStringLiteral("Next tab"), QStringLiteral("Tabs"),
         QKeySequence(Qt::CTRL | Qt::Key_PageDown)},
        {QStringLiteral("previous_tab"), QStringLiteral("Previous tab"), QStringLiteral("Tabs"),
         QKeySequence(Qt::CTRL | Qt::Key_PageUp)},
        {QStringLiteral("close_tab"), QStringLiteral("Close tab (or the active pane of a split tab)"),
         QStringLiteral("Tabs"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W)},
        {QStringLiteral("multi_exec"), QStringLiteral("Multi-exec: choose terminals"),
         QStringLiteral("Tabs"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M)},
        {QStringLiteral("split_right"), QStringLiteral("Split right"), QStringLiteral("Tabs"),
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)},
        {QStringLiteral("split_down"), QStringLiteral("Split down"), QStringLiteral("Tabs"),
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)},
        {QStringLiteral("duplicate_tab"), QStringLiteral("Duplicate tab"), QStringLiteral("Tabs"),
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)},
        {QStringLiteral("copy"), QStringLiteral("Copy"), QStringLiteral("Terminal"),
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C)},
        {QStringLiteral("paste"), QStringLiteral("Paste"), QStringLiteral("Terminal"),
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V)},
        {QStringLiteral("find"), QStringLiteral("Find in terminal"), QStringLiteral("Terminal"),
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)},
        {QStringLiteral("clear"), QStringLiteral("Clear terminal"), QStringLiteral("Terminal"),
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K)},
        {QStringLiteral("zoom_in"), QStringLiteral("Zoom in"), QStringLiteral("Terminal"),
         QKeySequence(Qt::CTRL | Qt::Key_Plus)},
        {QStringLiteral("zoom_out"), QStringLiteral("Zoom out"), QStringLiteral("Terminal"),
         QKeySequence(Qt::CTRL | Qt::Key_Minus)},
        {QStringLiteral("zoom_reset"), QStringLiteral("Reset zoom"), QStringLiteral("Terminal"),
         QKeySequence(Qt::CTRL | Qt::Key_0)},
        {QStringLiteral("broadcast"), QStringLiteral("Broadcast typing to all SSH tabs"),
         QStringLiteral("Tabs"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)},
        {QStringLiteral("presentation"), QStringLiteral("Presentation mode"),
         QStringLiteral("Window"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P)},
        {QStringLiteral("toggle_side_panel"), QStringLiteral("Show or hide the side panel"),
         QStringLiteral("Window"), QKeySequence(Qt::Key_F9)},
        {QStringLiteral("full_screen"), QStringLiteral("Full screen"), QStringLiteral("Window"),
         QKeySequence(Qt::Key_F11)},
        {QStringLiteral("lock_vault"), QStringLiteral("Lock or unlock the vault"),
         QStringLiteral("Window"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L)},
        {QStringLiteral("preferences"), QStringLiteral("Preferences"), QStringLiteral("Window"),
         QKeySequence(Qt::CTRL | Qt::Key_Comma)},
        {QStringLiteral("quit"), QStringLiteral("Exit"), QStringLiteral("Window"),
         QKeySequence(Qt::CTRL | Qt::Key_Q)},
    };
    return definitions;
}

QHash<QString, QKeySequence> loadShortcuts()
{
    const auto node = loadSection("shortcuts");
    QHash<QString, QKeySequence> shortcuts;
    for (const auto &definition : shortcutDefinitions()) {
        auto key = definition.defaultKey;
        QString text;
        read(node, definition.id.toStdString().c_str(), text);
        if (node[definition.id.toStdString()])
            key = QKeySequence::fromString(text, QKeySequence::PortableText);
        shortcuts.insert(definition.id, key);
    }
    return shortcuts;
}

void saveShortcuts(const QHash<QString, QKeySequence> &shortcuts)
{
    YAML::Node node(YAML::NodeType::Map);
    for (const auto &definition : shortcutDefinitions()) {
        const auto key = shortcuts.value(definition.id, definition.defaultKey);
        if (key != definition.defaultKey)
            node[definition.id.toStdString()] = key.toString(QKeySequence::PortableText).toStdString();
    }
    saveSection("shortcuts", node);
}

namespace {
// Whether another process runs a program from inside folder.
bool otherProcessUses(const QString &folder)
{
    const QString prefix = QDir::cleanPath(folder) + QLatin1Char('/');
#ifdef Q_OS_WIN
    bool found = false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return true; // Unknown: be safe.
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found;
         more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId())
            continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        wchar_t path[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            const auto image = QDir::fromNativeSeparators(QString::fromWCharArray(path, int(size)));
            found = image.startsWith(prefix, Qt::CaseInsensitive);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
#else
    const auto own = QString::number(QCoreApplication::applicationPid());
    for (const auto &pid : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (pid == own || !pid.front().isDigit())
            continue;
        const auto image = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
        if (image.startsWith(prefix))
            return true;
    }
    return false;
#endif
}
}

void scheduleRuntimeRemoval()
{
    const QString folder = QDir::cleanPath(QCoreApplication::applicationDirPath());
    const QString runtimeRoot = QDir::cleanPath(QDir(portableHome()).filePath(QStringLiteral("runtime")));
    // Only an unpacked copy made by the launcher, never an installed or
    // uncompressed program.
    if (QFileInfo(folder).dir().absolutePath() != runtimeRoot || otherProcessUses(folder))
        return;
#ifdef Q_OS_WIN
    // A running program's files cannot be deleted on Windows: a hidden
    // command interpreter waits a few seconds, until this process is gone.
    // It renames the folder first and deletes only if that worked: OpsRemote
    // started again meanwhile reuses this folder, and Windows refuses to
    // rename a folder whose programs run, so that copy keeps its files (it
    // used to lose every one it had not loaded yet). A start after the
    // rename finds no folder and unpacks a new one.
    const QString removing = folder + QStringLiteral(".removing");
    QProcess helper;
    helper.setProgram(QStringLiteral("cmd.exe"));
    // The runtime folder goes too once empty (rmdir without /s keeps it otherwise).
    helper.setNativeArguments(
        QStringLiteral("/d /c \"ping -n 4 127.0.0.1 >nul & ren \"%1\" \"%2\" && rmdir /s /q \"%3\" & "
                       "rmdir \"%4\" 2>nul\"")
            .arg(QDir::toNativeSeparators(folder), QFileInfo(removing).fileName(),
                 QDir::toNativeSeparators(removing), QDir::toNativeSeparators(runtimeRoot)));
    helper.setWorkingDirectory(portableHome());
    helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    helper.startDetached();
#else
    // Open and mapped files stay usable on Linux until the process ends.
    // Renamed first, so a start from now on unpacks a new copy instead of
    // reusing one that is being deleted.
    const QString removing = folder + QStringLiteral(".removing");
    QDir(removing).removeRecursively();
    if (QDir().rename(folder, removing))
        QDir(removing).removeRecursively();
    // The runtime folder too, unless another version is unpacked there.
    QDir().rmdir(runtimeRoot);
#endif
}
