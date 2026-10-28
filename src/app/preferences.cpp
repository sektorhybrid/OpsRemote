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
    read(node, "open_folder_after_download", value.openFolderAfterDownload);
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
    node["open_folder_after_download"] = value.openFolderAfterDownload;
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
    return value;
}

void saveSecurityPreferences(const SecurityPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["auto_lock_minutes"] = value.autoLockMinutes;
    node["lock_on_minimize"] = value.lockOnMinimize;
    node["unlock_at_startup"] = value.unlockAtStartup;
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
    return value;
}

void saveLoggingPreferences(const LoggingPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["enabled"] = value.enabled;
    node["folder"] = value.folder.toStdString();
    node["timestamps"] = value.timestamps;
    node["plain_text"] = value.plainText;
    saveSection("logging", node);
}

QString sessionLogFolder(const LoggingPreferences &preferences)
{
    if (preferences.folder.isEmpty())
        return portableFilePath(QStringLiteral("settings/logs"));
    // A relative folder is relative to the portable folder, so it travels with it.
    return QDir(portableHome()).absoluteFilePath(preferences.folder);
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
    read(node, "show_host_in_list", value.showHostInList);
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
    node["show_host_in_list"] = value.showHostInList;
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
        {QStringLiteral("delete_folder"), QStringLiteral("Delete selected folder"),
         QStringLiteral("Sessions"), QKeySequence(QKeySequence::Delete)},
        {QStringLiteral("next_tab"), QStringLiteral("Next tab"), QStringLiteral("Tabs"),
         QKeySequence(Qt::CTRL | Qt::Key_PageDown)},
        {QStringLiteral("previous_tab"), QStringLiteral("Previous tab"), QStringLiteral("Tabs"),
         QKeySequence(Qt::CTRL | Qt::Key_PageUp)},
        {QStringLiteral("close_tab"), QStringLiteral("Close tab"), QStringLiteral("Tabs"),
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W)},
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
    QProcess helper;
    helper.setProgram(QStringLiteral("cmd.exe"));
    helper.setNativeArguments(QStringLiteral("/d /c \"ping -n 4 127.0.0.1 >nul & rmdir /s /q \"%1\"\"")
                                  .arg(QDir::toNativeSeparators(folder)));
    helper.setWorkingDirectory(portableHome());
    helper.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
    helper.startDetached();
#else
    // Open and mapped files stay usable on Linux until the process ends.
    QDir(folder).removeRecursively();
#endif
}
