// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"
#include "mainwindow.h"
#include "filespage.h"
#include "icons.h"
#include "jumphost.h"
#include "castrecorder.h"
#ifdef Q_OS_WIN
#include "conpty.h"
#endif
#include "localservers.h"
#include "availabilitynote.h"
#include "localresources.h"
#include "looks.h"
#include "quickconnect.h"
#include "serialoptions.h"
#include "sessionimport.h"
#include "systemcredentials.h"
#include "splittab.h"
#include "streamconnection.h"
#include "xdmcp.h"
#include "opsremotestyle.h"
#include "keymanager.h"
#include "kvantumthemes.h"
#include "networktools.h"
#include "proxy.h"
#include "tunnels.h"
#include "wakeonlan.h"
#include "windowborder.h"
#include "masterpassworddialog.h"
#include "preferenceswindow.h"
#include "rdppage.h"
#include "vncpage.h"
#include "remotescreenpage.h"
#include "sftppanel.h"
#include "terminalpage.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QColorDialog>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QDateTime>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QScrollBar>
#include <QFont>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QTreeWidgetItemIterator>
#include <QSpinBox>
#include <QCheckBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QGuiApplication>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QMouseEvent>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <yaml-cpp/yaml.h>

namespace {
constexpr int ItemKindRole = Qt::UserRole;
constexpr int SessionIdRole = Qt::UserRole + 1;
// Session list columns; the name is column 0.
constexpr int HostColumn = 1;
constexpr int ProtocolColumn = 2;
constexpr int SummaryColumn = 3; // folder summaries when no other column follows the name
// A folder's jump host for its sessions (see JumpHost); empty: its parent's.
constexpr int FolderJumpHostRole = Qt::UserRole + 2;
// A folder's proxy for its sessions (see ProxySettings); empty: its parent's.
constexpr int FolderProxyRole = Qt::UserRole + 3;
constexpr int MaximumFolderDepth = 3;

// User state (sessions, preferences, vault, known hosts) lives in settings/
// next to the executable, so the application folder carries it.
QString stateDirectory()
{
    return portableFilePath(QStringLiteral("settings"));
}

QString stateFilePath(const QString &name)
{
    return QDir(stateDirectory()).filePath(name);
}

// Protocols a saved session can use, as shown in the session window.
QStringList sessionProtocols()
{
    return {QStringLiteral("SSH"), QStringLiteral("VNC"), QStringLiteral("RDP"),
            QStringLiteral("Telnet"), QStringLiteral("Mosh"), QStringLiteral("Serial"),
            QStringLiteral("FTP"), QStringLiteral("SFTP"), QStringLiteral("SCP"),
            QStringLiteral("Rlogin"), QStringLiteral("RSH"), QStringLiteral("XDMCP")};
}

// Protocols over SSH: a terminal (SSH), or only files (SFTP, SCP).
bool isSshProtocol(const QString &protocol)
{
    for (const auto *name : {"SSH", "SFTP", "SCP"}) {
        if (protocol.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

// Protocols shown in a terminal tab.
bool isTerminalProtocol(const QString &protocol)
{
    for (const auto *name : {"SSH", "Telnet", "Mosh", "Serial", "Rlogin", "RSH"}) {
        if (protocol.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QStringList loadRecentSessions();
void saveRecentSessions(const QStringList &ids);

// The folder icon with a badge in its lower right corner: a star for Pinned,
// a clock for Recent. Drawn here, so every icon theme has them.
QIcon badgedFolderIcon(const QIcon &folder, bool star)
{
    QIcon icon;
    for (const int size : {16, 20, 24, 32, 48}) {
        for (const qreal ratio : {1.0, 2.0}) {
            QPixmap pixmap(QSize(size, size) * ratio);
            pixmap.setDevicePixelRatio(ratio);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            folder.paint(&painter, QRect(0, 0, size, size));
            const qreal badge = size * 0.72;
            const QRectF area(size - badge, size - badge, badge, badge);
            const QPointF c = area.center();
            const qreal r = badge / 2.0;
            // A light ring keeps the badge readable over any folder colour.
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 235));
            painter.drawEllipse(c, r, r);
            const QColor ink(0x4a, 0x4a, 0x4a);
            if (star) {
                QPolygonF points;
                for (int i = 0; i < 10; ++i) {
                    const qreal radius = (i % 2 == 0) ? r * 0.82 : r * 0.36;
                    const qreal angle = -M_PI / 2 + i * M_PI / 5;
                    points << QPointF(c.x() + radius * std::cos(angle), c.y() + radius * std::sin(angle));
                }
                painter.setBrush(QColor(0xe0, 0x95, 0x5a));
                painter.setPen(QPen(QColor(0xb0, 0x60, 0x2a), qMax(0.6, size / 32.0)));
                painter.drawPolygon(points);
            } else {
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(ink, qMax(1.0, size / 20.0)));
                painter.drawEllipse(c, r * 0.72, r * 0.72);
                painter.setPen(QPen(ink, qMax(1.0, size / 20.0), Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(c, QPointF(c.x(), c.y() - r * 0.48));
                painter.drawLine(c, QPointF(c.x() + r * 0.36, c.y()));
            }
            painter.end();
            icon.addPixmap(pixmap);
        }
    }
    return icon;
}

// Moves state written by earlier versions from data/ into settings/.
void migrateStateFromDataDirectory()
{
    const QDir legacy(portableFilePath(QStringLiteral("data")));
    for (const auto *name : {"sessions.yaml", "sessions.json", "settings.yaml",
                             "credentials.vault", "known_hosts"}) {
        const auto from = legacy.filePath(QString::fromLatin1(name));
        const auto to = stateFilePath(QString::fromLatin1(name));
        if (QFile::exists(from) && !QFile::exists(to) && QDir().mkpath(stateDirectory()))
            QFile::rename(from, to);
    }
}

// Application color themes beyond System, Light, Dark, and Custom: plain
// palette colors, applied the same way as the built-in Dark theme.
struct PalettePreset
{
    const char *key;
    const char *label;
    QRgb window, base, alternateBase, text, button, highlight, highlightedText, link,
        placeholder;
};

constexpr PalettePreset PalettePresets[] = {
    {"nord", "Nord", 0x2e3440, 0x3b4252, 0x434c5e, 0xeceff4, 0x3b4252, 0x88c0d0, 0x2e3440,
     0x81a1c1, 0x8f9bb3},
    {"solarized-dark", "Solarized Dark", 0x002b36, 0x073642, 0x0a3f4c, 0xeee8d5, 0x073642,
     0x268bd2, 0xfdf6e3, 0x2aa198, 0x839496},
    {"solarized-light", "Solarized Light", 0xeee8d5, 0xfdf6e3, 0xf5efdc, 0x073642, 0xeee8d5,
     0x268bd2, 0xfdf6e3, 0x2aa198, 0x657b83},
    {"dracula", "Dracula", 0x282a36, 0x1e1f29, 0x343746, 0xf8f8f2, 0x343746, 0xbd93f9,
     0x282a36, 0x8be9fd, 0x8b95c9},
    {"gruvbox-dark", "Gruvbox Dark", 0x282828, 0x1d2021, 0x32302f, 0xebdbb2, 0x3c3836,
     0xd79921, 0x282828, 0x83a598, 0xa89984},
    {"high-contrast", "High Contrast", 0x000000, 0x000000, 0x1a1a1a, 0xffffff, 0x000000,
     0xffff00, 0x000000, 0x00ffff, 0xd0d0d0},
};

const PalettePreset *findPalettePreset(const QString &key)
{
    for (const auto &preset : PalettePresets) {
        if (key == QLatin1String(preset.key))
            return &preset;
    }
    return nullptr;
}

QString settingsFilePath()
{
    return stateFilePath(QStringLiteral("settings.yaml"));
}

QString loadUiStylePreference()
{
    try {
        const auto settings = YAML::LoadFile(settingsFilePath().toStdString());
        const auto ui = settings["ui"];
        if (ui && ui["style"])
            return QString::fromStdString(ui["style"].as<std::string>());
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the current platform style.
    }
    return QStringLiteral("system");
}

QString loadColorThemePreference()
{
    try {
        const auto settings = YAML::LoadFile(settingsFilePath().toStdString());
        const auto ui = settings["ui"];
        if (ui && ui["color_theme"])
            return QString::fromStdString(ui["color_theme"].as<std::string>());
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the platform palette.
    }
    return QStringLiteral("system");
}

// Local shells run in a pseudo-terminal: a PTY on Linux, a pseudo console
// (ConPTY) on Windows 10 1809 and newer. SSH sessions do not need one.
constexpr bool LocalTerminalSupported = true;

void disableLocalTerminalWhereUnsupported(QAction *action)
{
#ifdef Q_OS_WIN
    if (!ConPtyConnection::isAvailable()) {
        action->setEnabled(false);
        action->setToolTip(QStringLiteral("Local terminals need Windows 10 version 1809 or newer"));
    }
#else
    Q_UNUSED(action);
#endif
}

// The program a local terminal runs, from the Preferences: (program,
// arguments, label). An empty program is the login shell (Linux).
struct LocalShell
{
    QString program;
    QStringList arguments;
    QString label;
};

LocalShell localShell(const LocalShellPreferences &preferences)
{
#ifdef Q_OS_WIN
    const QString system = QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")));
    const QString choice = preferences.windowsShell;
    if (choice == QStringLiteral("custom") && !preferences.windowsCustom.isEmpty()) {
        auto parts = QProcess::splitCommand(preferences.windowsCustom);
        const QString program = parts.takeFirst();
        return {program, parts, QFileInfo(program).completeBaseName()};
    }
    if (choice == QStringLiteral("pwsh")) {
        QString pwsh = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
        if (pwsh.isEmpty() && QFile::exists(QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe")))
            pwsh = QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe");
        if (!pwsh.isEmpty())
            return {pwsh, {QStringLiteral("-NoLogo")}, QStringLiteral("PowerShell")};
    }
    if (choice == QStringLiteral("cmd"))
        return {qEnvironmentVariable("ComSpec", system + QStringLiteral("/System32/cmd.exe")), {},
                QStringLiteral("Command Prompt")};
    if (choice == QStringLiteral("wsl"))
        return {system + QStringLiteral("/System32/wsl.exe"), {QStringLiteral("~")}, QStringLiteral("WSL")};
    return {system + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe"),
            {QStringLiteral("-NoLogo")}, QStringLiteral("PowerShell")};
#else
    if (preferences.linuxShell == QStringLiteral("custom") && !preferences.linuxCustom.isEmpty()) {
        auto parts = QProcess::splitCommand(preferences.linuxCustom);
        const QString program = parts.takeFirst();
        return {program, parts, QFileInfo(program).fileName()};
    }
    return {QString(), {}, QStringLiteral("Local Terminal")};
#endif
}

QString loadIconThemePreference()
{
    try {
        const auto ui = YAML::LoadFile(settingsFilePath().toStdString())["ui"];
        if (ui && ui["icon_theme"])
            return QString::fromStdString(ui["icon_theme"].as<std::string>());
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the built-in icons.
    }
    return QString();
}

void saveIconThemePreference(const QString &themeName)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    settings["ui"]["icon_theme"] = themeName.toStdString();

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

QHash<QString, QColor> loadCustomColors()
{
    QHash<QString, QColor> colors;
    try {
        const auto settings = YAML::LoadFile(settingsFilePath().toStdString());
        const auto values = settings["ui"]["custom_colors"];
        for (const auto *key : {"window", "base", "text", "highlight"}) {
            if (values[key]) {
                const QColor color(QString::fromStdString(values[key].as<std::string>()));
                if (color.isValid())
                    colors.insert(QString::fromLatin1(key), color);
            }
        }
    } catch (const YAML::Exception &) {
        // Invalid custom colors are ignored.
    }
    return colors;
}

void saveColorThemePreference(const QString &themeKey,
                              const QHash<QString, QColor> &customColors)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    settings["ui"]["color_theme"] = themeKey.toStdString();
    for (auto iterator = customColors.cbegin(); iterator != customColors.cend(); ++iterator)
        settings["ui"]["custom_colors"][iterator.key().toStdString()] =
            iterator.value().name(QColor::HexRgb).toStdString();

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

void saveUiStylePreference(const QString &styleKey)
{
    const auto path = settingsFilePath();
    const auto directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if the file does not exist or is invalid.
    }
    settings["ui"]["style"] = styleKey.toStdString();

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

QHash<QString, bool> loadFolderExpansionStates()
{
    QHash<QString, bool> states;
    try {
        const auto settings = YAML::LoadFile(settingsFilePath().toStdString());
        const auto folders = settings["session_tree"]["folder_expansion"];
        if (!folders || !folders.IsMap())
            return states;
        for (const auto &entry : folders)
            states.insert(QString::fromStdString(entry.first.as<std::string>()),
                          entry.second.as<bool>(true));
    } catch (const YAML::Exception &) {
        // Missing or invalid tree state uses the default expanded folders.
    }
    return states;
}

void saveFolderExpansionStates(const QHash<QString, bool> &states)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    YAML::Node folders(YAML::NodeType::Map);
    for (auto iterator = states.cbegin(); iterator != states.cend(); ++iterator)
        folders[iterator.key().toStdString()] = iterator.value();
    settings["session_tree"]["folder_expansion"] = folders;

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

YAML::Node sessionToYaml(const SessionDefinition &session)
{
    YAML::Node node;
    node["id"] = session.id.toStdString();
    node["name"] = session.name.toStdString();
    node["protocol"] = session.protocol.toStdString();
    node["host"] = session.host.toStdString();
    node["port"] = static_cast<int>(session.port);
    node["username"] = session.username.toStdString();
    if (!session.jumpHost.isEmpty())
        node["jump_host"] = session.jumpHost.toStdString();
    if (!session.forwards.isEmpty())
        node["forwards"] = portForwardsToYaml(session.forwards);
    if (!session.proxy.isEmpty())
        node["proxy"] = session.proxy.toStdString();
    if (!session.wakeOnLan.isEmpty())
        node["wake_on_lan"] = wakeOnLanToYaml(session.wakeOnLan);
    if (session.pinned)
        node["pinned"] = true;
    if (!session.passwordStore.isEmpty())
        node["password_store"] = session.passwordStore.toStdString();
    if (!session.terminal.isEmpty()) {
        YAML::Node terminal(YAML::NodeType::Map);
        if (!session.terminal.colorScheme.isEmpty())
            terminal["color_scheme"] = session.terminal.colorScheme.toStdString();
        if (!session.terminal.fontFamily.isEmpty())
            terminal["font_family"] = session.terminal.fontFamily.toStdString();
        if (session.terminal.fontSize > 0)
            terminal["font_size"] = session.terminal.fontSize;
        node["terminal"] = terminal;
    }
    if (!session.rdp.isEmpty()) {
        YAML::Node rdp(YAML::NodeType::Map);
        rdpOptionsToYaml(session.rdp, rdp);
        node["rdp"] = rdp;
    }
    if (!session.ssh.isEmpty()) {
        YAML::Node ssh(YAML::NodeType::Map);
        sshOptionsToYaml(session.ssh, ssh);
        node["ssh"] = ssh;
    }
    if (!session.vnc.isEmpty()) {
        YAML::Node vnc(YAML::NodeType::Map);
        vncOptionsToYaml(session.vnc, vnc);
        node["vnc"] = vnc;
    }
    if (!session.ftp.isEmpty()) {
        YAML::Node ftp(YAML::NodeType::Map);
        ftpOptionsToYaml(session.ftp, ftp);
        node["ftp"] = ftp;
    }
    if (!session.rsh.isEmpty()) {
        YAML::Node rsh(YAML::NodeType::Map);
        rshOptionsToYaml(session.rsh, rsh);
        node["rsh"] = rsh;
    }
    if (!session.xdmcp.isEmpty()) {
        YAML::Node xdmcp(YAML::NodeType::Map);
        xdmcpOptionsToYaml(session.xdmcp, xdmcp);
        node["xdmcp"] = xdmcp;
    }
    if (session.protocol == QStringLiteral("Serial")) {
        YAML::Node serial(YAML::NodeType::Map);
        serialOptionsToYaml(session.serial, serial);
        node["serial"] = serial;
    }
    return node;
}

TerminalAppearance terminalAppearanceFromYaml(const YAML::Node &node)
{
    TerminalAppearance appearance;
    if (!node || !node.IsMap())
        return appearance;
    appearance.colorScheme = QString::fromStdString(node["color_scheme"].as<std::string>(""));
    appearance.fontFamily = QString::fromStdString(node["font_family"].as<std::string>(""));
    appearance.fontSize = qBound(0, node["font_size"].as<int>(0), 72);
    return appearance;
}

// Interface font for menus, panels, and dialogs (terminals have their own).
// An empty family or a size of 0 keeps the system's choice.
struct InterfaceFont
{
    QString family;
    int pointSize = 0;
};

InterfaceFont loadInterfaceFont()
{
    InterfaceFont font;
    try {
        const auto ui = YAML::LoadFile(settingsFilePath().toStdString())["ui"];
        font.family = QString::fromStdString(ui["font_family"].as<std::string>(""));
        font.pointSize = qBound(0, ui["font_size"].as<int>(0), 48);
    } catch (const YAML::Exception &) {
        // Missing or invalid settings keep the system font.
    }
    return font;
}

void saveInterfaceFont(const InterfaceFont &font)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    settings["ui"]["font_family"] = font.family.toStdString();
    settings["ui"]["font_size"] = font.pointSize;

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

void applyInterfaceFont(const QFont &systemFont, const InterfaceFont &choice)
{
    QFont font = systemFont;
    if (!choice.family.isEmpty())
        font.setFamily(choice.family);
    if (choice.pointSize > 0)
        font.setPointSize(choice.pointSize);
    QApplication::setFont(font);
}

bool loadWindowBorderPreference()
{
    try {
        const auto ui = YAML::LoadFile(settingsFilePath().toStdString())["ui"];
        if (ui && ui["window_border"])
            return ui["window_border"].as<bool>();
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the platform default.
    }
    return defaultWindowBorders();
}

void saveWindowBorderPreference(bool enabled)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    settings["ui"]["window_border"] = enabled;

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

QString loadDarkTerminalColorScheme()
{
    try {
        const auto terminal = YAML::LoadFile(settingsFilePath().toStdString())["terminal"];
        if (terminal && terminal["dark_color_scheme"])
            return QString::fromStdString(terminal["dark_color_scheme"].as<std::string>());
    } catch (const YAML::Exception &) {
        // Missing or invalid settings: no separate dark-mode scheme.
    }
    return QString();
}

TerminalAppearance loadTerminalAppearance()
{
    try {
        return terminalAppearanceFromYaml(
            YAML::LoadFile(settingsFilePath().toStdString())["terminal"]);
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the built-in terminal appearance.
    }
    return {};
}

void saveTerminalAppearance(const TerminalAppearance &appearance)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    YAML::Node terminal(YAML::NodeType::Map);
    terminal["color_scheme"] = appearance.colorScheme.toStdString();
    terminal["font_family"] = appearance.fontFamily.toStdString();
    terminal["font_size"] = appearance.fontSize;
    // Keep keys other parts of the application store under terminal:.
    try {
        const auto existing = YAML::LoadFile(path.toStdString())["terminal"];
        if (existing && existing["dark_color_scheme"])
            terminal["dark_color_scheme"] = existing["dark_color_scheme"];
    } catch (const YAML::Exception &) {
    }
    settings["terminal"] = terminal;

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

YAML::Node folderToYaml(const QTreeWidgetItem *folder,
                        const QHash<QString, SessionDefinition> &sessions)
{
    YAML::Node node;
    node["name"] = folder->text(0).toStdString();
    if (const auto jumpHost = folder->data(0, FolderJumpHostRole).toString(); !jumpHost.isEmpty())
        node["jump_host"] = jumpHost.toStdString();
    if (const auto proxy = folder->data(0, FolderProxyRole).toString(); !proxy.isEmpty())
        node["proxy"] = proxy.toStdString();
    YAML::Node sessionNodes(YAML::NodeType::Sequence);
    YAML::Node folderNodes(YAML::NodeType::Sequence);
    for (int index = 0; index < folder->childCount(); ++index) {
        const auto *child = folder->child(index);
        if (child->data(0, ItemKindRole).toString() == QStringLiteral("folder")) {
            folderNodes.push_back(folderToYaml(child, sessions));
            continue;
        }
        const auto session = sessions.constFind(child->data(0, SessionIdRole).toString());
        if (session != sessions.cend())
            sessionNodes.push_back(sessionToYaml(*session));
    }
    if (sessionNodes.size() > 0)
        node["sessions"] = sessionNodes;
    if (folderNodes.size() > 0)
        node["folder"] = folderNodes;
    return node;
}

class SessionPage final : public QWidget
{
public:
    explicit SessionPage(const SessionDefinition &session, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(32, 28, 32, 28);

        auto *title = new QLabel(session.name, this);
        QFont titleFont = title->font();
        titleFont.setPointSize(titleFont.pointSize() + 5);
        titleFont.setBold(true);
        title->setFont(titleFont);

        auto *details = new QLabel(
            QStringLiteral("%1  ·  %2%3")
                .arg(session.protocol, session.host,
                     session.port == 0 ? QString() : QStringLiteral(":%1").arg(session.port)),
            this);
        details->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto *placeholder = new QLabel(
            QStringLiteral("This tab is reserved for one session.\n"
                           "The %1 connection backend has not been implemented yet.")
                .arg(session.protocol),
            this);
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

        layout->addWidget(title);
        layout->addWidget(details);
        layout->addStretch();
        layout->addWidget(placeholder, 0, Qt::AlignCenter);
        layout->addStretch();
    }
};

// Renaming in the session tree: the editor holds just the name (the list
// may show "name (host)"), and the new name goes through MainWindow.
// The session list's look: a status dot before every row's icon (folders
// keep the space, so icons line up), and folder summaries like 3 ● 1 ○
// after the name or in a column of their own.
struct SessionListLook
{
    enum class Dot { None, Unknown, Checking, Up, Down };
    std::function<Dot(const QModelIndex &index)> dotOf;
    std::function<bool()> blinkOn;
    // Folders: (hosts that answer, hosts that do not); nothing for others.
    std::function<std::optional<QPair<int, int>>(const QModelIndex &index)> summaryOf;
    // The column showing folder summaries; 0 means after the name.
    std::function<int()> summaryColumn;

    static constexpr int DotSize = 5;
    static constexpr int DotSpace = DotSize + 7;

    static QColor upColor() { return QColor(0x7c, 0xc5, 0x8a); }
    static QColor downColor() { return QColor(0xe0, 0x8a, 0x8a); }
    static QColor greyColor() { return QColor(0xa0, 0xa0, 0xa0); }

    static void drawDot(QPainter *painter, const QPointF &center, Dot dot, bool blinkOn)
    {
        if (dot == Dot::None)
            return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QColor color = dot == Dot::Up ? upColor() : dot == Dot::Down ? downColor() : greyColor();
        // Sessions that cannot be checked: a short dash, unlike any dot.
        if (dot == Dot::Unknown) {
            painter->setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(QPointF(center.x() - DotSize / 2.0, center.y()),
                              QPointF(center.x() + DotSize / 2.0, center.y()));
            painter->restore();
            return;
        }
        // The shape carries the meaning (filled answers, empty does not), so
        // it reads without telling green from red.
        const QRectF circle(center.x() - DotSize / 2.0, center.y() - DotSize / 2.0, DotSize, DotSize);
        const bool filled = dot == Dot::Up || (dot == Dot::Checking && blinkOn);
        painter->setPen(QPen(color, 1.1));
        painter->setBrush(filled ? QBrush(color) : Qt::NoBrush);
        painter->drawEllipse(circle);
        painter->restore();
    }

    // "3 ● 1 ○" with the status dots; its width with draw = false.
    static int drawSummary(QPainter *painter, const QFontMetrics &metrics, int x, int centerY,
                           QPair<int, int> counts, const QColor &text, bool draw = true)
    {
        if (counts.first + counts.second == 0)
            return 0;
        const auto part = [&](const QString &piece) {
            if (draw) {
                painter->setPen(text);
                painter->drawText(QPoint(x, centerY + (metrics.ascent() - metrics.descent()) / 2), piece);
            }
            x += metrics.horizontalAdvance(piece);
        };
        const auto dot = [&](Dot kind) {
            if (draw)
                drawDot(painter, QPointF(x + DotSize / 2.0 + 1, centerY), kind, false);
            x += DotSize + 2;
        };
        const int start = x;
        if (draw)
            painter->save();
        part(QStringLiteral("%1 ").arg(counts.first));
        dot(Dot::Up);
        part(QStringLiteral(" %1 ").arg(counts.second));
        dot(Dot::Down);
        if (draw)
            painter->restore();
        return x - start;
    }
};

class SessionNameDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    std::function<QString(const QModelIndex &index)> nameOf;
    std::function<void(const QModelIndex &index, const QString &name)> rename;
    SessionListLook look;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        // The row's background (selection, hover) over the dot's space too.
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = QIcon();
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

        if (look.dotOf)
            SessionListLook::drawDot(painter,
                                     QPointF(option.rect.left() + SessionListLook::DotSize / 2.0 + 2,
                                             option.rect.center().y() + 0.5),
                                     look.dotOf(index), look.blinkOn && look.blinkOn());
        QStyleOptionViewItem content = option;
        content.rect.adjust(SessionListLook::DotSpace, 0, 0, 0);
        QStyledItemDelegate::paint(painter, content, index);

        if (look.summaryOf && look.summaryColumn && look.summaryColumn() == 0) {
            if (const auto counts = look.summaryOf(index)) {
                QStyleOptionViewItem text = content;
                initStyleOption(&text, index);
                const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &text, widget);
                const int x = textRect.left() + text.fontMetrics.horizontalAdvance(text.text) + 8;
                if (x < option.rect.right())
                    SessionListLook::drawSummary(painter, text.fontMetrics, x, textRect.center().y(),
                                                 *counts, summaryColor(option));
            }
        }
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.rwidth() += SessionListLook::DotSpace;
        if (look.summaryOf && look.summaryColumn && look.summaryColumn() == 0) {
            if (const auto counts = look.summaryOf(index))
                size.rwidth() += 8 + SessionListLook::drawSummary(nullptr, option.fontMetrics, 0, 0,
                                                                  *counts, QColor(), false);
        }
        return size;
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override
    {
        QStyleOptionViewItem shifted = option;
        shifted.rect.adjust(SessionListLook::DotSpace, 0, 0, 0);
        QStyledItemDelegate::updateEditorGeometry(editor, shifted, index);
    }

    static QColor summaryColor(const QStyleOptionViewItem &option)
    {
        return option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText
                                                                          : QPalette::PlaceholderText);
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (auto *line = qobject_cast<QLineEdit *>(editor)) {
            line->setText(nameOf(index));
            line->selectAll();
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget *editor, QAbstractItemModel *, const QModelIndex &index) const override
    {
        if (auto *line = qobject_cast<QLineEdit *>(editor))
            rename(index, line->text());
    }
};

// The Host, Protocol, and summary columns: plain text, except a folder's
// summary in the column chosen for it.
class SessionColumnDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    SessionListLook look;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        if (!look.summaryOf || !look.summaryColumn || look.summaryColumn() != index.column())
            return;
        if (const auto counts = look.summaryOf(index.siblingAtColumn(0))) {
            QStyleOptionViewItem text = option;
            initStyleOption(&text, index);
            SessionListLook::drawSummary(painter, option.fontMetrics, option.rect.left() + 4,
                                         option.rect.center().y(), *counts,
                                         option.palette.color(option.state & QStyle::State_Selected
                                                                  ? QPalette::HighlightedText
                                                                  : QPalette::PlaceholderText));
        }
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (look.summaryOf && look.summaryColumn && look.summaryColumn() == index.column()) {
            if (const auto counts = look.summaryOf(index.siblingAtColumn(0)))
                size.setWidth(qMax(size.width(), 12 + SessionListLook::drawSummary(
                                                          nullptr, option.fontMetrics, 0, 0, *counts,
                                                          QColor(), false)));
        }
        return size;
    }
};

// Session tree with drag-and-drop moves. canMove() decides whether an item
// may become a child of a new parent (nullptr is the top level); rejected
// targets show the "not allowed" cursor.
class SessionTreeWidget final : public QTreeWidget
{
public:
    explicit SessionTreeWidget(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
        setDragDropMode(QAbstractItemView::InternalMove);
        setDefaultDropAction(Qt::MoveAction);
        setDropIndicatorShown(true);
        // F2 renames in place; a double click opens the session instead.
        setEditTriggers(QAbstractItemView::EditKeyPressed);
    }

    QTreeWidgetItem *item(const QModelIndex &index) const { return itemFromIndex(index); }

    std::function<bool(const QTreeWidgetItem *item, const QTreeWidgetItem *newParent)> canMove;
    std::function<void(QTreeWidgetItem *item)> itemMoved;

protected:
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        QTreeWidget::dragMoveEvent(event);
        if (event->isAccepted() && !dropAllowed(event->position().toPoint()))
            event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
        auto *item = currentItem();
        if (!item || event->source() != this || !dropAllowed(event->position().toPoint())) {
            event->ignore();
            return;
        }
        // Moving re-parents the same item objects, but Qt collapses them.
        QHash<QTreeWidgetItem *, bool> expanded;
        std::function<void(QTreeWidgetItem *)> remember = [&](QTreeWidgetItem *node) {
            expanded.insert(node, node->isExpanded());
            for (int index = 0; index < node->childCount(); ++index)
                remember(node->child(index));
        };
        remember(item);
        QTreeWidget::dropEvent(event);
        for (auto iterator = expanded.cbegin(); iterator != expanded.cend(); ++iterator)
            iterator.key()->setExpanded(iterator.value());
        if (item->parent())
            item->parent()->setExpanded(true);
        setCurrentItem(item);
        if (itemMoved)
            itemMoved(item);
    }

private:
    bool dropAllowed(const QPoint &position) const
    {
        const auto *item = currentItem();
        if (!item || !canMove)
            return false;
        const auto *target = itemAt(position);
        const QTreeWidgetItem *newParent = nullptr;
        switch (dropIndicatorPosition()) {
        case QAbstractItemView::OnItem:
            newParent = target;
            break;
        case QAbstractItemView::AboveItem:
        case QAbstractItemView::BelowItem:
            newParent = target ? target->parent() : nullptr;
            break;
        case QAbstractItemView::OnViewport:
            break;
        }
        for (auto *ancestor = newParent; ancestor; ancestor = ancestor->parent()) {
            if (ancestor == item)
                return false;
        }
        return canMove(item, newParent);
    }
};

class ThemeColorDialog final : public QDialog
{
public:
    explicit ThemeColorDialog(const QHash<QString, QColor> &initialColors,
                              const QPalette &fallbackPalette,
                              QWidget *parent = nullptr)
        : QDialog(parent), m_colors(initialColors)
    {
        setWindowTitle(QStringLiteral("Customize Theme Colors"));
        auto *layout = new QFormLayout(this);
        addColorRow(layout, QStringLiteral("window"), QStringLiteral("Window"),
                    fallbackPalette.color(QPalette::Window));
        addColorRow(layout, QStringLiteral("base"), QStringLiteral("Input and panel background"),
                    fallbackPalette.color(QPalette::Base));
        addColorRow(layout, QStringLiteral("text"), QStringLiteral("Text"),
                    fallbackPalette.color(QPalette::Text));
        addColorRow(layout, QStringLiteral("highlight"), QStringLiteral("Selection and accent"),
                    fallbackPalette.color(QPalette::Highlight));

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    QHash<QString, QColor> colors() const { return m_colors; }

private:
    void addColorRow(QFormLayout *layout, const QString &key, const QString &label,
                     const QColor &fallback)
    {
        auto color = m_colors.value(key, fallback);
        auto *button = new QPushButton(color.name(QColor::HexRgb), this);
        auto updateButton = [button](const QColor &value) {
            button->setText(value.name(QColor::HexRgb));
            button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                                      .arg(value.name(QColor::HexRgb),
                                           value.lightness() < 128 ? QStringLiteral("white")
                                                                   : QStringLiteral("black")));
        };
        updateButton(color);
        connect(button, &QPushButton::clicked, this, [this, key, color, button, updateButton]() mutable {
            const auto selected = QColorDialog::getColor(color, this,
                                                         QStringLiteral("Choose %1 color").arg(key));
            if (selected.isValid()) {
                color = selected;
                m_colors.insert(key, color);
                updateButton(color);
            }
        });
        layout->addRow(label, button);
    }

    QHash<QString, QColor> m_colors;
};
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_vault(stateFilePath(QStringLiteral("credentials.vault")))
{
    migrateStateFromDataDirectory();
    // A new installation starts with the OpsRemote Light Orange look, matching the icon.
    const bool firstStart = !QFileInfo::exists(settingsFilePath());
    SftpPanel::removeStaleEditFolders();
    m_systemStyleKey = QApplication::style()->objectName();
    m_systemPalette = QApplication::palette();
    m_systemFont = QApplication::font();
    applyInterfaceFont(m_systemFont, loadInterfaceFont());
    setWindowBordersEnabled(loadWindowBorderPreference());
    initializeIconThemes();
    m_iconTheme = loadIconThemePreference();
    bool knownIconTheme = false;
    for (const auto &theme : availableIconThemes())
        knownIconTheme = knownIconTheme || theme.first == m_iconTheme;
    if (!knownIconTheme)
        m_iconTheme.clear();
    setIconTheme(m_iconTheme);
    m_selectedStyleKey = loadUiStylePreference();
    m_colorThemeKey = loadColorThemePreference();
    if (m_colorThemeKey != QStringLiteral("system")
        && m_colorThemeKey != QStringLiteral("light")
        && m_colorThemeKey != QStringLiteral("dark")
        && m_colorThemeKey != QStringLiteral("custom")
        && m_colorThemeKey != QStringLiteral("auto")
        && !findPalettePreset(m_colorThemeKey))
        m_colorThemeKey = QStringLiteral("system");
    m_customColors = loadCustomColors();
    m_userLooks = loadUserLooks();
    m_terminalAppearance = loadTerminalAppearance();
    m_darkTerminalColorScheme = loadDarkTerminalColorScheme();
    m_behaviour = loadTerminalBehaviour();
    m_startup = loadStartupPreferences();
    m_shortcuts = loadShortcuts();
    m_security = loadSecurityPreferences();
    m_logging = loadLoggingPreferences();
    m_presentation = loadPresentationPreferences();
    m_highlightRules = loadHighlightRules();
    m_snippets = loadSnippets();
    // A copy of settings/ per start, so a bad edit or import can be undone.
    if (const auto backup = loadBackupPreferences(); backup.backupOnStart) {
        createBackup();
        pruneBackups(backup.keep);
    }
    // "Automatic" colour mode follows the OS light/dark setting live.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_colorThemeKey == QStringLiteral("auto")) {
            applyColorTheme(m_colorThemeKey);
            applyTerminalAppearanceToTabs();
        }
    });
    applyUiStyle(m_selectedStyleKey);

    setWindowTitle(QStringLiteral("OpsRemote"));
    resize(1100, 720);

    // Fixed layout: the left panel and tabs share a splitter and cannot be
    // undocked, floated, or rearranged.
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);

    m_leftTabs = new QTabWidget(splitter);
    m_leftTabs->setDocumentMode(true);
    m_leftTabs->setMinimumWidth(220);
    auto *sessionTree = new SessionTreeWidget(m_leftTabs);
    auto *nameDelegate = new SessionNameDelegate(sessionTree);
    nameDelegate->nameOf = [this, sessionTree](const QModelIndex &index) {
        auto *item = sessionTree->item(index);
        if (!item || isFolder(item))
            return item ? item->text(0) : QString();
        return m_sessions.value(item->data(0, SessionIdRole).toString()).name;
    };
    nameDelegate->rename = [this, sessionTree](const QModelIndex &index, const QString &name) {
        auto *item = sessionTree->item(index);
        // A copy in Pinned or Recent renames the session itself.
        if (item && item->data(0, ItemKindRole).toString() == QStringLiteral("clone"))
            item = realSessionItem(item->data(0, SessionIdRole).toString());
        if (item)
            renameTreeItem(item, name);
    };
    SessionListLook look;
    look.dotOf = [this, sessionTree](const QModelIndex &index) {
        auto *item = sessionTree->item(index);
        if (!item || isFolder(item) || item->data(0, ItemKindRole).toString() == QStringLiteral("quick"))
            return SessionListLook::Dot::None;
        if (!m_sessionList.checkStatus)
            return SessionListLook::Dot::Unknown;
        switch (m_hostStatus->status(item->data(0, SessionIdRole).toString())) {
        case HostStatusChecker::Status::Up:
            return SessionListLook::Dot::Up;
        case HostStatusChecker::Status::Down:
            return SessionListLook::Dot::Down;
        case HostStatusChecker::Status::Checking:
            return SessionListLook::Dot::Checking;
        case HostStatusChecker::Status::Unknown:
            break;
        }
        return SessionListLook::Dot::Unknown;
    };
    look.blinkOn = [this] { return m_blinkOn; };
    look.summaryOf = [this, sessionTree](const QModelIndex &index) -> std::optional<QPair<int, int>> {
        auto *item = sessionTree->item(index);
        if (!m_sessionList.checkStatus || !item
            || (!isFolder(item) && item->data(0, ItemKindRole).toString() != QStringLiteral("quick")))
            return std::nullopt;
        return folderStatusCounts(item);
    };
    look.summaryColumn = [this] { return summaryColumn(); };
    nameDelegate->look = look;
    sessionTree->setItemDelegate(nameDelegate);
    auto *columnDelegate = new SessionColumnDelegate(sessionTree);
    columnDelegate->look = look;
    for (const int column : {HostColumn, ProtocolColumn, SummaryColumn})
        sessionTree->setItemDelegateForColumn(column, columnDelegate);
    sessionTree->canMove = [this](const QTreeWidgetItem *item, const QTreeWidgetItem *newParent) {
        // Pinned, Recent, and their copies stay where they are.
        if (newParent && newParent->data(0, ItemKindRole).toString() == QStringLiteral("quick"))
            return false;
        // Sessions go in a folder or the top level; folders keep the nesting limit.
        if (!isFolder(item))
            return !newParent || isFolder(newParent);
        if (newParent && !isFolder(newParent))
            return false;
        std::function<int(const QTreeWidgetItem *)> subtreeDepth = [&](const QTreeWidgetItem *node) {
            int deepest = 0;
            for (int index = 0; index < node->childCount(); ++index) {
                if (isFolder(node->child(index)))
                    deepest = std::max(deepest, subtreeDepth(node->child(index)));
            }
            return deepest + 1;
        };
        return (newParent ? folderDepth(newParent) : 0) + subtreeDepth(item) <= MaximumFolderDepth;
    };
    sessionTree->itemMoved = [this](QTreeWidgetItem *item) {
        saveSessions();
        saveFolderExpansion();
        statusBar()->showMessage(QStringLiteral("Moved %1").arg(item->text(0)), 3000);
    };
    m_sessionTree = sessionTree;
    m_sessionTree->setColumnCount(4);
    m_sessionTree->setHeaderLabels({QStringLiteral("Saved sessions"), QStringLiteral("Host"),
                                    QStringLiteral("Protocol"), QString()});
    m_sessionList = loadSessionListPreferences();
    m_localShell = loadLocalShellPreferences();
    // Each password's store: the session's own choice, else the Preferences'.
    m_vault.setStoreRule([this](const QString &key) {
        const auto session = m_sessions.constFind(key.section(QLatin1Char('#'), 0, 0));
        QString choice = session != m_sessions.cend() ? session->passwordStore : QString();
        if (choice.isEmpty())
            choice = m_security.passwordStore;
        return choice == QStringLiteral("system") ? PasswordStore::System
               : choice == QStringLiteral("both") ? PasswordStore::Both
                                                  : PasswordStore::Vault;
    });
    m_recentSessions = loadRecentSessions();
    m_hostStatus = new HostStatusChecker(this);
    connect(m_hostStatus, &HostStatusChecker::changed, this, [this] {
        // The dot blinks (hollow, filled) while its check runs.
        if (m_hostStatus->isChecking() && !m_blinkTimer->isActive())
            m_blinkTimer->start();
        m_sessionTree->viewport()->update();
        fitSessionPanel();
    });
    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(500);
    connect(m_blinkTimer, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        if (!m_hostStatus->isChecking()) {
            m_blinkTimer->stop();
            m_blinkOn = false;
        }
        m_sessionTree->viewport()->update();
    });
    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::checkHostStatus);
    m_sessionTree->setContextMenuPolicy(Qt::CustomContextMenu);
    // The Sessions tab: a search box above the list.
    auto *sessionsPage = new QWidget(m_leftTabs);
    auto *sessionsLayout = new QVBoxLayout(sessionsPage);
    sessionsLayout->setContentsMargins(0, 4, 0, 0);
    sessionsLayout->setSpacing(4);
    m_sessionSearch = new QLineEdit(sessionsPage);
    m_sessionSearch->setObjectName(QStringLiteral("sessionSearch"));
    m_sessionSearch->setPlaceholderText(QStringLiteral("Search sessions"));
    m_sessionSearch->setClearButtonEnabled(true);
    m_sessionSearch->setToolTip(QStringLiteral(
        "Parts of a name, host, user, protocol, or folder; every word must match.\n"
        "Down: go to the results. Enter: open the first one. Esc: clear."));
    m_sessionSearch->installEventFilter(this);
    connect(m_sessionSearch, &QLineEdit::textChanged, this, &MainWindow::applySessionFilter);
    m_noSessionMatches = new QLabel(QStringLiteral("No session matches."), sessionsPage);
    m_noSessionMatches->setAlignment(Qt::AlignCenter);
    m_noSessionMatches->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    m_noSessionMatches->hide();
    sessionsLayout->addWidget(m_sessionSearch);
    sessionsLayout->addWidget(m_noSessionMatches);
    sessionsLayout->addWidget(m_sessionTree, 1);
    m_leftTabs->addTab(sessionsPage, QStringLiteral("Sessions"));

    // Each SSH tab owns its own SFTP panel; the stack shows the active one.
    m_sftpStack = new QStackedWidget(m_leftTabs);
    m_sftpPlaceholder = new QLabel(QStringLiteral("Open an SSH session to browse its files here."),
                                   m_sftpStack);
    m_sftpPlaceholder->setAlignment(Qt::AlignCenter);
    m_sftpPlaceholder->setWordWrap(true);
    m_sftpPlaceholder->setMargin(12);
    m_sftpPlaceholder->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    m_sftpStack->addWidget(m_sftpPlaceholder);
    // "Files": SFTP, SCP or FTP; the panel's header says which.
    m_leftTabs->addTab(m_sftpStack, QStringLiteral("Files"));

    m_sessionTabs = new QTabWidget(splitter);
    m_sessionTabs->setTabsClosable(true);
    m_sessionTabs->setDocumentMode(true);
    // Session tabs can be dragged into a different order; they stay inside
    // the window (no detaching).
    m_sessionTabs->setMovable(true);
    connect(m_sessionTabs, &QTabWidget::tabCloseRequested,
            this, &MainWindow::closeSessionTab);
    connect(m_sessionTabs, &QTabWidget::currentChanged,
            this, &MainWindow::showSftpPanelForCurrentTab);
    // The current tab is no longer marked as having new output.
    connect(m_sessionTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index >= 0)
            m_sessionTabs->tabBar()->setTabTextColor(index, QColor());
    });
    m_sessionTabs->tabBar()->installEventFilter(this);
    m_sessionTabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sessionTabs->tabBar(), &QWidget::customContextMenuRequested,
            this, &MainWindow::showTabContextMenu);

    splitter->addWidget(m_leftTabs);
    splitter->addWidget(m_sessionTabs);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({300, 800});
    connect(m_sessionTree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item) {
                if (item && !isFolder(item))
                    openSelectedSession();
            });
    connect(m_sessionTree, &QTreeWidget::customContextMenuRequested,
            this, &MainWindow::showTreeContextMenu);

    createMenus();
    createToolbar();
    loadSessions();
    restoreFolderExpansion();
    refreshSessionList();
    applySessionListPreferences();
    m_tunnelsWindow = new TunnelsWindow(
        [this](const SavedTunnel &tunnel, SshConnectionSettings *settings) {
            return resolveTunnel(tunnel, settings);
        },
        [this] { return jumpHostChoices(); }, [this](QWidget *over) { return createSshSession(over); },
        [this](const QString &server, const QStringList &via) { return describeTunnelServer(server, via); },
        [this](const QString &sessionId) { return openTabConnection(sessionId); },
        [this](const QString &host) {
            if (!m_networkTools)
                m_networkTools = new NetworkToolsWindow(this);
            m_networkTools->showPortScan(host);
        },
        this);
    // Once the main window is up, so prompts have a parent to show over.
    QTimer::singleShot(0, m_tunnelsWindow, [this] { m_tunnelsWindow->startAutomatic(); });
    connect(m_sessionTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *) {
        if (!m_filtering)
            saveFolderExpansion();
        fitSessionPanel();
    });
    connect(m_sessionTree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *) {
        if (!m_filtering)
            saveFolderExpansion();
        fitSessionPanel();
    });

    statusBar()->showMessage(QStringLiteral("Ready — double-click a saved session to open its tab"));

    if (m_startup.rememberWindow) {
        const auto geometry = loadWindowGeometry();
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
    }
    // After the window is shown, so a vault prompt has a parent on screen.
    QTimer::singleShot(0, this, &MainWindow::restoreStartupTabs);

    // Auto-lock: any keyboard or mouse input in the application counts as activity.
    m_lastInput.start();
    qApp->installEventFilter(this);
    auto *idleCheck = new QTimer(this);
    connect(idleCheck, &QTimer::timeout, this, [this] {
        if (m_security.autoLockMinutes > 0 && m_vault.isUnlocked()
            && m_lastInput.elapsed() > qint64(m_security.autoLockMinutes) * 60000)
            lockVaultFor(QStringLiteral("after %1 min without activity").arg(m_security.autoLockMinutes));
    });
    idleCheck->start(15000);

    m_broadcastIndicator = new QLabel(QStringLiteral("  BROADCAST  "), this);
    m_broadcastIndicator->setStyleSheet(QStringLiteral(
        "background: #c0392b; color: white; font-weight: bold; border-radius: 3px;"));
    m_broadcastIndicator->setToolTip(QStringLiteral("Typing goes to every connected SSH tab"));
    m_broadcastIndicator->hide();
    statusBar()->addPermanentWidget(m_broadcastIndicator);
    m_multiExecIndicator = new QToolButton(this);
    m_multiExecIndicator->setAutoRaise(true);
    m_multiExecIndicator->setStyleSheet(QStringLiteral(
        "QToolButton { background: palette(highlight); color: palette(highlighted-text); "
        "font-weight: bold; border-radius: 3px; padding: 0 6px; }"));
    m_multiExecIndicator->setToolTip(QStringLiteral("Typing into a chosen terminal goes to all chosen "
                                                    "terminals. Click to stop."));
    m_multiExecIndicator->hide();
    connect(m_multiExecIndicator, &QToolButton::clicked, this, [this] {
        for (auto *page : allTerminalPages())
            page->setMultiExecTarget(false);
        updateMultiExecIndicator();
    });
    statusBar()->addPermanentWidget(m_multiExecIndicator);

    if (firstStart) {
        for (const auto &look : builtInLooks()) {
            if (look.id == QStringLiteral("builtin:opsremote-light-orange"))
                applyLook(look);
        }
    }
}

void MainWindow::createMenus()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto *localTerminalAction = fileMenu->addAction(QStringLiteral("New Local Terminal"));
    setThemedIcon(localTerminalAction, QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    bindShortcut(localTerminalAction, QStringLiteral("new_local_terminal"));
    connect(localTerminalAction, &QAction::triggered, this, &MainWindow::openLocalTerminal);
    disableLocalTerminalWhereUnsupported(localTerminalAction);
#ifdef Q_OS_WIN
    // Through UAC, in a tab like any other (see openLocalTerminalAsAdministrator()).
    auto *administratorAction = fileMenu->addAction(QStringLiteral("New Local Terminal (Administrator)"));
    administratorAction->setIcon(style()->standardIcon(QStyle::SP_VistaShield));
    connect(administratorAction, &QAction::triggered, this, &MainWindow::openLocalTerminalAsAdministrator);
    disableLocalTerminalWhereUnsupported(administratorAction);
#endif
    auto *newSessionAction = fileMenu->addAction(QStringLiteral("New Session…"));
    setThemedIcon(newSessionAction, QStringLiteral("document-new"), QStyle::SP_FileIcon);
    bindShortcut(newSessionAction, QStringLiteral("new_session"));
    connect(newSessionAction, &QAction::triggered, this, &MainWindow::createSession);
    fileMenu->addSeparator();
    auto *exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    setThemedIcon(exitAction, QStringLiteral("application-exit"), QStyle::SP_DialogCloseButton);
    bindShortcut(exitAction, QStringLiteral("quit"));
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    auto *sessionsMenu = menuBar()->addMenu(QStringLiteral("&Sessions"));
    auto *newFolderAction = sessionsMenu->addAction(QStringLiteral("New Folder…"));
    setThemedIcon(newFolderAction, QStringLiteral("folder-new"), QStyle::SP_FileDialogNewFolder);
    connect(newFolderAction, &QAction::triggered, this, &MainWindow::createFolder);
    auto *deleteFolderAction = sessionsMenu->addAction(QStringLiteral("Delete…"));
    setThemedIcon(deleteFolderAction, QStringLiteral("edit-delete"), QStyle::SP_TrashIcon);
    bindShortcut(deleteFolderAction, QStringLiteral("delete_folder"));
    connect(deleteFolderAction, &QAction::triggered, this, &MainWindow::deleteSelectedItem);
    sessionsMenu->addAction(QStringLiteral("Folder Settings…"), this, [this] {
        editFolderSettings(selectedFolder());
    });
    auto *searchAction = sessionsMenu->addAction(QStringLiteral("Search Sessions"));
    setThemedIcon(searchAction, QStringLiteral("edit-find"), QStyle::SP_FileDialogContentsView);
    bindShortcut(searchAction, QStringLiteral("search_sessions"));
    connect(searchAction, &QAction::triggered, this, &MainWindow::focusSessionSearch);
    auto *tunnelsAction = sessionsMenu->addAction(QStringLiteral("SSH Tunnels…"));
    setThemedIcon(tunnelsAction, QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    connect(tunnelsAction, &QAction::triggered, this, &MainWindow::showTunnels);
    auto *addSessionAction = sessionsMenu->addAction(QStringLiteral("New Session…"));
    setThemedIcon(addSessionAction, QStringLiteral("list-add"), QStyle::SP_FileIcon);
    connect(addSessionAction, &QAction::triggered, this, &MainWindow::createSession);
    auto *editSessionAction = sessionsMenu->addAction(QStringLiteral("Edit…"));
    setThemedIcon(editSessionAction, QStringLiteral("edit-rename"), QStyle::SP_FileDialogDetailedView);
    bindShortcut(editSessionAction, QStringLiteral("edit_session"));
    connect(editSessionAction, &QAction::triggered, this, &MainWindow::editSelectedSession);
    sessionsMenu->addSeparator();
    auto *openAction = sessionsMenu->addAction(QStringLiteral("Open"));
    setThemedIcon(openAction, QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay);
    bindShortcut(openAction, QStringLiteral("open_session"));
    connect(openAction, &QAction::triggered, this, &MainWindow::openSelectedSession);
    // Rename (F2 in the session list) and Duplicate act on the selection.
    sessionsMenu->addAction(QStringLiteral("Rename"), this, [this] {
        if (auto *item = selectedItem())
            m_sessionTree->editItem(item, 0);
    });
    auto *duplicateAction = sessionsMenu->addAction(QStringLiteral("Duplicate"), this, [this] {
        duplicateSession(selectedItem());
    });
    connect(sessionsMenu, &QMenu::aboutToShow, this, [this, duplicateAction] {
        auto *item = selectedItem();
        duplicateAction->setEnabled(item && !isFolder(item));
    });
    auto *snippetsMenu = sessionsMenu->addMenu(QStringLiteral("Send Snippet"));
    snippetsMenu->setIcon(themedIcon(QStringLiteral("insert-text"), QStyle::SP_FileDialogContentsView));
    connect(snippetsMenu, &QMenu::aboutToShow, this, [this, snippetsMenu] {
        snippetsMenu->clear();
        auto *page = activeTerminal(m_sessionTabs->currentWidget());
        for (const auto &snippet : std::as_const(m_snippets)) {
            auto *action = snippetsMenu->addAction(snippet.name, this, [page, snippet] {
                if (page)
                    page->typeText(snippet.text + (snippet.pressEnter ? QStringLiteral("\r") : QString()));
            });
            action->setEnabled(page);
        }
        if (!m_snippets.isEmpty())
            snippetsMenu->addSeparator();
        snippetsMenu->addAction(QStringLiteral("Edit Snippets…"), this,
                                [this] { showPreferences(QStringLiteral("snippets")); });
    });
    sessionsMenu->addSeparator();
    auto *importAction = sessionsMenu->addAction(QStringLiteral("Import from YAML…"));
    setThemedIcon(importAction, QStringLiteral("document-import"), QStyle::SP_DialogOpenButton);
    connect(importAction, &QAction::triggered, this, &MainWindow::importSessions);
    // Sessions saved in other programs, each in a folder of its own.
    auto *importFrom = sessionsMenu->addMenu(QStringLiteral("Import From"));
    importFrom->addAction(QStringLiteral("OpenSSH Config (~/.ssh/config)…"), this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import OpenSSH Config"), SessionImport::defaultOpenSshConfigPath());
        if (!path.isEmpty())
            importResult(QStringLiteral("OpenSSH"), SessionImport::fromOpenSshConfig(path));
    });
    importFrom->addAction(QStringLiteral("PuTTY"), this, [this] {
        importResult(QStringLiteral("PuTTY"), SessionImport::fromPutty());
    });
    importFrom->addAction(QStringLiteral("MobaXterm (.mxtsessions, MobaXterm.ini)…"), this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import MobaXterm Sessions"), QString(),
            QStringLiteral("MobaXterm sessions (*.mxtsessions *.ini);;All files (*)"));
        if (!path.isEmpty())
            importResult(QStringLiteral("MobaXterm"), SessionImport::fromMobaXterm(path));
    });
    importFrom->addAction(QStringLiteral("mRemoteNG (confCons.xml)…"), this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import mRemoteNG Connections"), QString(),
            QStringLiteral("mRemoteNG connections (*.xml);;All files (*)"));
        if (!path.isEmpty())
            importResult(QStringLiteral("mRemoteNG"), SessionImport::fromMRemoteNg(path));
    });
    importFrom->addAction(QStringLiteral("Remmina…"), this, [this] {
        const auto folder = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Remmina Connections Folder"), SessionImport::defaultRemminaFolder());
        if (!folder.isEmpty())
            importResult(QStringLiteral("Remmina"), SessionImport::fromRemmina(folder));
    });
    auto *exportAction = sessionsMenu->addAction(QStringLiteral("Export to YAML…"));
    setThemedIcon(exportAction, QStringLiteral("document-export"), QStyle::SP_DialogSaveButton);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportSessions);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    auto *sessionsPanelAction = viewMenu->addAction(QStringLiteral("Sessions and Files Panel"));
    sessionsPanelAction->setCheckable(true);
    sessionsPanelAction->setChecked(true);
    connect(sessionsPanelAction, &QAction::toggled, m_leftTabs, &QWidget::setVisible);
    bindShortcut(sessionsPanelAction, QStringLiteral("toggle_side_panel"));
    auto *fullScreenAction = viewMenu->addAction(QStringLiteral("Full Screen"));
    fullScreenAction->setCheckable(true);
    setThemedIcon(fullScreenAction, QStringLiteral("view-fullscreen"), QStyle::SP_TitleBarMaxButton);
    bindShortcut(fullScreenAction, QStringLiteral("full_screen"));
    m_presentationAction = viewMenu->addAction(QStringLiteral("Presentation Mode"));
    m_presentationAction->setCheckable(true);
    setThemedIcon(m_presentationAction, QStringLiteral("video-display"), QStyle::SP_DesktopIcon);
    bindShortcut(m_presentationAction, QStringLiteral("presentation"));
    connect(m_presentationAction, &QAction::toggled, this, &MainWindow::setPresentationMode);
    m_broadcastAction = viewMenu->addAction(QStringLiteral("Broadcast Typing to All SSH Tabs"));
    m_broadcastAction->setCheckable(true);
    setThemedIcon(m_broadcastAction, QStringLiteral("network-transmit"), QStyle::SP_ArrowRight);
    bindShortcut(m_broadcastAction, QStringLiteral("broadcast"));
    connect(m_broadcastAction, &QAction::toggled, this, &MainWindow::setBroadcast);
    auto *desktopFullScreen = viewMenu->addAction(QStringLiteral("Remote Desktop Full Screen"));
    desktopFullScreen->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Return));
    desktopFullScreen->setToolTip(QStringLiteral("Shows the current remote desktop tab on the whole "
                                                 "screen (or every monitor, if its options say so)."));
    connect(desktopFullScreen, &QAction::triggered, this, [this] {
        if (auto *screen = qobject_cast<RemoteScreenPage *>(m_sessionTabs->currentWidget()))
            screen->setFullScreen(true);
    });
    connect(viewMenu, &QMenu::aboutToShow, this, [this, desktopFullScreen] {
        desktopFullScreen->setEnabled(qobject_cast<RemoteScreenPage *>(m_sessionTabs->currentWidget()));
    });
    connect(fullScreenAction, &QAction::toggled, this, [this](bool on) {
        setWindowState(on ? windowState() | Qt::WindowFullScreen
                          : windowState() & ~Qt::WindowFullScreen);
    });

    // Tab actions live on the window so their shortcuts work from any tab.
    // The Tabs menu: its actions also work by shortcut anywhere in the window.
    auto *tabsMenu = menuBar()->addMenu(QStringLiteral("T&abs"));
    auto addTabAction = [this, tabsMenu](const QString &text, const QString &id, auto slot) {
        auto *action = new QAction(text, this);
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
        tabsMenu->addAction(action);
        bindShortcut(action, id);
        connect(action, &QAction::triggered, this, slot);
    };
    addTabAction(QStringLiteral("Next Tab"), QStringLiteral("next_tab"), [this] {
        if (m_sessionTabs->count() > 1)
            m_sessionTabs->setCurrentIndex((m_sessionTabs->currentIndex() + 1) % m_sessionTabs->count());
    });
    addTabAction(QStringLiteral("Previous Tab"), QStringLiteral("previous_tab"), [this] {
        if (m_sessionTabs->count() > 1)
            m_sessionTabs->setCurrentIndex((m_sessionTabs->currentIndex() + m_sessionTabs->count() - 1)
                                           % m_sessionTabs->count());
    });
    // In a split tab, the active pane closes first (as in Terminator).
    addTabAction(QStringLiteral("Close Tab or Pane"), QStringLiteral("close_tab"), [this] {
        auto *split = qobject_cast<SplitTab *>(m_sessionTabs->currentWidget());
        if (split && split->activePane())
            closePane(split->activePane());
        else if (m_sessionTabs->currentIndex() >= 0)
            closeSessionTab(m_sessionTabs->currentIndex());
    });
    addTabAction(QStringLiteral("Multi-exec…"), QStringLiteral("multi_exec"),
                 [this] { chooseMultiExecTargets(); });
    // The plain splits open the same session at once; "With…" asks.
    addTabAction(QStringLiteral("Split Right"), QStringLiteral("split_right"),
                 [this] { splitActivePane(Qt::Horizontal, QStringLiteral("same")); });
    addTabAction(QStringLiteral("Split Down"), QStringLiteral("split_down"),
                 [this] { splitActivePane(Qt::Vertical, QStringLiteral("same")); });
    tabsMenu->addAction(QStringLiteral("Split Right With…"), this,
                        [this] { splitActivePane(Qt::Horizontal, QStringLiteral("choose")); });
    tabsMenu->addAction(QStringLiteral("Split Down With…"), this,
                        [this] { splitActivePane(Qt::Vertical, QStringLiteral("choose")); });
    addTabAction(QStringLiteral("Duplicate Tab"), QStringLiteral("duplicate_tab"), [this] {
        if (m_sessionTabs->currentIndex() >= 0)
            duplicateTab(m_sessionTabs->currentIndex());
    });

    // Looks: quick switching here; every setting is in Preferences → Appearance.
    auto *lookMenu = viewMenu->addMenu(QStringLiteral("Look"));
    connect(lookMenu, &QMenu::aboutToShow, this, [this, lookMenu] {
        lookMenu->clear();
        auto *group = new QActionGroup(lookMenu);
        const QString current = matchingLookId();
        bool separated = false;
        for (const auto &look : looks()) {
            if (!look.isBuiltIn() && !separated) {
                lookMenu->addSeparator();
                separated = true;
            }
            auto *action = lookMenu->addAction(look.name, this, [this, look] { applyLook(look); });
            action->setCheckable(true);
            action->setChecked(look.id == current);
            group->addAction(action);
        }
        lookMenu->addSeparator();
        auto *custom = lookMenu->addAction(QStringLiteral("Custom"));
        custom->setCheckable(true);
        custom->setChecked(current.isEmpty());
        custom->setEnabled(false);
    });
    viewMenu->addAction(QStringLiteral("Appearance…"), this,
                        [this] { showPreferences(QStringLiteral("appearance")); });

    auto *toolsMenu = menuBar()->addMenu(QStringLiteral("T&ools"));
    auto *tunnelsTool = toolsMenu->addAction(QStringLiteral("SSH Tunnels…"));
    setThemedIcon(tunnelsTool, QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    connect(tunnelsTool, &QAction::triggered, this, &MainWindow::showTunnels);
    auto *runCommand = toolsMenu->addAction(QStringLiteral("Run Command on Hosts…"));
    runCommand->setToolTip(QStringLiteral("On the SSH sessions of the selected folder, or on all of them"));
    setThemedIcon(runCommand, QStringLiteral("system-run"), QStyle::SP_MediaPlay);
    connect(runCommand, &QAction::triggered, this, [this] {
        auto *item = selectedItem();
        showRunCommand(item && isFolder(item) ? item : nullptr);
    });
    auto *keyManager = toolsMenu->addAction(QStringLiteral("SSH Keys…"));
    setThemedIcon(keyManager, QStringLiteral("dialog-password"), QStyle::SP_DialogApplyButton);
    connect(keyManager, &QAction::triggered, this, &MainWindow::showKeyManager);
    auto *localServers = toolsMenu->addAction(QStringLiteral("Local Servers (TFTP, HTTP)…"));
    setThemedIcon(localServers, QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    connect(localServers, &QAction::triggered, this, [this] {
        // Made when first needed; its servers run only while it is open.
        if (!m_localServersWindow)
            m_localServersWindow = new LocalServersWindow(this);
        m_localServersWindow->show();
        m_localServersWindow->raise();
        m_localServersWindow->activateWindow();
    });
    auto *networkTools = toolsMenu->addAction(QStringLiteral("Network Tools…"));
    setThemedIcon(networkTools, QStringLiteral("network-wired"), QStyle::SP_DriveNetIcon);
    connect(networkTools, &QAction::triggered, this, &MainWindow::showNetworkTools);
    toolsMenu->addSeparator();
    auto *playRecording = toolsMenu->addAction(QStringLiteral("Play Recording…"));
    setThemedIcon(playRecording, QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay);
    connect(playRecording, &QAction::triggered, this, [this] {
        const auto folder = sessionLogFolder(m_logging);
        const auto path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Play Recording"), folder,
            QStringLiteral("asciinema recordings (*.cast);;All files (*)"));
        if (path.isEmpty())
            return;
        auto *player = new CastPlayer(this);
        QString error;
        if (!player->load(path, &error)) {
            delete player;
            QMessageBox::warning(this, QStringLiteral("Play Recording"), error);
            return;
        }
        player->show();
    });

    auto *settingsMenu = menuBar()->addMenu(QStringLiteral("Se&ttings"));
    // One item that locks or unlocks (or creates) the vault, depending on its state.
    auto *vaultAction = settingsMenu->addAction(QString());
    connect(vaultAction, &QAction::triggered, this, [this] {
        if (m_vault.isUnlocked()) {
            lockVaultFor(QString());
        } else {
            ensureVaultUnlocked();
        }
    });
    auto updateVaultAction = [this, vaultAction] {
        const bool unlocked = m_vault.isUnlocked();
        vaultAction->setText(unlocked ? QStringLiteral("Lock Vault")
                             : m_vault.exists() ? QStringLiteral("Unlock Vault…")
                                                : QStringLiteral("Create Credential Vault…"));
        vaultAction->setIcon(unlocked
                                 ? themedIcon(QStringLiteral("object-locked"), QStyle::SP_DialogCancelButton)
                                 : themedIcon(QStringLiteral("object-unlocked"), QStyle::SP_DialogOkButton));
    };
    connect(settingsMenu, &QMenu::aboutToShow, vaultAction, updateVaultAction);
    bindShortcut(vaultAction, QStringLiteral("lock_vault"));
    updateVaultAction();
    settingsMenu->addSeparator();
    auto *preferencesAction = settingsMenu->addAction(QStringLiteral("Preferences…"));
    setThemedIcon(preferencesAction, QStringLiteral("preferences-system"), QStyle::SP_FileDialogDetailedView);
    // QKeySequence::Preferences is only defined on macOS; Ctrl+, everywhere.
    bindShortcut(preferencesAction, QStringLiteral("preferences"));
    connect(preferencesAction, &QAction::triggered, this, [this] { showPreferences(); });

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    auto *aboutAction = helpMenu->addAction(QStringLiteral("About OpsRemote"));
    setThemedIcon(aboutAction, QStringLiteral("help-about"), QStyle::SP_MessageBoxInformation);
    connect(aboutAction, &QAction::triggered, this, [this] {
        showPreferences(QStringLiteral("about"));
    });
}

void MainWindow::applyUiStyle(const QString &styleKey)
{
    QString key = styleKey;
    if (key == QStringLiteral("system"))
        key = m_systemStyleKey;

    QStyle *style = key.compare(OpsRemoteStyle::key(), Qt::CaseInsensitive) == 0
                        ? new OpsRemoteStyle
                    : KvantumThemes::isKvantumKey(key) ? KvantumThemes::create(key)
                                                        : QStyleFactory::create(key);
    if (!style && key.compare(m_systemStyleKey, Qt::CaseInsensitive) != 0)
        style = QStyleFactory::create(m_systemStyleKey);
    if (!style)
        return;

    QApplication::setStyle(style);
    applyColorTheme(m_colorThemeKey);
}

void MainWindow::applyColorTheme(const QString &requestedKey)
{
    // A Kvantum theme brings its own colours: "System" uses those.
    if (requestedKey == QStringLiteral("system") && KvantumThemes::isKvantumKey(m_selectedStyleKey)) {
        QApplication::setPalette(QApplication::style()->standardPalette());
        return;
    }
    QApplication::setPalette(paletteFor(requestedKey, m_customColors));
}

// The palette of a colour mode, without applying it (also for previews).
QPalette MainWindow::paletteFor(const QString &requestedKey,
                                const QHash<QString, QColor> &customColors) const
{
    // Automatic: the light or dark palette, following the OS setting.
    const QString themeKey = requestedKey != QStringLiteral("auto")
        ? requestedKey
        : QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
            ? QStringLiteral("dark")
            : QStringLiteral("light");
    QPalette palette = m_systemPalette;
    if (themeKey == QStringLiteral("dark")) {
        const QColor window(45, 45, 48);
        const QColor base(30, 30, 30);
        const QColor text(235, 235, 235);
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, base);
        palette.setColor(QPalette::AlternateBase, QColor(45, 45, 48));
        palette.setColor(QPalette::ToolTipBase, QColor(245, 245, 245));
        palette.setColor(QPalette::ToolTipText, QColor(25, 25, 25));
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, window);
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Link, QColor(80, 160, 255));
        palette.setColor(QPalette::LinkVisited, QColor(190, 120, 220));
        palette.setColor(QPalette::PlaceholderText, QColor(150, 150, 150));
    } else if (const auto *preset = findPalettePreset(themeKey)) {
        const QColor text(preset->text);
        palette.setColor(QPalette::Window, QColor(preset->window));
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, QColor(preset->base));
        palette.setColor(QPalette::AlternateBase, QColor(preset->alternateBase));
        palette.setColor(QPalette::ToolTipBase, QColor(preset->base));
        palette.setColor(QPalette::ToolTipText, text);
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, QColor(preset->button));
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Highlight, QColor(preset->highlight));
        palette.setColor(QPalette::HighlightedText, QColor(preset->highlightedText));
        palette.setColor(QPalette::Link, QColor(preset->link));
        palette.setColor(QPalette::LinkVisited, QColor(preset->link).darker(120));
        palette.setColor(QPalette::PlaceholderText, QColor(preset->placeholder));
        // Keep disabled controls readable against the preset's background.
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(QPalette::Disabled, role, QColor(preset->placeholder));
    } else if (themeKey == QStringLiteral("light")) {
        palette.setColor(QPalette::Window, QColor(245, 245, 245));
        palette.setColor(QPalette::WindowText, QColor(30, 30, 30));
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::AlternateBase, QColor(238, 238, 238));
        palette.setColor(QPalette::Text, QColor(30, 30, 30));
        palette.setColor(QPalette::Button, QColor(245, 245, 245));
        palette.setColor(QPalette::ButtonText, QColor(30, 30, 30));
        palette.setColor(QPalette::Highlight, QColor(45, 125, 210));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::PlaceholderText, QColor(120, 120, 120));
    } else if (themeKey == QStringLiteral("custom")) {
        const auto window = customColors.value(QStringLiteral("window"),
                                                 palette.color(QPalette::Window));
        const auto base = customColors.value(QStringLiteral("base"),
                                               palette.color(QPalette::Base));
        const auto text = customColors.value(QStringLiteral("text"),
                                               palette.color(QPalette::Text));
        const auto highlight = customColors.value(QStringLiteral("highlight"),
                                                    palette.color(QPalette::Highlight));
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, base);
        palette.setColor(QPalette::AlternateBase, window.darker(108));
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, window);
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::Highlight, highlight);
        palette.setColor(QPalette::HighlightedText,
                         highlight.lightness() < 128 ? Qt::white : Qt::black);
        palette.setColor(QPalette::PlaceholderText, text);
    }
    // Frame, bevel and tab-bar lines use these roles. Taken from the system
    // palette they stay light, which drew a white line in dark themes; derive
    // them from the theme's own button colour, as QPalette(button) does.
    if (palette.color(QPalette::Button) != m_systemPalette.color(QPalette::Button)) {
        const QColor button = palette.color(QPalette::Button);
        const bool dark = button.lightness() < 128;
        const QColor light = dark ? button.lighter(160) : button.lighter(150);
        const QColor shade = dark ? button.darker(160) : button.darker(200);
        for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            palette.setColor(group, QPalette::Light, light);
            palette.setColor(group, QPalette::Midlight,
                             QColor((button.red() + light.red()) / 2,
                                    (button.green() + light.green()) / 2,
                                    (button.blue() + light.blue()) / 2));
            palette.setColor(group, QPalette::Mid, dark ? button.lighter(130) : button.darker(150));
            palette.setColor(group, QPalette::Dark, shade);
            palette.setColor(group, QPalette::Shadow, dark ? button.darker(250) : button.darker(300));
        }
    }
    return palette;
}

void MainWindow::editCustomColors()
{
    ThemeColorDialog dialog(m_customColors, QApplication::palette(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_customColors = dialog.colors();
    setColorTheme(QStringLiteral("custom"));
}

void MainWindow::createToolbar()
{
    auto *toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);

    auto *newSessionAction = toolbar->addAction(QStringLiteral("New Session"));
    setThemedIcon(newSessionAction, QStringLiteral("document-new"), QStyle::SP_ComputerIcon);
    connect(newSessionAction, &QAction::triggered, this, &MainWindow::createSession);

    auto *newFolderAction = toolbar->addAction(QStringLiteral("New Folder"));
    setThemedIcon(newFolderAction, QStringLiteral("folder-new"), QStyle::SP_FileDialogNewFolder);
    connect(newFolderAction, &QAction::triggered, this, &MainWindow::createFolder);

    toolbar->addSeparator();

    auto *openAction = toolbar->addAction(QStringLiteral("Open Selected"));
    setThemedIcon(openAction, QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay);
    auto *localTerminalAction = toolbar->addAction(QStringLiteral("New Local Terminal"));
    setThemedIcon(localTerminalAction, QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    connect(localTerminalAction, &QAction::triggered, this, &MainWindow::openLocalTerminal);
    disableLocalTerminalWhereUnsupported(localTerminalAction);
    connect(openAction, &QAction::triggered, this, &MainWindow::openSelectedSession);

    toolbar->addSeparator();
    auto *tunnelsAction = toolbar->addAction(QStringLiteral("SSH Tunnels"));
    setThemedIcon(tunnelsAction, QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    connect(tunnelsAction, &QAction::triggered, this, &MainWindow::showTunnels);
    auto *networkToolsAction = toolbar->addAction(QStringLiteral("Network Tools"));
    setThemedIcon(networkToolsAction, QStringLiteral("network-wired"), QStyle::SP_ComputerIcon);
    connect(networkToolsAction, &QAction::triggered, this, &MainWindow::showNetworkTools);

    // Quick connect: [protocol://][user@]host[:port], with a history.
    toolbar->addSeparator();
    auto *quickConnect = new QComboBox(toolbar);
    quickConnect->setObjectName(QStringLiteral("quickConnect"));
    quickConnect->setEditable(true);
    quickConnect->setInsertPolicy(QComboBox::NoInsert);
    quickConnect->setMinimumContentsLength(24);
    quickConnect->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    quickConnect->lineEdit()->setPlaceholderText(QStringLiteral("Quick connect: user@host, rdp://host"));
    quickConnect->lineEdit()->setClearButtonEnabled(true);
    quickConnect->setToolTip(QStringLiteral(
        "Type [ssh|telnet|mosh|rdp|vnc|sftp|scp|ftp|ftps|ftpes://][user@]host[:port], or serial://COM3:9600, and press "
        "Enter. Without a protocol it is SSH."));
    quickConnect->addItems(QuickConnect::history());
    quickConnect->setCurrentIndex(-1);
    toolbar->addWidget(quickConnect);
    connect(quickConnect->lineEdit(), &QLineEdit::returnPressed, this, [this, quickConnect] {
        const QString target = quickConnect->currentText().trimmed();
        QString error;
        if (!openTarget(target, &error)) {
            statusBar()->showMessage(error, 6000);
            return;
        }
        const QSignalBlocker blocker(quickConnect);
        quickConnect->clear();
        quickConnect->addItems(QuickConnect::history());
        quickConnect->setCurrentIndex(-1);
        quickConnect->clearEditText();
    });
}

bool MainWindow::openTarget(const QString &target, QString *error)
{
    const auto session = QuickConnect::parse(target, error);
    if (!session)
        return false;
    QuickConnect::remember(target);
    openSession(*session);
    return true;
}

bool MainWindow::openSessionByPath(const QString &path, QString *error)
{
    const QString wanted = path.trimmed();
    const bool byPath = wanted.contains(QLatin1Char('/'));
    QStringList matches;
    std::function<void(QTreeWidgetItem *, const QString &)> walk =
        [&](QTreeWidgetItem *item, const QString &prefix) {
            for (int index = 0; index < item->childCount(); ++index) {
                auto *child = item->child(index);
                if (isFolder(child)) {
                    walk(child, prefix + child->text(0) + QLatin1Char('/'));
                    continue;
                }
                const QString id = child->data(0, SessionIdRole).toString();
                const QString name = m_sessions.value(id).name;
                if (name.compare(byPath ? wanted.section(QLatin1Char('/'), -1) : wanted,
                                 Qt::CaseInsensitive) == 0
                    && (!byPath || (prefix + name).compare(wanted, Qt::CaseInsensitive) == 0))
                    matches.append(id);
            }
        };
    walk(m_sessionTree->invisibleRootItem(), QString());
    if (matches.size() != 1) {
        if (error)
            *error = matches.isEmpty()
                         ? QStringLiteral("No saved session is called \"%1\".").arg(wanted)
                         : QStringLiteral("Several sessions are called \"%1\"; use Folder/Name.")
                               .arg(wanted);
        return false;
    }
    openSession(m_sessions.value(matches.first()));
    return true;
}

// OpsRemote [--session NAME | --session Folder/NAME]... [TARGET]...
// where TARGET is a quick-connect target such as ssh://user@host:22.
void MainWindow::handleCommandLine(const QStringList &arguments)
{
    QStringList problems;
    for (int index = 0; index < arguments.size(); ++index) {
        const QString &argument = arguments.at(index);
        QString error;
        if (argument == QStringLiteral("--session") || argument == QStringLiteral("-s")) {
            if (++index >= arguments.size()) {
                problems.append(QStringLiteral("--session needs a session name."));
                break;
            }
            if (!openSessionByPath(arguments.at(index), &error))
                problems.append(error);
        } else if (argument.startsWith(QStringLiteral("--session="))) {
            if (!openSessionByPath(argument.mid(10), &error))
                problems.append(error);
        } else if (argument.startsWith(QLatin1Char('-'))) {
            // Qt's own options (-style, -platform) are taken out before this.
            problems.append(QStringLiteral("Unknown option %1.").arg(argument));
        } else if (!openTarget(argument, &error)) {
            problems.append(error);
        }
    }
    if (!problems.isEmpty())
        QMessageBox::warning(this, QStringLiteral("OpsRemote"), problems.join(QLatin1Char('\n')));
}

void MainWindow::addExampleSessions()
{
    auto *examples = new QTreeWidgetItem(m_sessionTree, {QStringLiteral("Examples")});
    setItemKind(examples, ItemKind::Folder);
    auto *linuxFolder = new QTreeWidgetItem(examples, {QStringLiteral("Linux")});
    setItemKind(linuxFolder, ItemKind::Folder);
    auto *servers = new QTreeWidgetItem(linuxFolder, {QStringLiteral("Servers")});
    setItemKind(servers, ItemKind::Folder);

    SessionDefinition sample;
    sample.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sample.name = QStringLiteral("Example SSH Session");
    sample.protocol = QStringLiteral("SSH");
    sample.host = QStringLiteral("server.example.com");
    sample.port = 22;
    addSession(sample, servers);

    examples->setExpanded(true);
    linuxFolder->setExpanded(true);
    servers->setExpanded(true);
}

void MainWindow::loadSessions()
{
    const auto dataDirectory = stateDirectory();
    const auto yamlPath = QDir(dataDirectory).filePath(QStringLiteral("sessions.yaml"));
    const auto legacyJsonPath = QDir(dataDirectory).filePath(QStringLiteral("sessions.json"));
    const bool importingLegacyJson = !QFile::exists(yamlPath) && QFile::exists(legacyJsonPath);
    const auto sourcePath = importingLegacyJson ? legacyJsonPath : yamlPath;
    bool migrateToCompactFormat = importingLegacyJson;

    if (!QFile::exists(sourcePath)) {
        addExampleSessions();
        saveSessions();
        return;
    }

    try {
        const auto document = YAML::LoadFile(sourcePath.toStdString());
        const auto version = document["version"].as<int>(1);
        migrateToCompactFormat = migrateToCompactFormat || version < 2;
        QString error;
        const bool loaded = version == 2
            ? appendCompactSessionYaml(document["folder"], nullptr, &error, document["sessions"])
            : appendSessionYaml(document["items"], nullptr, &error);
        if (!loaded)
            throw YAML::Exception(YAML::Mark::null_mark(), error.toStdString());
    } catch (const YAML::Exception &error) {
        QMessageBox::warning(this, QStringLiteral("Invalid sessions file"),
                             QStringLiteral("Could not read %1: %2")
                                 .arg(sourcePath, QString::fromLocal8Bit(error.what())));
        return;
    }

    if (migrateToCompactFormat)
        saveSessions();
}

bool MainWindow::appendSessionYaml(const YAML::Node &roots, QTreeWidgetItem *parent,
                                  QString *error)
{
    if (!roots || !roots.IsSequence()) {
        if (error)
            *error = QStringLiteral("The YAML document must contain an 'items' sequence.");
        return false;
    }

    QSet<QString> importedIds;
    const int startingDepth = parent ? folderDepth(parent) : 0;
    std::function<bool(const YAML::Node &, int)> validate;
    validate = [&](const YAML::Node &node, int depth) {
        if (!node.IsMap()) {
            if (error) *error = QStringLiteral("Each session or folder must be a YAML mapping.");
            return false;
        }
        if (node["password"] || node["credential"] || node["credentials"]) {
            if (error) *error = QStringLiteral("Passwords cannot be imported from session YAML. Use the credential vault.");
            return false;
        }

        const auto kind = QString::fromStdString(node["kind"].as<std::string>(""));
        if (kind == QStringLiteral("folder")) {
            const auto name = QString::fromStdString(node["name"].as<std::string>("" )).trimmed();
            if (name.isEmpty() || depth + 1 > MaximumFolderDepth) {
                if (error) *error = QStringLiteral("Folder names must be set and nesting cannot exceed three levels.");
                return false;
            }
            const auto children = node["children"];
            if (children && !children.IsSequence()) {
                if (error) *error = QStringLiteral("Folder children must be a YAML sequence.");
                return false;
            }
            if (children) {
                for (const auto &child : children) {
                    if (!validate(child, depth + 1))
                        return false;
                }
            }
            return true;
        }
        if (kind != QStringLiteral("session")) {
            if (error) *error = QStringLiteral("Unknown item kind in session YAML.");
            return false;
        }

        const auto value = node["session"];
        if (!value || !value.IsMap() || value["password"] || value["credential"]
            || value["credentials"]) {
            if (error) *error = QStringLiteral("Session data must be a mapping without inline credentials.");
            return false;
        }
        const auto name = QString::fromStdString(value["name"].as<std::string>(
            node["name"].as<std::string>(""))).trimmed();
        const auto protocol = QString::fromStdString(value["protocol"].as<std::string>(""));
        const auto host = QString::fromStdString(value["host"].as<std::string>(""));
        if (name.isEmpty() || protocol.isEmpty() || host.isEmpty()) {
            if (error) *error = QStringLiteral("Every session needs a name, protocol, and host.");
            return false;
        }
        const auto port = value["port"].as<int>(0);
        if (port < 0 || port > 65535) {
            if (error) *error = QStringLiteral("A session port must be between 0 and 65535.");
            return false;
        }
        const auto id = QString::fromStdString(value["id"].as<std::string>(""));
        if (!id.isEmpty()) {
            if (importedIds.contains(id)) {
                if (error) *error = QStringLiteral("Session IDs must be unique in an import file.");
                return false;
            }
            importedIds.insert(id);
        }
        return true;
    };

    for (const auto &root : roots) {
        if (!validate(root, startingDepth))
            return false;
    }

    std::function<void(const YAML::Node &, QTreeWidgetItem *)> add;
    add = [this, &add](const YAML::Node &node, QTreeWidgetItem *destination) {
        const auto kind = QString::fromStdString(node["kind"].as<std::string>());
        if (kind == QStringLiteral("folder")) {
            const auto name = QString::fromStdString(node["name"].as<std::string>());
            auto *folder = destination ? new QTreeWidgetItem(destination, {name})
                                       : new QTreeWidgetItem(m_sessionTree, {name});
            setItemKind(folder, ItemKind::Folder);
            const auto children = node["children"];
            if (children) {
                for (const auto &child : children)
                    add(child, folder);
            }
            folder->setExpanded(true);
            return;
        }

        const auto value = node["session"];
        SessionDefinition session;
        session.id = QString::fromStdString(value["id"].as<std::string>(""));
        if (session.id.isEmpty() || m_sessions.contains(session.id))
            session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        session.name = QString::fromStdString(value["name"].as<std::string>(
            node["name"].as<std::string>("")));
        session.protocol = QString::fromStdString(value["protocol"].as<std::string>());
        session.host = QString::fromStdString(value["host"].as<std::string>());
        session.port = static_cast<quint16>(value["port"].as<int>(0));
        session.username = QString::fromStdString(value["username"].as<std::string>(""));
        addSession(session, destination);
    };

    for (const auto &root : roots)
        add(root, parent);
    return true;
}

bool MainWindow::appendCompactSessionYaml(const YAML::Node &folders, QTreeWidgetItem *parent,
                                          QString *error, const YAML::Node &rootSessions)
{
    if (!folders || !folders.IsSequence()) {
        if (error)
            *error = QStringLiteral("The YAML document must contain a 'folder' sequence.");
        return false;
    }

    QSet<QString> importedIds;
    const auto validateSessions = [&](const YAML::Node &sessions) {
        if (sessions && !sessions.IsSequence()) {
            if (error) *error = QStringLiteral("Folder sessions must be a YAML sequence.");
            return false;
        }
        if (!sessions)
            return true;
        for (const auto &session : sessions) {
            if (!session.IsMap() || session["password"] || session["credential"]
                || session["credentials"]) {
                if (error) *error = QStringLiteral("Session data must be a mapping without inline credentials.");
                return false;
            }
            const auto sessionName = QString::fromStdString(
                session["name"].as<std::string>("")).trimmed();
            const auto protocol = QString::fromStdString(session["protocol"].as<std::string>(""));
            const auto host = QString::fromStdString(session["host"].as<std::string>(""));
            const auto port = session["port"].as<int>(0);
            if (sessionName.isEmpty() || protocol.isEmpty() || host.isEmpty()
                || port < 0 || port > 65535) {
                if (error) *error = QStringLiteral("Every session needs a name, protocol, host, and valid port.");
                return false;
            }
            const auto id = QString::fromStdString(session["id"].as<std::string>(""));
            if (!id.isEmpty()) {
                if (importedIds.contains(id)) {
                    if (error) *error = QStringLiteral("Session IDs must be unique in an import file.");
                    return false;
                }
                importedIds.insert(id);
            }
        }
        return true;
    };
    const int startingDepth = parent ? folderDepth(parent) : 0;
    std::function<bool(const YAML::Node &, int)> validateFolder;
    validateFolder = [&](const YAML::Node &folder, int depth) {
        if (!folder.IsMap()) {
            if (error) *error = QStringLiteral("Each folder must be a YAML mapping.");
            return false;
        }
        if (folder["password"] || folder["credential"] || folder["credentials"]) {
            if (error) *error = QStringLiteral("Passwords cannot be imported from session YAML.");
            return false;
        }
        const auto name = QString::fromStdString(folder["name"].as<std::string>("")).trimmed();
        if (name.isEmpty() || depth + 1 > MaximumFolderDepth) {
            if (error) *error = QStringLiteral("Folder names must be set and nesting cannot exceed three levels.");
            return false;
        }
        if (!validateSessions(folder["sessions"]))
            return false;
        const auto children = folder["folder"];
        if (children && !children.IsSequence()) {
            if (error) *error = QStringLiteral("Child folders must be a YAML sequence named 'folder'.");
            return false;
        }
        if (children) {
            for (const auto &child : children) {
                if (!validateFolder(child, depth + 1))
                    return false;
            }
        }
        return true;
    };

    for (const auto &folder : folders) {
        if (!validateFolder(folder, startingDepth))
            return false;
    }
    if (!validateSessions(rootSessions))
        return false;

    const auto addSessions = [this](const YAML::Node &sessions, QTreeWidgetItem *destination) {
        if (!sessions)
            return;
        for (const auto &value : sessions) {
            SessionDefinition session;
            session.id = QString::fromStdString(value["id"].as<std::string>(""));
            if (session.id.isEmpty() || m_sessions.contains(session.id))
                session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            session.name = QString::fromStdString(value["name"].as<std::string>());
            session.protocol = QString::fromStdString(value["protocol"].as<std::string>());
            session.host = QString::fromStdString(value["host"].as<std::string>());
            session.port = static_cast<quint16>(value["port"].as<int>(0));
            session.username = QString::fromStdString(value["username"].as<std::string>(""));
            session.terminal = terminalAppearanceFromYaml(value["terminal"]);
            session.rdp = rdpOptionsFromYaml(value["rdp"]);
            session.vnc = vncOptionsFromYaml(value["vnc"]);
            session.ssh = sshOptionsFromYaml(value["ssh"]);
            session.serial = serialOptionsFromYaml(value["serial"]);
            session.ftp = ftpOptionsFromYaml(value["ftp"]);
            session.rsh = rshOptionsFromYaml(value["rsh"]);
            session.xdmcp = xdmcpOptionsFromYaml(value["xdmcp"]);
            session.jumpHost = QString::fromStdString(value["jump_host"].as<std::string>(""));
            session.forwards = portForwardsFromYaml(value["forwards"]);
            session.proxy = QString::fromStdString(value["proxy"].as<std::string>(""));
            session.wakeOnLan = wakeOnLanFromYaml(value["wake_on_lan"]);
            session.pinned = value["pinned"].as<bool>(false);
            session.passwordStore = QString::fromStdString(value["password_store"].as<std::string>(""));
            // Older files kept a VNC session's SSH tunnel with its VNC options.
            if (session.jumpHost.isEmpty())
                session.jumpHost = session.vnc.sshTunnel;
            session.vnc.sshTunnel.clear();
            addSession(session, destination);
        }
    };
    std::function<void(const YAML::Node &, QTreeWidgetItem *)> addFolder;
    addFolder = [this, &addFolder, &addSessions](const YAML::Node &node,
                                                 QTreeWidgetItem *destination) {
        const auto name = QString::fromStdString(node["name"].as<std::string>());
        auto *folder = destination ? new QTreeWidgetItem(destination, {name})
                                   : new QTreeWidgetItem(m_sessionTree, {name});
        setItemKind(folder, ItemKind::Folder);
        folder->setData(0, FolderJumpHostRole,
                        QString::fromStdString(node["jump_host"].as<std::string>("")));
        folder->setData(0, FolderProxyRole,
                        QString::fromStdString(node["proxy"].as<std::string>("")));
        addSessions(node["sessions"], folder);
        const auto children = node["folder"];
        if (children) {
            for (const auto &child : children)
                addFolder(child, folder);
        }
        folder->setExpanded(true);
    };

    for (const auto &folder : folders)
        addFolder(folder, parent);
    // Sessions outside any folder (or, when importing, into the chosen one).
    addSessions(rootSessions, parent);
    return true;
}

void MainWindow::importSessions()
{
    const auto filePath = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import YAML Sessions"), QString(),
        QStringLiteral("YAML files (*.yaml *.yml);;All files (*)"));
    if (filePath.isEmpty())
        return;

    try {
        const auto document = YAML::LoadFile(filePath.toStdString());
        const auto version = document["version"].as<int>(1);
        QString error;
        const bool imported = version == 2
            ? appendCompactSessionYaml(document["folder"], selectedFolder(), &error,
                                       document["sessions"])
            : appendSessionYaml(document["items"], selectedFolder(), &error);
        if (!imported) {
            QMessageBox::warning(this, QStringLiteral("Could not import sessions"), error);
            return;
        }
        saveSessions();
        statusBar()->showMessage(QStringLiteral("Imported session configuration"), 3000);
    } catch (const YAML::Exception &error) {
        QMessageBox::warning(this, QStringLiteral("Invalid YAML file"),
                             QString::fromLocal8Bit(error.what()));
    }
}

// Puts imported sessions in a new top-level folder "Imported from <source>",
// keeping their folders up to the nesting limit (deeper ones are joined).
void MainWindow::importResult(const QString &source, const SessionImport::Result &result)
{
    if (!result.error.isEmpty() && result.sessions.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Import from %1").arg(source), result.error);
        return;
    }
    if (result.sessions.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Import from %1").arg(source),
                                 QStringLiteral("Nothing to import: %1 entries use protocols "
                                                "OpsRemote does not have.").arg(result.skipped));
        return;
    }
    auto *top = addFolder(QStringLiteral("Imported from %1").arg(source), nullptr);
    QHash<QString, QTreeWidgetItem *> folders;
    for (const auto &imported : result.sessions) {
        QStringList path = imported.folders;
        if (path.size() > MaximumFolderDepth - 1) {
            const QString deepest = path.mid(MaximumFolderDepth - 2).join(QStringLiteral(" / "));
            path = path.mid(0, MaximumFolderDepth - 2) << deepest;
        }
        QTreeWidgetItem *parent = top;
        QString key;
        for (const auto &name : std::as_const(path)) {
            key += QLatin1Char('/') + name;
            if (!folders.contains(key))
                folders.insert(key, addFolder(name, parent));
            parent = folders.value(key);
        }
        addSession(imported.session, parent);
    }
    saveSessions();
    QString message = QStringLiteral("Imported %1 session(s) from %2 into \"%3\".")
                          .arg(result.sessions.size())
                          .arg(source, top->text(0));
    if (result.skipped > 0)
        message += QStringLiteral(" %1 entries were left out (patterns, or protocols OpsRemote "
                                  "does not have).").arg(result.skipped);
    message += QStringLiteral(" Passwords are not imported; OpsRemote asks for them and can keep "
                              "them in its vault.");
    QMessageBox::information(this, QStringLiteral("Import from %1").arg(source), message);
}

void MainWindow::exportSessions()
{
    const auto filePath = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export YAML Sessions"), QStringLiteral("sessions.yaml"),
        QStringLiteral("YAML files (*.yaml *.yml)"));
    if (filePath.isEmpty())
        return;

    // The selected folder (or the one holding the selected session), or everything.
    YAML::Node document;
    const auto *selected = selectedItem();
    const auto *folder = selected && isFolder(selected) ? selected
                         : selected                     ? selected->parent()
                                                        : nullptr;
    if (folder) {
        YAML::Node folders(YAML::NodeType::Sequence);
        folders.push_back(folderToYaml(folder, m_sessions));
        document["version"] = 2;
        document["folder"] = folders;
    } else {
        document = sessionsDocument(m_sessionTree->invisibleRootItem());
    }
    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << document;

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Could not export sessions"), file.errorString());
        return;
    }
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Could not export sessions"), file.errorString());
        return;
    }
    QMessageBox::information(this, QStringLiteral("Sessions exported"),
                             QStringLiteral("Session configuration was exported. Passwords are never included in YAML; they remain in the encrypted credential vault."));
}

bool MainWindow::ensureVaultUnlocked()
{
    if (m_vault.isUnlocked())
        return true;

    bool accepted = false;
    if (m_vault.exists()) {
        constexpr int MaximumUnlockAttempts = 3;
        QString error;
        for (int attempt = 1; attempt <= MaximumUnlockAttempts; ++attempt) {
            const auto label = error.isEmpty()
                ? QStringLiteral("Master password:")
                : QStringLiteral("%1\nMaster password (attempt %2 of %3):")
                      .arg(error).arg(attempt).arg(MaximumUnlockAttempts);
            auto masterPassword = QInputDialog::getText(
                this, QStringLiteral("Unlock Credential Vault"), label, QLineEdit::Password,
                QString(), &accepted);
            if (!accepted)
                return false;
            const bool unlocked = m_vault.unlock(masterPassword, &error);
            masterPassword.fill(QChar(0));
            if (unlocked) {
                statusBar()->showMessage(QStringLiteral("Credential vault unlocked"), 3000);
                return true;
            }
        }
        QMessageBox::warning(this, QStringLiteral("Could not unlock vault"), error);
        return false;
    }

    MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Create, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    auto masterPassword = dialog.newPassword();
    QString error;
    const bool created = m_vault.initialize(masterPassword, &error);
    masterPassword.fill(QChar(0));
    if (!created) {
        QMessageBox::warning(this, QStringLiteral("Could not create vault"), error);
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Credential vault created and unlocked"), 3000);
    return true;
}

// The sessions.yaml document for the whole tree: top-level folders under
// "folder", sessions outside any folder under "sessions".
YAML::Node MainWindow::sessionsDocument(const QTreeWidgetItem *root) const
{
    YAML::Node folders(YAML::NodeType::Sequence);
    YAML::Node sessions(YAML::NodeType::Sequence);
    for (int index = 0; index < root->childCount(); ++index) {
        const auto *item = root->child(index);
        if (isFolder(item)) {
            folders.push_back(folderToYaml(item, m_sessions));
            continue;
        }
        const auto session = m_sessions.constFind(item->data(0, SessionIdRole).toString());
        if (session != m_sessions.cend())
            sessions.push_back(sessionToYaml(*session));
    }
    YAML::Node document;
    document["version"] = 2;
    document["folder"] = folders;
    if (sessions.size() > 0)
        document["sessions"] = sessions;
    return document;
}

void MainWindow::saveSessions()
{
    const auto dataDirectory = stateDirectory();
    if (!QDir().mkpath(dataDirectory)) {
        QMessageBox::warning(this, QStringLiteral("Could not save sessions"),
                             QStringLiteral("Could not create the settings folder next to the application."));
        return;
    }

    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << sessionsDocument(m_sessionTree->invisibleRootItem());

    QSaveFile file(QDir(dataDirectory).filePath(QStringLiteral("sessions.yaml")));
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Could not save sessions"), file.errorString());
        return;
    }
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Could not save sessions"), file.errorString());
    }
    refreshSessionList();
}

// The selected item; a copy in Pinned or Recent stands for its session,
// and Pinned and Recent themselves are not something to act on.
QTreeWidgetItem *MainWindow::selectedItem() const
{
    auto *item = m_sessionTree->currentItem();
    if (!item)
        return nullptr;
    const auto kind = item->data(0, ItemKindRole).toString();
    if (kind == QStringLiteral("quick"))
        return nullptr;
    if (kind == QStringLiteral("clone"))
        return realSessionItem(item->data(0, SessionIdRole).toString());
    return item;
}

bool MainWindow::isQuickItem(const QTreeWidgetItem *item) const
{
    const auto kind = item ? item->data(0, ItemKindRole).toString() : QString();
    return kind == QStringLiteral("quick") || kind == QStringLiteral("clone");
}

QTreeWidgetItem *MainWindow::realSessionItem(const QString &id) const
{
    for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
        if ((*it)->data(0, ItemKindRole).toString() == QStringLiteral("session")
            && (*it)->data(0, SessionIdRole).toString() == id)
            return *it;
    }
    return nullptr;
}

QTreeWidgetItem *MainWindow::selectedFolder() const
{
    auto *item = selectedItem();
    if (!item)
        return nullptr;
    if (isFolder(item))
        return item;
    return item->parent();
}

int MainWindow::folderDepth(const QTreeWidgetItem *folder) const
{
    int depth = 0;
    for (auto *item = folder; item; item = item->parent()) {
        if (isFolder(item))
            ++depth;
    }
    return depth;
}

void MainWindow::createFolder()
{
    auto *parent = selectedFolder();
    const int newDepth = parent ? folderDepth(parent) + 1 : 1;
    if (newDepth > MaximumFolderDepth) {
        QMessageBox::information(this, QStringLiteral("Folder depth limit"),
                                 QStringLiteral("Folders can be nested up to three levels."));
        return;
    }

    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("New Folder"),
                                            QStringLiteral("Folder name:"), QLineEdit::Normal,
                                            QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;

    addFolder(name, parent);
    saveSessions();
}

// A folder's settings, in tabs: the jump host and proxy its sessions inherit.
void MainWindow::editFolderSettings(QTreeWidgetItem *folder)
{
    if (!folder || !isFolder(folder))
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Folder Settings: %1").arg(folder->text(0)));
    dialog.setMinimumWidth(480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    auto *jumpHost = new JumpHostEditor(folder->data(0, FolderJumpHostRole).toString(),
                                        jumpHostLabel(inheritedJumpHost(folder->parent())),
                                        jumpHostChoices(), QString(), tabs);
    jumpHost->offerNew([this](QWidget *over) { return createSshSession(over); },
                       [this] { return jumpHostChoices(); });
    tabs->addTab(jumpHost, QStringLiteral("Jump host"));
    auto *proxy = new ProxyEditor(folder->data(0, FolderProxyRole).toString(), QString(),
                                  ProxySettings::label(inheritedProxy(folder->parent())), tabs);
    tabs->addTab(proxy, QStringLiteral("Proxy"));
    layout->addWidget(tabs);
    auto *note = new QLabel(QStringLiteral("Sessions in this folder and its subfolders use these "
                                           "settings unless they choose their own."), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    folder->setData(0, FolderJumpHostRole, jumpHost->value());
    folder->setData(0, FolderProxyRole, proxy->value());
    saveProxyPassword(proxy->value(), proxy->password());
    saveSessions();
    statusBar()->showMessage(QStringLiteral("Saved folder settings: %1").arg(folder->text(0)), 3000);
}

// Del: the selected folder (with its contents) or session.
void MainWindow::deleteSelectedItem()
{
    auto *item = selectedItem();
    if (!item)
        return;
    if (isFolder(item)) {
        deleteSelectedFolder();
        return;
    }
    const auto id = item->data(0, SessionIdRole).toString();
    const auto session = m_sessions.constFind(id);
    if (session == m_sessions.cend())
        return;
    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("Delete Session"));
    confirmation.setText(QStringLiteral("Delete the session \"%1\"?").arg(session->name));
    auto *deleteButton = confirmation.addButton(QStringLiteral("Delete Session"),
                                                QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != deleteButton)
        return;

    const QString name = session->name;
    for (int index = m_sessionTabs->count() - 1; index >= 0; --index) {
        auto *page = m_sessionTabs->widget(index);
        if (page && page->property("sessionId").toString() == id)
            closeSessionTab(index);
    }
    m_sessions.remove(id);
    if (auto *parent = item->parent())
        delete parent->takeChild(parent->indexOfChild(item));
    else
        delete m_sessionTree->takeTopLevelItem(m_sessionTree->indexOfTopLevelItem(item));
    saveSessions();
    statusBar()->showMessage(QStringLiteral("Deleted session: %1").arg(name), 3000);
}

void MainWindow::deleteSelectedFolder()
{
    auto *folder = selectedItem();
    if (!folder || !isFolder(folder)) {
        QMessageBox::information(this, QStringLiteral("Delete Folder"),
                                 QStringLiteral("Select a folder first."));
        return;
    }

    int folderCount = 0;
    int sessionCount = 0;
    QSet<QString> sessionIds;
    std::function<void(QTreeWidgetItem *)> inspect = [this, &folderCount, &sessionCount,
                                                       &sessionIds, &inspect](QTreeWidgetItem *item) {
        if (isFolder(item))
            ++folderCount;
        else {
            ++sessionCount;
            sessionIds.insert(item->data(0, SessionIdRole).toString());
        }
        for (int index = 0; index < item->childCount(); ++index)
            inspect(item->child(index));
    };
    inspect(folder);

    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("Delete Folder"));
    confirmation.setText(QStringLiteral("Delete \"%1\" and its contents?").arg(folder->text(0)));
    confirmation.setInformativeText(
        QStringLiteral("This removes %1 folder(s) and %2 saved session(s) from the session configuration.")
            .arg(folderCount).arg(sessionCount));
    auto *deleteButton = confirmation.addButton(QStringLiteral("Delete Folder"),
                                                QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != deleteButton)
        return;

    const auto folderName = folder->text(0);

    for (int index = m_sessionTabs->count() - 1; index >= 0; --index) {
        auto *page = m_sessionTabs->widget(index);
        if (page && sessionIds.contains(page->property("sessionId").toString()))
            closeSessionTab(index);
    }
    for (const auto &sessionId : sessionIds)
        m_sessions.remove(sessionId);

    auto *parent = folder->parent();
    if (parent)
        delete parent->takeChild(parent->indexOfChild(folder));
    else
        delete m_sessionTree->takeTopLevelItem(m_sessionTree->indexOfTopLevelItem(folder));
    saveSessions();
    saveFolderExpansion();
    statusBar()->showMessage(QStringLiteral("Deleted folder: %1").arg(folderName), 3000);
}

void MainWindow::createSession()
{
    createSessionOver(this, QString());
}

QString MainWindow::createSshSession(QWidget *over)
{
    return createSessionOver(over, QStringLiteral("SSH"));
}

QString MainWindow::createSessionOver(QWidget *over, const QString &protocol)
{
    // In the selected folder, or at the top level when none is selected.
    auto *parentFolder = selectedFolder();

    QDialog dialog(over ? over : this);
    dialog.setWindowTitle(QStringLiteral("New Session"));
    // Wide enough for the terminal preview and the settings tabs.
    dialog.setMinimumWidth(560);
    auto *form = new QFormLayout(&dialog);

    auto *nameEdit = new QLineEdit(&dialog);
    auto *protocolCombo = new QComboBox(&dialog);
    protocolCombo->addItems(sessionProtocols());
    auto *hostEdit = new QLineEdit(&dialog);
    hostEdit->setPlaceholderText(QStringLiteral("hostname or IP address"));
    auto *portEdit = new QLineEdit(&dialog);
    portEdit->setPlaceholderText(QStringLiteral("Use protocol default"));
    auto *usernameEdit = new QLineEdit(&dialog);
    auto *passwordEdit = new QLineEdit(&dialog);
    passwordEdit->setEchoMode(QLineEdit::Password);

    form->addRow(QStringLiteral("Name:"), nameEdit);
    form->addRow(QStringLiteral("Protocol:"), protocolCombo);
    form->addRow(QStringLiteral("Host:"), hostEdit);
    form->addRow(QStringLiteral("Port:"), portEdit);
    form->addRow(QStringLiteral("Username:"), usernameEdit);
    form->addRow(QStringLiteral("Password (optional):"), passwordEdit);
    // From a list of SSH sessions: only that kind fits there.
    if (!protocol.isEmpty()) {
        protocolCombo->setCurrentText(protocol);
        protocolCombo->setEnabled(false);
    }
    auto *storeCombo = passwordStoreChoice({}, &dialog);
    form->addRow(QStringLiteral("Keep the password in:"), storeCombo);
    SshOptionsEditor *sshEditor = nullptr;
    SerialOptionsEditor *serialEditor = nullptr;
    auto *appearanceEditor = addTerminalAppearanceGroup(form, protocolCombo, {}, {}, &sshEditor,
                                                        {}, &serialEditor);
    connectHostField(protocolCombo, hostEdit, portEdit);
    // Mosh needs the mosh program on this computer.
    auto *moshNote = new AvailabilityNote(
        [protocolCombo] {
            return protocolCombo->currentText() == QStringLiteral("Mosh") ? LocalResources::moshProblem()
                                                                          : QString();
        },
        {}, &dialog);
    int protocolRow = -1;
    form->getWidgetPosition(protocolCombo, &protocolRow, nullptr);
    form->insertRow(protocolRow + 1, QString(), moshNote);
    connect(protocolCombo, &QComboBox::currentTextChanged, moshNote, &AvailabilityNote::check);
    auto *rdpEditor = addRdpOptionsGroup(form, protocolCombo, {});
    auto *vncEditor = addVncOptionsGroup(form, protocolCombo, {});
    auto *ftpEditor = addFtpOptionsGroup(form, protocolCombo, {});
    auto *rshEditor = addRshOptionsTab(protocolCombo, appearanceEditor, {});
    auto *xdmcpEditor = addXdmcpOptionsGroup(form, protocolCombo, {});
    auto *jumpHostEditor = addJumpHostTab(protocolCombo, appearanceEditor, rdpEditor, vncEditor,
                                          QString(), parentFolder, QString());
    auto *forwardsEditor = addPortForwardingTab(appearanceEditor, {});
    auto *proxyEditor = addProxyTab(protocolCombo, appearanceEditor, rdpEditor, vncEditor,
                                    QString(), parentFolder);
    auto *wakeEditor = new WakeOnLanEditor({});
    addProtocolTab(protocolCombo, appearanceEditor, rdpEditor, vncEditor, wakeEditor,
                   QStringLiteral("Wake-on-LAN"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    validateSessionDialog(&dialog, form, buttons, protocolCombo, nameEdit, hostEdit, portEdit,
                          wakeEditor);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return QString();
    const auto portText = portEdit->text().trimmed();
    const auto portValue = portText.isEmpty() ? quint16(0) : portText.toUShort();

    SessionDefinition session;
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.name = nameEdit->text().trimmed();
    session.protocol = protocolCombo->currentText();
    session.host = hostEdit->text().trimmed();
    session.port = portValue;
    session.username = usernameEdit->text().trimmed();
    session.jumpHost = jumpHostEditor->value();
    session.proxy = proxyEditor->value();
    session.wakeOnLan = wakeEditor->value();
    saveProxyPassword(session.proxy, proxyEditor->password());
    if (isSshProtocol(session.protocol)) {
        session.forwards = forwardsEditor->value();
        session.ssh = sshEditor->value();
    }
    if (isTerminalProtocol(session.protocol))
        session.terminal = appearanceEditor->value();
    if (session.protocol == QStringLiteral("Serial"))
        session.serial = serialEditor->value();
    if (session.protocol == QStringLiteral("RDP"))
        session.rdp = rdpEditor->value();
    if (session.protocol == QStringLiteral("VNC"))
        session.vnc = vncEditor->value();
    if (session.protocol == QStringLiteral("FTP"))
        session.ftp = ftpEditor->value();
    if (session.protocol == QStringLiteral("Rlogin") || session.protocol == QStringLiteral("RSH"))
        session.rsh = rshEditor->value();
    if (session.protocol == QStringLiteral("XDMCP"))
        session.xdmcp = xdmcpEditor->value();
    session.passwordStore = storeCombo->currentData().toString();
    // Added first, so the password goes to the store this session chose.
    addSession(session, parentFolder);
    saveSessions();
    auto sessionPassword = passwordEdit->text();
    if (!sessionPassword.isEmpty()) {
        QString vaultError;
        if ((m_vault.needsVault(session.id) && !ensureVaultUnlocked())
            || !m_vault.setPassword(session.id, sessionPassword, &vaultError)) {
            QMessageBox::warning(this, QStringLiteral("Could not save password"),
                                 vaultError.isEmpty() ? QStringLiteral("The vault stays locked.") : vaultError);
        }
        sessionPassword.fill(QChar(0));
        passwordEdit->clear();
    }
    return session.id;
}

void MainWindow::openLocalTerminal()
{
    const auto shell = localShell(m_localShell);
    TerminalPage *page = shell.program.isEmpty()
                             ? new TerminalPage(effectiveTerminalAppearance(), m_sessionTabs)
                             : newLocalProgramPage(shell.program, shell.arguments, shell.label,
                                                   effectiveTerminalAppearance());
    connectTerminalPage(page);
    applyTabSettings(page, QStringLiteral("local"));
    addSessionTab(page, shell.label, QStringLiteral("Local shell: %1").arg(shell.label));
}

// A local program in a terminal tab: a PTY on Linux, a pseudo console on
// Windows.
TerminalPage *MainWindow::newLocalProgramPage(const QString &program, const QStringList &arguments,
                                              const QString &label,
                                              const TerminalAppearance &appearance)
{
#ifdef Q_OS_WIN
    return new TerminalPage(new ConPtyConnection(program, arguments, label, QDir::homePath()),
                            appearance, m_sessionTabs);
#else
    Q_UNUSED(label);
    return new TerminalPage(program, arguments, appearance, m_sessionTabs);
#endif
}

// File → New Local Terminal (Administrator), Windows only: through Windows
// sudo when it is on in inline mode (and allowed), otherwise through the
// elevated helper (ElevatedConsoleConnection).
void MainWindow::openLocalTerminalAsAdministrator()
{
#ifdef Q_OS_WIN
    if (m_localShell.windowsShell == QStringLiteral("wsl")) {
        QMessageBox::information(this, QStringLiteral("Administrator Terminal"),
                                 QStringLiteral("WSL does not run as a Windows administrator; use "
                                                "sudo inside Linux instead."));
        return;
    }
    const auto shell = localShell(m_localShell);
    const QString label = QStringLiteral("Administrator: %1").arg(shell.label);
    StreamConnection *connection = nullptr;
    if (m_localShell.useSudo && windowsSudoInlineEnabled())
        connection = new ConPtyConnection(windowsSudoPath(), QStringList{shell.program} + shell.arguments,
                                          label, QDir::homePath());
    else
        connection = new ElevatedConsoleConnection(shell.program, shell.arguments, label,
                                                   QDir::homePath());
    auto *page = new TerminalPage(connection, effectiveTerminalAppearance(), m_sessionTabs);
    connectTerminalPage(page);
    applyTabSettings(page, QStringLiteral("local-administrator"));
    addSessionTab(page, label, QStringLiteral("Local shell as administrator: %1").arg(shell.label));
    if (const int index = m_sessionTabs->indexOf(page); index >= 0)
        m_sessionTabs->setTabIcon(index, style()->standardIcon(QStyle::SP_VistaShield));
#endif
}

void MainWindow::setLocalShellPreferences(const LocalShellPreferences &preferences)
{
    m_localShell = preferences;
    saveLocalShellPreferences(preferences);
}

void MainWindow::openSelectedSession()
{
    auto *item = selectedItem();
    if (!item || isFolder(item)) {
        statusBar()->showMessage(QStringLiteral("Select a saved session first"), 3000);
        return;
    }

    const auto sessionId = item->data(0, SessionIdRole).toString();
    const auto session = m_sessions.constFind(sessionId);
    if (session != m_sessions.cend())
        openSession(*session);
}

void MainWindow::editSelectedSession()
{
    auto *item = selectedItem();
    if (!item || isFolder(item)) {
        QMessageBox::information(this, QStringLiteral("Edit Session"),
                                 QStringLiteral("Select a saved session first."));
        return;
    }

    const auto sessionId = item->data(0, SessionIdRole).toString();
    const auto sessionIterator = m_sessions.find(sessionId);
    if (sessionIterator == m_sessions.end())
        return;

    const SessionDefinition original = *sessionIterator;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit Saved Session"));
    // Wide enough for the terminal preview and the settings tabs.
    dialog.setMinimumWidth(560);
    auto *form = new QFormLayout(&dialog);

    auto *nameEdit = new QLineEdit(original.name, &dialog);
    auto *protocolCombo = new QComboBox(&dialog);
    protocolCombo->addItems(sessionProtocols());
    protocolCombo->setCurrentText(original.protocol);
    auto *hostEdit = new QLineEdit(original.host, &dialog);
    auto *portEdit = new QLineEdit(original.port == 0 ? QString() : QString::number(original.port),
                                   &dialog);
    auto *usernameEdit = new QLineEdit(original.username, &dialog);
    auto *passwordEdit = new QLineEdit(&dialog);
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText(QStringLiteral("Leave blank to keep the stored password"));

    form->addRow(QStringLiteral("Name:"), nameEdit);
    form->addRow(QStringLiteral("Protocol:"), protocolCombo);
    form->addRow(QStringLiteral("Host:"), hostEdit);
    form->addRow(QStringLiteral("Port:"), portEdit);
    form->addRow(QStringLiteral("Username:"), usernameEdit);
    form->addRow(QStringLiteral("New password (optional):"), passwordEdit);
    auto *storeCombo = passwordStoreChoice(original.passwordStore, &dialog);
    form->addRow(QStringLiteral("Keep the password in:"), storeCombo);
    SshOptionsEditor *sshEditor = nullptr;
    SerialOptionsEditor *serialEditor = nullptr;
    auto *appearanceEditor = addTerminalAppearanceGroup(form, protocolCombo, original.terminal,
                                                        original.ssh, &sshEditor, original.serial,
                                                        &serialEditor);
    connectHostField(protocolCombo, hostEdit, portEdit);
    // Mosh needs the mosh program on this computer.
    auto *moshNote = new AvailabilityNote(
        [protocolCombo] {
            return protocolCombo->currentText() == QStringLiteral("Mosh") ? LocalResources::moshProblem()
                                                                          : QString();
        },
        {}, &dialog);
    int protocolRow = -1;
    form->getWidgetPosition(protocolCombo, &protocolRow, nullptr);
    form->insertRow(protocolRow + 1, QString(), moshNote);
    connect(protocolCombo, &QComboBox::currentTextChanged, moshNote, &AvailabilityNote::check);
    auto *rdpEditor = addRdpOptionsGroup(form, protocolCombo, original.rdp);
    auto *vncEditor = addVncOptionsGroup(form, protocolCombo, original.vnc);
    auto *ftpEditor = addFtpOptionsGroup(form, protocolCombo, original.ftp);
    auto *rshEditor = addRshOptionsTab(protocolCombo, appearanceEditor, original.rsh);
    auto *xdmcpEditor = addXdmcpOptionsGroup(form, protocolCombo, original.xdmcp);
    auto *jumpHostEditor = addJumpHostTab(protocolCombo, appearanceEditor, rdpEditor, vncEditor,
                                          original.jumpHost, item->parent(), original.id);
    auto *forwardsEditor = addPortForwardingTab(appearanceEditor, original.forwards);
    auto *proxyEditor = addProxyTab(protocolCombo, appearanceEditor, rdpEditor, vncEditor,
                                    original.proxy, item->parent());
    auto *wakeEditor = new WakeOnLanEditor(original.wakeOnLan);
    addProtocolTab(protocolCombo, appearanceEditor, rdpEditor, vncEditor, wakeEditor,
                   QStringLiteral("Wake-on-LAN"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    form->addRow(buttons);
    validateSessionDialog(&dialog, form, buttons, protocolCombo, nameEdit, hostEdit, portEdit,
                          wakeEditor);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto portText = portEdit->text().trimmed();
    const auto portValue = portText.isEmpty() ? quint16(0) : portText.toUShort();

    auto updated = original;
    updated.name = nameEdit->text().trimmed();
    updated.protocol = protocolCombo->currentText();
    updated.host = hostEdit->text().trimmed();
    updated.port = portValue;
    updated.username = usernameEdit->text().trimmed();
    updated.jumpHost = jumpHostEditor->value();
    updated.proxy = proxyEditor->value();
    updated.wakeOnLan = wakeEditor->value();
    saveProxyPassword(updated.proxy, proxyEditor->password());
    updated.forwards = isSshProtocol(updated.protocol) ? forwardsEditor->value()
                                                      : QList<PortForward>();
    updated.terminal = isTerminalProtocol(updated.protocol) ? appearanceEditor->value()
                                                           : TerminalAppearance();
    updated.serial = updated.protocol == QStringLiteral("Serial") ? serialEditor->value()
                                                                 : SerialOptions();
    updated.ssh = isSshProtocol(updated.protocol) ? sshEditor->value() : SshOptions();
    updated.rdp = updated.protocol == QStringLiteral("RDP") ? rdpEditor->value() : RdpOptions();
    updated.vnc = updated.protocol == QStringLiteral("VNC") ? vncEditor->value() : VncOptions();
    updated.ftp = updated.protocol == QStringLiteral("FTP") ? ftpEditor->value() : FtpOptions();
    updated.rsh = updated.protocol == QStringLiteral("Rlogin") || updated.protocol == QStringLiteral("RSH")
                      ? rshEditor->value()
                      : RshOptions();
    updated.xdmcp = updated.protocol == QStringLiteral("XDMCP") ? xdmcpEditor->value() : XdmcpOptions();

    updated.passwordStore = storeCombo->currentData().toString();
    auto newPassword = passwordEdit->text();
    // A different store and no new password: the saved one moves along.
    const PasswordStore storeBefore = m_vault.storeFor(updated.id);
    QString moving;
    if (newPassword.isEmpty() && updated.passwordStore != original.passwordStore) {
        if (m_vault.needsUnlock(updated.id))
            ensureVaultUnlocked();
        moving = m_vault.passwordForSession(updated.id);
    }

    // Looked up again: "New…" in the jump host list may have added a
    // session while the window was open.
    m_sessions.insert(sessionId, updated);
    if (!moving.isEmpty() && m_vault.storeFor(updated.id) != storeBefore) {
        newPassword = moving;
        moving.fill(QChar(0));
    }
    if (!newPassword.isEmpty()) {
        QString vaultError;
        if ((m_vault.needsVault(updated.id) && !ensureVaultUnlocked())
            || !m_vault.setPassword(updated.id, newPassword, &vaultError)) {
            QMessageBox::warning(this, QStringLiteral("Could not save password"),
                                 vaultError.isEmpty() ? QStringLiteral("The vault stays locked.") : vaultError);
        } else if (m_vault.storeFor(updated.id) == PasswordStore::Vault
                   && storeBefore != PasswordStore::Vault) {
            SystemCredentials::remove(updated.id); // no copy left behind
        }
        newPassword.fill(QChar(0));
        passwordEdit->clear();
    }

    item->setText(0, sessionLabel(updated));
    item->setToolTip(0, QStringLiteral("%1 — %2:%3")
                            .arg(updated.protocol, updated.host,
                                 updated.port == 0 ? QStringLiteral("default")
                                                   : QString::number(updated.port)));
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *page = m_sessionTabs->widget(index);
        if (page && page->property("sessionId").toString() == updated.id) {
            m_sessionTabs->setTabText(index, updated.name);
            m_sessionTabs->setTabToolTip(index, QStringLiteral("%1 — %2")
                                                    .arg(updated.protocol, updated.host));
        }
    }
    applyTerminalAppearanceToTabs();
    saveSessions();
    statusBar()->showMessage(QStringLiteral("Saved session: %1").arg(updated.name), 3000);
}

// Renames a session or folder (F2 in the session list).
void MainWindow::renameTreeItem(QTreeWidgetItem *item, const QString &newName)
{
    const QString name = newName.trimmed();
    if (name.isEmpty())
        return;
    if (isFolder(item)) {
        if (item->text(0) == name)
            return;
        item->setText(0, name);
        saveSessions();
        return;
    }
    const auto id = item->data(0, SessionIdRole).toString();
    auto session = m_sessions.find(id);
    if (session == m_sessions.end())
        return;
    if (session->name == name) {
        item->setText(0, sessionLabel(*session));
        return;
    }
    session->name = name;
    item->setText(0, sessionLabel(*session));
    // Open tabs titled by session name follow.
    if (m_startup.tabTitle != QStringLiteral("userhost")) {
        for (int index = 0; index < m_sessionTabs->count(); ++index) {
            if (m_sessionTabs->widget(index)->property("sessionId").toString() == id)
                m_sessionTabs->setTabText(index, name);
        }
    }
    saveSessions();
    statusBar()->showMessage(QStringLiteral("Renamed session to %1").arg(name), 3000);
}

// A copy of the session right below it, named "Name (1)", "Name (2)", ...
void MainWindow::duplicateSession(QTreeWidgetItem *item)
{
    if (!item || isFolder(item))
        return;
    const auto original = m_sessions.constFind(item->data(0, SessionIdRole).toString());
    if (original == m_sessions.cend())
        return;
    QString base = original->name;
    static const QRegularExpression numbered(QStringLiteral("^(.*) \\((\\d+)\\)$"));
    if (const auto match = numbered.match(base); match.hasMatch())
        base = match.captured(1);
    QSet<QString> names;
    for (const auto &session : std::as_const(m_sessions))
        names.insert(session.name);
    int number = 1;
    while (names.contains(QStringLiteral("%1 (%2)").arg(base).arg(number)))
        ++number;

    SessionDefinition copy = *original;
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy.name = QStringLiteral("%1 (%2)").arg(base).arg(number);
    // The saved password comes along when the vault is open.
    bool passwordCopied = false;
    if (m_vault.isUnlocked()) {
        QString password = m_vault.passwordForSession(original->id);
        if (!password.isEmpty())
            passwordCopied = m_vault.setPassword(copy.id, password);
        password.fill(QChar(0));
    }
    auto *parent = item->parent();
    addSession(copy, parent);
    // addSession() appends; place the copy after the original and the
    // copies already below it, so they stay in order.
    auto *added = m_sessionTree->currentItem();
    const auto childAt = [this, parent](int index) {
        return parent ? parent->child(index) : m_sessionTree->topLevelItem(index);
    };
    const auto indexOf = [this, parent](QTreeWidgetItem *child) {
        return parent ? parent->indexOfChild(child) : m_sessionTree->indexOfTopLevelItem(child);
    };
    if (parent)
        parent->removeChild(added);
    else
        m_sessionTree->takeTopLevelItem(indexOf(added));
    int position = indexOf(item) + 1;
    const QString prefix = base + QStringLiteral(" (");
    while (auto *next = childAt(position)) {
        if (isFolder(next)
            || !m_sessions.value(next->data(0, SessionIdRole).toString()).name.startsWith(prefix))
            break;
        ++position;
    }
    if (parent)
        parent->insertChild(position, added);
    else
        m_sessionTree->insertTopLevelItem(position, added);
    m_sessionTree->setCurrentItem(added);
    saveSessions();
    statusBar()->showMessage(
        m_vault.exists() && !m_vault.isUnlocked()
            ? QStringLiteral("Duplicated as %1 (the vault is locked, so its password was not copied)")
                  .arg(copy.name)
            : QStringLiteral("Duplicated as %1%2")
                  .arg(copy.name, passwordCopied ? QStringLiteral(", with its password") : QString()),
        5000);
}

void MainWindow::showTreeContextMenu(const QPoint &position)
{
    auto *clickedItem = m_sessionTree->itemAt(position);
    // Pinned and Recent have no menu; their copies act on the session.
    if (clickedItem && clickedItem->data(0, ItemKindRole).toString() == QStringLiteral("quick"))
        return;
    if (clickedItem && clickedItem->data(0, ItemKindRole).toString() == QStringLiteral("clone"))
        clickedItem = realSessionItem(clickedItem->data(0, SessionIdRole).toString());
    // An empty area adds at the top level.
    m_sessionTree->setCurrentItem(clickedItem);
    if (!clickedItem)
        m_sessionTree->clearSelection();

    QMenu menu(this);
    const auto addNew = [this, &menu] {
        menu.addAction(QStringLiteral("New Folder…"), this, &MainWindow::createFolder);
        menu.addAction(QStringLiteral("New Session…"), this, &MainWindow::createSession);
    };
    const auto addRename = [this, &menu, clickedItem] {
        auto *rename = menu.addAction(QStringLiteral("Rename"), this, [this, clickedItem] {
            m_sessionTree->editItem(clickedItem, 0);
        });
        rename->setShortcut(QKeySequence(Qt::Key_F2));
    };
    if (!clickedItem) {
        addNew();
    } else if (isFolder(clickedItem)) {
        addNew();
        menu.addSeparator();
        menu.addAction(QStringLiteral("Folder Settings…"), this, [this, clickedItem] {
            editFolderSettings(clickedItem);
        });
        menu.addAction(QStringLiteral("Run Command on Hosts…"), this, [this, clickedItem] {
            showRunCommand(clickedItem);
        });
        addRename();
        auto *remove = menu.addAction(QStringLiteral("Delete Folder…"), this,
                                      &MainWindow::deleteSelectedItem);
        remove->setShortcut(QKeySequence::Delete);
    } else {
        menu.addAction(QStringLiteral("Open"), this, &MainWindow::openSelectedSession);
        menu.addAction(QStringLiteral("Edit…"), this, &MainWindow::editSelectedSession);
        addRename();
        menu.addAction(QStringLiteral("Duplicate"), this, [this, clickedItem] {
            duplicateSession(clickedItem);
        });
        const QString pinId = clickedItem->data(0, SessionIdRole).toString();
        menu.addAction(m_sessions.value(pinId).pinned ? QStringLiteral("Unpin") : QStringLiteral("Pin"),
                       this, [this, pinId] {
                           auto session = m_sessions.find(pinId);
                           if (session == m_sessions.end())
                               return;
                           session->pinned = !session->pinned;
                           saveSessions();
                       });
        const auto session = m_sessions.constFind(clickedItem->data(0, SessionIdRole).toString());
        if (session != m_sessions.cend() && session->wakeOnLan.isSet()) {
            const auto wake = session->wakeOnLan;
            const QString name = session->name;
            menu.addAction(QStringLiteral("Wake Up"), this, [this, wake, name] {
                QString error;
                if (wake.send(&error))
                    statusBar()->showMessage(QStringLiteral("Sent Wake-on-LAN to %1").arg(name), 5000);
                else
                    QMessageBox::warning(this, QStringLiteral("Wake-on-LAN"), error);
            });
        }
        auto *remove = menu.addAction(QStringLiteral("Delete Session…"), this,
                                      &MainWindow::deleteSelectedItem);
        remove->setShortcut(QKeySequence::Delete);
    }
    menu.exec(m_sessionTree->viewport()->mapToGlobal(position));
}

void MainWindow::closeSessionTab(int index)
{
    auto *page = m_sessionTabs->widget(index);
    if (!page)
        return;
    // Remote sessions ask first; local shells close directly.
    auto *terminal = qobject_cast<TerminalPage *>(page);
    auto *screen = qobject_cast<RemoteScreenPage *>(page);
    QString host = terminal ? terminal->sshSettings().host : screen ? screen->host() : QString();
    if (auto *split = qobject_cast<SplitTab *>(page))
        host = QStringLiteral("%1 sessions").arg(split->panes().size());
    if (m_startup.confirmCloseConnected && tabIsConnected(page) && !host.isEmpty()
        && QMessageBox::question(this, QStringLiteral("Close Tab"),
                                 QStringLiteral("Disconnect from %1 and close the tab?").arg(host))
               != QMessageBox::Yes)
        return;
    m_sessionTabs->removeTab(index);
    page->deleteLater();
}

// Whether closing this tab would end a live session: a local shell, or a
// connected SSH or remote desktop session.
bool MainWindow::tabIsConnected(QWidget *page) const
{
    if (auto *split = qobject_cast<SplitTab *>(page)) {
        for (auto *pane : split->panes()) {
            if (pane->isConnected())
                return true;
        }
        return false;
    }
    if (auto *terminal = qobject_cast<TerminalPage *>(page))
        return terminal->isConnected();
    if (auto *screen = qobject_cast<RemoteScreenPage *>(page))
        return screen->isConnected();
    return false;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QStringList connected;
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        if (tabIsConnected(m_sessionTabs->widget(index)))
            connected.append(m_sessionTabs->tabText(index));
    }
    if (m_startup.confirmExit && !connected.isEmpty()) {
        QMessageBox box(QMessageBox::Question, QStringLiteral("Exit OpsRemote"),
                        connected.size() == 1
                            ? QStringLiteral("1 session is still open. Exit anyway?")
                            : QStringLiteral("%1 sessions are still open. Exit anyway?")
                                  .arg(connected.size()),
                        QMessageBox::Yes | QMessageBox::No, this);
        box.setInformativeText(QStringLiteral("• ") + connected.join(QStringLiteral("\n• ")));
        box.setDefaultButton(QMessageBox::No);
        if (box.exec() != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    saveOpenSessions(openSessionIds());
    if (m_startup.rememberWindow)
        saveWindowGeometry(saveGeometry());
    saveFolderExpansion();
    QMainWindow::closeEvent(event);
    // Other top-level windows (Preferences, dialogs) must not keep the
    // application running once the main window is closed.
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

QString MainWindow::folderStateKey(const QTreeWidgetItem *folder) const
{
    QStringList segments;
    for (auto *item = folder; item; item = item->parent()) {
        const auto *parent = item->parent();
        const int index = parent ? parent->indexOfChild(const_cast<QTreeWidgetItem *>(item))
                                 : m_sessionTree->indexOfTopLevelItem(
                                       const_cast<QTreeWidgetItem *>(item));
        segments.prepend(QString::number(index));
    }
    return segments.join(QLatin1Char('/'));
}

void MainWindow::restoreFolderExpansion()
{
    const auto states = loadFolderExpansionStates();
    if (states.isEmpty())
        return;

    std::function<void(QTreeWidgetItem *)> restore = [this, &states, &restore](QTreeWidgetItem *item) {
        if (isFolder(item))
            item->setExpanded(states.value(folderStateKey(item), true));
        for (int index = 0; index < item->childCount(); ++index)
            restore(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        restore(m_sessionTree->topLevelItem(index));
}

void MainWindow::saveFolderExpansion() const
{
    QHash<QString, bool> states;
    std::function<void(const QTreeWidgetItem *)> collect = [this, &states, &collect](
                                                             const QTreeWidgetItem *item) {
        if (isFolder(item))
            states.insert(folderStateKey(item), item->isExpanded());
        for (int index = 0; index < item->childCount(); ++index)
            collect(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        collect(m_sessionTree->topLevelItem(index));
    saveFolderExpansionStates(states);
}

QTreeWidgetItem *MainWindow::addFolder(const QString &name, QTreeWidgetItem *parent)
{
    auto *folder = parent ? new QTreeWidgetItem(parent, {name})
                          : new QTreeWidgetItem(m_sessionTree, {name});
    setItemKind(folder, ItemKind::Folder);
    folder->setExpanded(true);
    m_sessionTree->setCurrentItem(folder);
    return folder;
}

void MainWindow::addSession(const SessionDefinition &session, QTreeWidgetItem *parent)
{
    auto *item = parent ? new QTreeWidgetItem(parent, {sessionLabel(session)})
                        : new QTreeWidgetItem(m_sessionTree, {sessionLabel(session)});
    setItemKind(item, ItemKind::Session);
    item->setData(0, SessionIdRole, session.id);
    item->setToolTip(0, QStringLiteral("%1 — %2:%3")
                            .arg(session.protocol, session.host,
                                 session.port == 0 ? QStringLiteral("default")
                                                   : QString::number(session.port)));
    m_sessions.insert(session.id, session);
    if (parent)
        parent->setExpanded(true);
    m_sessionTree->setCurrentItem(item);
}

void MainWindow::openSession(const SessionDefinition &session)
{
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *page = m_sessionTabs->widget(index);
        bool open = page && page->property("sessionId").toString() == session.id;
        for (auto *pane : terminalPagesIn(page))
            open = open || pane->property("sessionId").toString() == session.id;
        if (open) {
            m_sessionTabs->setCurrentIndex(index);
            return;
        }
    }

    // Wake the computer first, then come back here after the wait.
    const auto &wake = session.wakeOnLan;
    if (wake.isSet() && wake.beforeConnecting && !m_waking.contains(session.id)) {
        QString error;
        if (!wake.send(&error)) {
            statusBar()->showMessage(QStringLiteral("Wake-on-LAN failed: %1").arg(error), 5000);
        } else {
            m_waking.insert(session.id);
            statusBar()->showMessage(QStringLiteral("Waking %1; connecting in %2 s…")
                                         .arg(session.name).arg(wake.effectiveWait()),
                                     wake.effectiveWait() * 1000);
            QTimer::singleShot(wake.effectiveWait() * 1000, this, [this, session] {
                const auto current = m_sessions.value(session.id, session);
                openSession(current);
                m_waking.remove(session.id);
            });
            return;
        }
    }

    QWidget *page = nullptr;
    if (session.protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0) {
        auto settings = sshSettingsFor(session.host, session.port, session.username, session.ssh);
        // A locked vault must be unlocked before a session opens (when its
        // password is kept there); a wrong master password or cancelling the
        // prompt does not open the session.
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            statusBar()->showMessage(
                QStringLiteral("Session not opened: the credential vault is locked"), 5000);
            return;
        }
        if (!resolveJumpHosts(session, &settings.jumps))
            return;
        applyProxy(session, &settings, settings.jumps);
        settings.forwards = session.forwards;
        settings.password = m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0));
        auto *terminal = new TerminalPage(
            settings, effectiveTerminalAppearance().overriddenBy(session.terminal), m_sessionTabs);
        settings.password.fill(QChar(0));
        for (auto &jump : settings.jumps)
            jump.password.fill(QChar(0));
        connectTerminalPage(terminal);
        applyTabSettings(terminal, session.name);
        page = terminal;
    } else if (session.protocol.compare(QStringLiteral("RDP"), Qt::CaseInsensitive) == 0) {
        RdpConnectionSettings settings;
        settings.host = session.host;
        settings.port = session.port == 0 ? 3389 : session.port;
        // "DOMAIN\user" or "user@domain" both work; FreeRDP needs them apart.
        settings.username = session.username;
        if (const int slash = session.username.indexOf(QLatin1Char('\\')); slash > 0) {
            settings.domain = session.username.left(slash);
            settings.username = session.username.mid(slash + 1);
        }
        settings.configPath = stateFilePath(QStringLiteral("rdp"));
        settings.options = loadRdpDefaults().overriddenBy(session.rdp);
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            statusBar()->showMessage(
                QStringLiteral("Session not opened: the credential vault is locked"), 5000);
            return;
        }
        if (!resolveJumpHosts(session, &settings.jumps))
            return;
        applyProxy(session, nullptr, settings.jumps);
        settings.password = m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0));
        auto *rdp = new RdpPage(settings, m_sessionTabs);
        settings.password.fill(QChar(0));
        for (auto &jump : settings.jumps)
            jump.password.fill(QChar(0));
        connect(rdp, &RdpPage::statusMessage, this, [this](const QString &message) {
            statusBar()->showMessage(message, 5000);
        });
        page = rdp;
    } else if (session.protocol.compare(QStringLiteral("VNC"), Qt::CaseInsensitive) == 0) {
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            statusBar()->showMessage(
                QStringLiteral("Session not opened: the credential vault is locked"), 5000);
            return;
        }
        VncPageSettings settings;
        auto &connection = settings.connection;
        connection.host = session.host;
        connection.port = session.port == 0 ? 5900 : session.port;
        // "host:1" is display 1, port 5901 (a bare IPv6 address has more colons).
        if (session.port == 0 && session.host.count(QLatin1Char(':')) == 1) {
            bool ok = false;
            const int display = session.host.section(QLatin1Char(':'), 1).toInt(&ok);
            if (ok && display >= 0 && display < 100) {
                connection.host = session.host.section(QLatin1Char(':'), 0, 0);
                connection.port = quint16(5900 + display);
            }
        }
        connection.username = session.username;
        connection.options = loadVncDefaults().overriddenBy(session.vnc);
        const QString baseId = session.id.section(QLatin1Char('#'), 0, 0);
        connection.password = m_vault.passwordForSession(baseId);
        settings.canSavePassword = m_vault.exists();
        if (!resolveJumpHosts(session, &settings.jumps))
            return;
        applyProxy(session, nullptr, settings.jumps);
        auto *vnc = new VncPage(settings, m_sessionTabs);
        connection.password.fill(QChar(0));
        for (auto &jump : settings.jumps)
            jump.password.fill(QChar(0));
        connect(vnc, &VncPage::statusMessage, this, [this](const QString &message) {
            statusBar()->showMessage(message, 5000);
        });
        connect(vnc, &VncPage::passwordSaveRequested, this, [this, baseId](const QString &password) {
            if (m_vault.needsVault(baseId) && !ensureVaultUnlocked())
                return;
            QString error;
            if (!m_vault.setPassword(baseId, password, &error))
                QMessageBox::warning(this, QStringLiteral("Could not save password"), error);
        });
        page = vnc;
    } else if (session.protocol.compare(QStringLiteral("SFTP"), Qt::CaseInsensitive) == 0
               || session.protocol.compare(QStringLiteral("SCP"), Qt::CaseInsensitive) == 0) {
        // A file session: SSH without a terminal, the file browser in the tab.
        auto settings = sshSettingsFor(session.host, session.port, session.username, session.ssh);
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            statusBar()->showMessage(
                QStringLiteral("Session not opened: the credential vault is locked"), 5000);
            return;
        }
        if (!resolveJumpHosts(session, &settings.jumps))
            return;
        applyProxy(session, &settings, settings.jumps);
        settings.forwards = session.forwards;
        settings.openShell = false;
        settings.filesOnly = true;
        settings.forwardAgent = false;
        settings.forwardX11 = false;
        settings.followTerminalFolder = false;
        settings.fileTransfer = session.protocol.toLower();
        settings.password = m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0));
        auto *files = new FilesPage(settings, m_sessionTabs);
        settings.password.fill(QChar(0));
        for (auto &jump : settings.jumps)
            jump.password.fill(QChar(0));
        connect(files, &FilesPage::statusMessage, this, [this](const QString &message) {
            statusBar()->showMessage(message, 5000);
        });
        page = files;
    } else if (session.protocol.compare(QStringLiteral("FTP"), Qt::CaseInsensitive) == 0) {
        if (m_vault.needsUnlock(session.id) && !ensureVaultUnlocked()) {
            statusBar()->showMessage(
                QStringLiteral("Session not opened: the credential vault is locked"), 5000);
            return;
        }
        const auto options = loadFtpDefaults().overriddenBy(session.ftp);
        FtpConnectionSettings settings;
        settings.host = session.host;
        settings.security = options.security;
        settings.port = session.port != 0 ? session.port
                                          : options.security == QStringLiteral("implicit") ? 990 : 21;
        settings.username = session.username;
        settings.passive = options.mode != QStringLiteral("active");
        settings.listing = options.listing;
        settings.connectTimeout = options.connectTimeout;
        settings.keepaliveSeconds = options.keepaliveSeconds;
        settings.knownCertificatesPath = stateFilePath(QStringLiteral("ftp_certificates"));
        settings.proxy = effectiveProxy(session);
        const QString baseId = session.id.section(QLatin1Char('#'), 0, 0);
        settings.password = m_vault.passwordForSession(baseId);
        auto *ftp = new FilesPage(settings, m_vault.exists(), m_sessionTabs);
        settings.password.fill(QChar(0));
        settings.proxy.password.fill(QChar(0));
        connect(ftp, &FilesPage::statusMessage, this, [this](const QString &message) {
            statusBar()->showMessage(message, 5000);
        });
        connect(ftp, &FilesPage::passwordSaveRequested, this, [this, baseId](const QString &password) {
            if (m_vault.needsVault(baseId) && !ensureVaultUnlocked())
                return;
            QString error;
            if (!m_vault.setPassword(baseId, password, &error))
                QMessageBox::warning(this, QStringLiteral("Could not save password"), error);
        });
        page = ftp;
    } else if (session.protocol.compare(QStringLiteral("XDMCP"), Qt::CaseInsensitive) == 0) {
        page = new XdmcpPage(session.name, session.host, session.port,
                             loadXdmcpDefaults().overriddenBy(session.xdmcp), m_sessionTabs);
    } else if (session.protocol.compare(QStringLiteral("Telnet"), Qt::CaseInsensitive) == 0
               || session.protocol.compare(QStringLiteral("Serial"), Qt::CaseInsensitive) == 0
               || session.protocol.compare(QStringLiteral("Rlogin"), Qt::CaseInsensitive) == 0
               || session.protocol.compare(QStringLiteral("RSH"), Qt::CaseInsensitive) == 0) {
        StreamConnection *connection = nullptr;
        if (session.protocol.compare(QStringLiteral("Telnet"), Qt::CaseInsensitive) == 0)
            connection = new TelnetConnection(session.host, session.port,
                                              loadSshPreferences().terminalType);
        else if (session.protocol.compare(QStringLiteral("Rlogin"), Qt::CaseInsensitive) == 0)
            connection = new RloginConnection(session.host, session.port, session.username,
                                              loadRshDefaults().overriddenBy(session.rsh),
                                              loadSshPreferences().terminalType);
        else if (session.protocol.compare(QStringLiteral("RSH"), Qt::CaseInsensitive) == 0)
            connection = new RshConnection(session.host, session.port, session.username,
                                           loadRshDefaults().overriddenBy(session.rsh));
        else
            connection = new SerialConnection(session.host, session.serial);
        auto *terminal = new TerminalPage(
            connection, effectiveTerminalAppearance().overriddenBy(session.terminal), m_sessionTabs);
        connectTerminalPage(terminal);
        applyTabSettings(terminal, session.name);
        page = terminal;
    } else if (session.protocol.compare(QStringLiteral("Mosh"), Qt::CaseInsensitive) == 0) {
        // The system's mosh client, which logs in with the system's ssh (on
        // Windows, one on the PATH, e.g. from Cygwin or MSYS2).
        const QString mosh = QStandardPaths::findExecutable(QStringLiteral("mosh"));
        if (mosh.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Mosh"),
                QStringLiteral("Mosh is not installed. Install the \"mosh\" package on this "
                               "computer, and on the server (it provides mosh-server)."));
            return;
        }
        QStringList arguments;
        if (session.port != 0)
            arguments << QStringLiteral("--ssh=ssh -p %1").arg(session.port);
        arguments << (session.username.isEmpty()
                          ? session.host
                          : QStringLiteral("%1@%2").arg(session.username, session.host));
        auto *terminal = newLocalProgramPage(mosh, arguments, QStringLiteral("Mosh"),
                                             effectiveTerminalAppearance().overriddenBy(session.terminal));
        connectTerminalPage(terminal);
        applyTabSettings(terminal, session.name);
        page = terminal;
    } else {
        page = new SessionPage(session, m_sessionTabs);
    }
    page->setProperty("sessionId", session.id);
    QString title = session.name;
    if (m_startup.tabTitle == QStringLiteral("userhost") && !session.host.isEmpty())
        title = session.username.isEmpty() ? session.host
                                           : QStringLiteral("%1@%2").arg(session.username, session.host);
    addSessionTab(page, title, QStringLiteral("%1 — %2").arg(session.protocol, session.host));
    statusBar()->showMessage(QStringLiteral("Opened session tab: %1").arg(session.name), 3000);
    recordRecentSession(session.id);
}

// The SSH settings of the session window: appearance and connection tabs.
// The protocols a page of the session window's shared tabs belongs to
// (pages without it are SSH's).
const char *const ProtocolsProperty = "sessionTabProtocols";

TerminalAppearanceEditor *MainWindow::addTerminalAppearanceGroup(
    QFormLayout *form, QComboBox *protocolCombo, const TerminalAppearance &value,
    const SshOptions &ssh, SshOptionsEditor **sshEditor, const SerialOptions &serial,
    SerialOptionsEditor **serialEditor)
{
    // One row of tabs for the terminal and SSH protocols, in the order every
    // protocol's tabs follow: its own settings first (Connection, Serial
    // line), then Appearance, then the network tabs (Jump host, Port
    // forwarding, Proxy), Wake-on-LAN last.
    auto *tabs = new QTabWidget(form->parentWidget());
    tabs->setObjectName(QStringLiteral("sessionTabs"));
    *sshEditor = new SshOptionsEditor(ssh, loadSshPreferences(), tabs);
    tabs->addTab(*sshEditor, QStringLiteral("Connection"));
    *serialEditor = new SerialOptionsEditor(serial, tabs);
    (*serialEditor)->setProperty(ProtocolsProperty, QStringList{QStringLiteral("Serial")});
    tabs->addTab(*serialEditor, QStringLiteral("Serial line"));
    auto *editor = new TerminalAppearanceEditor(value, true, m_terminalAppearance, tabs);
    QStringList terminals;
    for (const auto &protocol : sessionProtocols()) {
        if (isTerminalProtocol(protocol))
            terminals << protocol;
    }
    editor->setProperty(ProtocolsProperty, terminals);
    tabs->addTab(editor, QStringLiteral("Appearance"));
    form->addRow(tabs);
    // Only the selected protocol's tabs are shown; the dialog shrinks to fit.
    // Tabs other windows' editors move here (addProtocolTab) say their
    // protocols the same way when they are not only SSH's.
    auto updateVisible = [protocolCombo, tabs] {
        const QString protocol = protocolCombo->currentText();
        bool any = false;
        for (int index = 0; index < tabs->count(); ++index) {
            const auto protocols = tabs->widget(index)->property(ProtocolsProperty);
            const bool shown = protocols.isValid() ? protocols.toStringList().contains(protocol)
                                                   : isSshProtocol(protocol);
            tabs->setTabVisible(index, shown);
            any = any || shown;
        }
        tabs->setVisible(any);
        // Another protocol starts at its first tab (its own settings).
        const bool changed = tabs->property("shownProtocol").toString() != protocol;
        tabs->setProperty("shownProtocol", protocol);
        if (changed || tabs->currentIndex() < 0 || !tabs->isTabVisible(tabs->currentIndex())) {
            for (int index = 0; index < tabs->count(); ++index) {
                if (tabs->isTabVisible(index)) {
                    tabs->setCurrentIndex(index);
                    break;
                }
            }
        }
        if (auto *dialog = tabs->window())
            QTimer::singleShot(0, dialog, [dialog] { dialog->adjustSize(); });
    };
    // After the shared tabs (Wake-on-LAN, Jump host, Proxy) have moved to the
    // new protocol's tabs, which happens later in the same change.
    connect(protocolCombo, &QComboBox::currentTextChanged, tabs,
            [tabs, updateVisible] { QTimer::singleShot(0, tabs, updateVisible); });
    QTimer::singleShot(0, tabs, updateVisible);
    updateVisible();
    return editor;
}

// The host and port fields mean other things for some protocols: a serial
// session's host is the port (COM3, /dev/ttyUSB0), and it has no TCP port.
void MainWindow::connectHostField(QComboBox *protocolCombo, QLineEdit *hostEdit, QLineEdit *portEdit)
{
    auto *form = qobject_cast<QFormLayout *>(hostEdit->parentWidget()->layout());
    QStringList ports;
    for (const auto &[name, label] : availableSerialPorts())
        ports.append(name);
    auto *completer = new QCompleter(ports, hostEdit);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    const auto update = [protocolCombo, hostEdit, portEdit, form, completer, ports] {
        const bool serial = protocolCombo->currentText() == QStringLiteral("Serial");
        hostEdit->setCompleter(serial ? completer : nullptr);
        hostEdit->setPlaceholderText(
            serial ? (ports.isEmpty() ? QStringLiteral("serial port, e.g. COM3 or /dev/ttyUSB0")
                                      : QStringLiteral("serial port: %1").arg(ports.join(QStringLiteral(", "))))
                   : QStringLiteral("hostname or IP address"));
        if (form) {
            if (auto *label = qobject_cast<QLabel *>(form->labelForField(hostEdit)))
                label->setText(serial ? QStringLiteral("Port:") : QStringLiteral("Host:"));
            form->setRowVisible(portEdit, !serial);
        }
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, hostEdit, update);
    update();
}

// What an empty jump host means in a folder: the nearest folder (this one
// or above) that sets one, or connecting directly.
QString MainWindow::inheritedJumpHost(const QTreeWidgetItem *folder) const
{
    for (; folder; folder = folder->parent()) {
        const auto jumpHost = folder->data(0, FolderJumpHostRole).toString();
        if (!jumpHost.isEmpty())
            return jumpHost;
    }
    return JumpHost::Direct;
}

QString MainWindow::effectiveJumpHost(const SessionDefinition &session) const
{
    if (!session.jumpHost.isEmpty())
        return session.jumpHost;
    QTreeWidgetItem *item = nullptr;
    for (QTreeWidgetItemIterator it(m_sessionTree); *it && !item; ++it) {
        if ((*it)->data(0, SessionIdRole).toString() == session.id && !isQuickItem(*it))
            item = *it;
    }
    const auto inherited = inheritedJumpHost(item ? item->parent() : nullptr);
    // A bastion kept in the folder it serves does not go through itself.
    return inherited == JumpHost::SessionPrefix + session.id ? JumpHost::Direct : inherited;
}

// A label for a jump host value, as shown in the session and folder windows.
QString MainWindow::jumpHostLabel(const QString &jumpHost) const
{
    if (jumpHost.isEmpty() || jumpHost == JumpHost::Direct)
        return QStringLiteral("none");
    if (jumpHost.startsWith(JumpHost::SessionPrefix)) {
        const auto session = m_sessions.constFind(jumpHost.mid(JumpHost::SessionPrefix.size()));
        return session == m_sessions.cend() ? QStringLiteral("a deleted session") : session->name;
    }
    return jumpHost;
}

// The jump hosts to pass through, the first one reached directly. Shows a
// message and returns false when one cannot be used.
bool MainWindow::resolveJumpHosts(const SessionDefinition &session,
                                  std::vector<SshConnectionSettings> *jumps)
{
    jumps->clear();
    QSet<QString> visited{session.id};
    QString jumpHost = effectiveJumpHost(session);
    while (jumpHost != JumpHost::Direct && !jumpHost.isEmpty()) {
        SshConnectionSettings hop;
        QString next;
        if (jumpHost.startsWith(JumpHost::SessionPrefix)) {
            const auto id = jumpHost.mid(JumpHost::SessionPrefix.size());
            const auto gateway = m_sessions.constFind(id);
            if (gateway == m_sessions.cend()
                || gateway->protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
                QMessageBox::warning(this, QStringLiteral("Jump Host"),
                                     QStringLiteral("The jump host of %1 is no longer a saved SSH "
                                                    "session. Choose another in the session's or "
                                                    "folder's settings.")
                                         .arg(session.name));
                return false;
            }
            if (visited.contains(id)) {
                QMessageBox::warning(this, QStringLiteral("Jump Host"),
                                     QStringLiteral("The jump hosts of %1 lead back to %2. Check "
                                                    "the jump hosts of these sessions and their "
                                                    "folders.")
                                         .arg(session.name, gateway->name));
                return false;
            }
            visited.insert(id);
            hop = sshSettingsFor(gateway->host, gateway->port, gateway->username, gateway->ssh);
            hop.password = m_vault.passwordForSession(id);
            next = effectiveJumpHost(*gateway);
        } else {
            QString user, host;
            quint16 port = 0;
            if (!JumpHost::parseAddress(jumpHost, &user, &host, &port)) {
                QMessageBox::warning(this, QStringLiteral("Jump Host"),
                                     QStringLiteral("\"%1\" is not a jump host. Write it as "
                                                    "user@host or user@host:port.")
                                         .arg(jumpHost));
                return false;
            }
            hop = sshSettingsFor(host, port, user.isEmpty() ? session.username : user);
        }
        jumps->insert(jumps->begin(), std::move(hop));
        jumpHost = next;
    }
    return true;
}

// The SSH connection settings for a host, with the SSH preferences (no
// password; the caller adds it from the vault).
SshConnectionSettings MainWindow::sshSettingsFor(const QString &host, quint16 port,
                                                 const QString &username,
                                                 const SshOptions &options) const
{
    SshConnectionSettings settings;
    settings.host = host;
    settings.port = port == 0 ? 22 : port;
    settings.username = username;
    settings.knownHostsPath = stateFilePath(QStringLiteral("known_hosts"));
    const auto ssh = options.appliedTo(loadSshPreferences());
    settings.connectTimeout = ssh.connectTimeout;
    settings.keepaliveSeconds = ssh.keepaliveSeconds;
    settings.compression = ssh.compression;
    settings.tryPublicKeys = ssh.tryPublicKeys;
    settings.forwardAgent = ssh.forwardAgent;
    settings.followTerminalFolder = ssh.followTerminalFolder;
    settings.forwardX11 = ssh.forwardX11;
    settings.fileTransfer = ssh.fileTransfer;
    settings.agent = ssh.agent;
    // A relative key path (like settings/keys/id_ed25519) is in the
    // program's folder, so it moves with it.
    settings.identityFile = ssh.identityFile.isEmpty() || QDir::isAbsolutePath(ssh.identityFile)
                                ? ssh.identityFile
                                : portableFilePath(ssh.identityFile);
    // A passphrase remembered by the key manager; otherwise it is asked for.
    if (!ssh.identityFile.isEmpty())
        settings.keyPassphrase = m_vault.passwordForSession(keyPassphraseVaultKey(ssh.identityFile));
    settings.terminalType = ssh.terminalType;
    settings.autoReconnect = ssh.autoReconnect;
    settings.reconnectDelay = ssh.reconnectDelay;
    settings.reconnectAttempts = ssh.reconnectAttempts;
    return settings;
}

FtpOptionsEditor *MainWindow::addFtpOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                                 const FtpOptions &value)
{
    auto *group = new QWidget(form->parentWidget());
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *editor = new FtpOptionsEditor(value, loadFtpDefaults(), group);
    layout->addWidget(editor);
    form->addRow(group);
    auto updateVisible = [protocolCombo, group] {
        group->setVisible(protocolCombo->currentText() == QStringLiteral("FTP"));
        if (auto *dialog = group->window())
            QTimer::singleShot(0, dialog, [dialog] { dialog->adjustSize(); });
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, group, updateVisible);
    updateVisible();
    return editor;
}

// The Connection tab of Rlogin and RSH sessions, first in the shared tabs.
RshOptionsEditor *MainWindow::addRshOptionsTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                                               const RshOptions &value)
{
    QTabWidget *tabs = nullptr;
    for (auto *widget = sshTabsMember->parentWidget(); widget && !tabs; widget = widget->parentWidget())
        tabs = qobject_cast<QTabWidget *>(widget);
    auto *editor = new RshOptionsEditor(value, loadRshDefaults(), tabs);
    editor->setProperty(ProtocolsProperty, QStringList{QStringLiteral("Rlogin"), QStringLiteral("RSH")});
    tabs->insertTab(0, editor, QStringLiteral("Connection"));
    auto updateCommand = [protocolCombo, editor] {
        editor->setCommandShown(protocolCombo->currentText() == QStringLiteral("RSH"));
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, editor, updateCommand);
    updateCommand();
    return editor;
}

XdmcpOptionsEditor *MainWindow::addXdmcpOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                                     const XdmcpOptions &value)
{
    auto *group = new QWidget(form->parentWidget());
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *editor = new XdmcpOptionsEditor(value, loadXdmcpDefaults(), group);
    layout->addWidget(editor);
    form->addRow(group);
    auto updateVisible = [protocolCombo, group] {
        group->setVisible(protocolCombo->currentText() == QStringLiteral("XDMCP"));
        if (auto *dialog = group->window())
            QTimer::singleShot(0, dialog, [dialog] { dialog->adjustSize(); });
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, group, updateVisible);
    updateVisible();
    return editor;
}

VncOptionsEditor *MainWindow::addVncOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                                 const VncOptions &value)
{
    // The editor's own tabs; no frame around them.
    auto *group = new QWidget(form->parentWidget());
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *editor = new VncOptionsEditor(value, loadVncDefaults(), group);
    layout->addWidget(editor);
    form->addRow(group);
    auto updateVisible = [protocolCombo, group] {
        group->setVisible(protocolCombo->currentText() == QStringLiteral("VNC"));
        if (auto *dialog = group->window())
            QTimer::singleShot(0, dialog, [dialog] { dialog->adjustSize(); });
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, group, updateVisible);
    updateVisible();
    return editor;
}

// The session window's "Jump host" tab, moved to the selected protocol's tabs.
// folder: where the session is (for what "Inherit" means).
JumpHostEditor *MainWindow::addJumpHostTab(QComboBox *protocolCombo,
                                           TerminalAppearanceEditor *sshTabsMember,
                                           QWidget *rdpEditor, QWidget *vncEditor,
                                           const QString &value, const QTreeWidgetItem *folder,
                                           const QString &sessionId)
{
    auto *editor = new JumpHostEditor(value, jumpHostLabel(inheritedJumpHost(folder)),
                                      jumpHostChoices(), sessionId);
    editor->offerNew([this](QWidget *over) { return createSshSession(over); },
                     [this] { return jumpHostChoices(); });
    addProtocolTab(protocolCombo, sshTabsMember, rdpEditor, vncEditor, editor,
                   QStringLiteral("Jump host"));
    return editor;
}

// A tab shown with whichever protocol is selected in the session window,
// moved between the SSH, RDP and VNC settings' tabs.
void MainWindow::addProtocolTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                                QWidget *rdpEditor, QWidget *vncEditor, QWidget *page,
                                const QString &title)
{
    QTabWidget *sshTabs = nullptr;
    for (auto *widget = sshTabsMember->parentWidget(); widget && !sshTabs;
         widget = widget->parentWidget())
        sshTabs = qobject_cast<QTabWidget *>(widget);
    QHash<QString, QTabWidget *> tabs{
        {QStringLiteral("SSH"), sshTabs},
        {QStringLiteral("SFTP"), sshTabs},
        {QStringLiteral("SCP"), sshTabs},
        {QStringLiteral("RDP"), rdpEditor->findChild<QTabWidget *>()},
        {QStringLiteral("VNC"), vncEditor->findChild<QTabWidget *>()}};
    // Telnet, Mosh, Rlogin, RSH and XDMCP go directly: of these tabs only
    // Wake-on-LAN joins theirs (a serial line has no host to wake).
    if (title == QStringLiteral("Wake-on-LAN")) {
        QStringList protocols;
        for (const auto &protocol : sessionProtocols()) {
            if (protocol != QStringLiteral("Serial"))
                protocols << protocol;
        }
        page->setProperty(ProtocolsProperty, protocols);
        for (const auto *name : {"Telnet", "Mosh", "Rlogin", "RSH"})
            tabs.insert(QLatin1String(name), sshTabs);
        if (auto *xdmcpTabs = protocolCombo->window()->findChild<QTabWidget *>(QStringLiteral("xdmcpTabs")))
            tabs.insert(QStringLiteral("XDMCP"), xdmcpTabs);
    }
    // FTP has no jump hosts: its data connections go to other ports.
    if (title != QStringLiteral("Jump host")) {
        if (auto *ftpTabs = protocolCombo->window()->findChild<QTabWidget *>(QStringLiteral("ftpTabs")))
            tabs.insert(QStringLiteral("FTP"), ftpTabs);
    }
    auto place = [protocolCombo, page, tabs, title] {
        auto *target = tabs.value(protocolCombo->currentText());
        for (auto *other : tabs) {
            if (other && other != target && other->indexOf(page) >= 0)
                other->removeTab(other->indexOf(page));
        }
        if (target && target->indexOf(page) < 0)
            target->addTab(page, title);
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, page, place);
    place();
}

// The session window's "Proxy" tab.
ProxyEditor *MainWindow::addProxyTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                                     QWidget *rdpEditor, QWidget *vncEditor, const QString &value,
                                     const QTreeWidgetItem *folder)
{
    auto *editor = new ProxyEditor(value, QString(), ProxySettings::label(inheritedProxy(folder)));
    addProtocolTab(protocolCombo, sshTabsMember, rdpEditor, vncEditor, editor,
                   QStringLiteral("Proxy"));
    return editor;
}

// What an empty proxy means in a folder: the nearest folder (this one or
// above) that sets one, then the Preferences.
QString MainWindow::inheritedProxy(const QTreeWidgetItem *folder) const
{
    for (; folder; folder = folder->parent()) {
        const auto proxy = folder->data(0, FolderProxyRole).toString();
        if (!proxy.isEmpty())
            return proxy;
    }
    return loadSshPreferences().proxy;
}

ProxySettings MainWindow::effectiveProxy(const SessionDefinition &session) const
{
    QString value = session.proxy;
    if (value.isEmpty()) {
        const QTreeWidgetItem *item = nullptr;
        for (QTreeWidgetItemIterator it(m_sessionTree); *it && !item; ++it) {
            if ((*it)->data(0, SessionIdRole).toString() == session.id && !isQuickItem(*it))
                item = *it;
        }
        value = inheritedProxy(item ? item->parent() : nullptr);
    }
    auto proxy = ProxySettings::parse(value);
    if (proxy.isSet() && !proxy.user.isEmpty())
        proxy.password = m_vault.passwordForSession(proxy.vaultKey());
    return proxy;
}

// Puts a session's proxy on the first connection: the first jump host, or
// the server itself.
void MainWindow::applyProxy(const SessionDefinition &session, SshConnectionSettings *server,
                            std::vector<SshConnectionSettings> &jumps) const
{
    const auto proxy = effectiveProxy(session);
    if (!proxy.isSet())
        return;
    if (!jumps.empty())
        jumps.front().proxy = proxy;
    else if (server)
        server->proxy = proxy;
}

// Stores a proxy password typed in a Proxy tab (blank keeps the stored one).
void MainWindow::saveProxyPassword(const QString &value, QString password)
{
    const auto proxy = ProxySettings::parse(value);
    if (password.isEmpty() || !proxy.isSet() || proxy.user.isEmpty())
        return;
    if (!m_vault.needsVault(proxy.vaultKey()) || ensureVaultUnlocked()) {
        QString error;
        if (!m_vault.setPassword(proxy.vaultKey(), password, &error))
            QMessageBox::warning(this, QStringLiteral("Could not save password"), error);
    }
    password.fill(QChar(0));
}

// The SSH settings' "Port forwarding" tab.
PortForwardListEditor *MainWindow::addPortForwardingTab(TerminalAppearanceEditor *sshTabsMember,
                                                        const QList<PortForward> &value)
{
    QTabWidget *sshTabs = nullptr;
    for (auto *widget = sshTabsMember->parentWidget(); widget && !sshTabs;
         widget = widget->parentWidget())
        sshTabs = qobject_cast<QTabWidget *>(widget);
    auto *editor = new PortForwardListEditor(value, sshTabs);
    sshTabs->addTab(editor, QStringLiteral("Port forwarding"));
    return editor;
}

// A saved tunnel's SSH server: a saved SSH session (with its jump hosts and
// password) or user@host[:port].
bool MainWindow::resolveTunnelServer(const QString &server, SshConnectionSettings *settings)
{
    if (server.startsWith(JumpHost::SessionPrefix)) {
        const auto id = server.mid(JumpHost::SessionPrefix.size());
        const auto session = m_sessions.constFind(id);
        if (session == m_sessions.cend()
            || session->protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                                 QStringLiteral("The tunnel's SSH server is no longer a saved SSH "
                                                "session. Choose another in the tunnel's settings."));
            return false;
        }
        *settings = sshSettingsFor(session->host, session->port, session->username, session->ssh);
        if (!resolveJumpHosts(*session, &settings->jumps))
            return false;
        applyProxy(*session, settings, settings->jumps);
        // Without the vault, keys and the log-on prompt still work.
        if (m_vault.needsUnlock(id))
            ensureVaultUnlocked();
        settings->password = m_vault.passwordForSession(id);
        return true;
    }
    QString user, host;
    quint16 port = 0;
    if (!JumpHost::parseAddress(server, &user, &host, &port)) {
        QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                             QStringLiteral("\"%1\" is not an SSH server. Write it as "
                                            "user@host[:port].").arg(server));
        return false;
    }
    *settings = sshSettingsFor(host, port, user);
    settings->proxy = ProxySettings::parse(loadSshPreferences().proxy);
    if (settings->proxy.isSet() && !settings->proxy.user.isEmpty())
        settings->proxy.password = m_vault.passwordForSession(settings->proxy.vaultKey());
    return true;
}

// A tunnel's server with its jump hosts: a session's own, or (for a
// user@host server) the tunnel's, each a session or user@host[:port].
bool MainWindow::resolveTunnel(const SavedTunnel &tunnel, SshConnectionSettings *settings)
{
    if (!resolveTunnelServer(tunnel.server, settings))
        return false;
    if (tunnel.server.startsWith(JumpHost::SessionPrefix) || tunnel.via.isEmpty())
        return true;
    settings->jumps.clear();
    for (const auto &hop : tunnel.via) {
        SshConnectionSettings jump;
        if (hop.trimmed().isEmpty())
            continue;
        if (hop.startsWith(JumpHost::SessionPrefix)) {
            const auto id = hop.mid(JumpHost::SessionPrefix.size());
            const auto gateway = m_sessions.constFind(id);
            if (gateway == m_sessions.cend()) {
                QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                                     QStringLiteral("A jump host of the tunnel is no longer a saved session."));
                return false;
            }
            jump = sshSettingsFor(gateway->host, gateway->port, gateway->username, gateway->ssh);
            jump.password = m_vault.passwordForSession(id);
        } else {
            QString user, host;
            quint16 port = 0;
            if (!JumpHost::parseAddress(hop, &user, &host, &port)) {
                QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                                     QStringLiteral("\"%1\" is not a jump host. Write it as user@host[:port].")
                                         .arg(hop));
                return false;
            }
            jump = sshSettingsFor(host, port, user.isEmpty() ? settings->username : user);
        }
        settings->jumps.push_back(std::move(jump));
    }
    // The proxy (if any) is for the first connection made.
    if (!settings->jumps.empty() && settings->proxy.isSet()) {
        settings->jumps.front().proxy = settings->proxy;
        settings->proxy = ProxySettings();
    }
    return true;
}

TunnelServerInfo MainWindow::describeTunnelServer(const QString &server, const QStringList &via) const
{
    TunnelServerInfo info;
    const auto address = [](const QString &user, const QString &host, quint16 port) {
        return (user.isEmpty() ? host : QStringLiteral("%1@%2").arg(user, host))
               + (port && port != 22 ? QStringLiteral(":%1").arg(port) : QString());
    };
    const auto describeHop = [this, &address](const QString &hop) {
        if (hop.startsWith(JumpHost::SessionPrefix)) {
            const auto session = m_sessions.constFind(hop.mid(JumpHost::SessionPrefix.size()));
            return session == m_sessions.cend() ? QStringLiteral("(deleted session)")
                                                : address(session->username, session->host, session->port);
        }
        return hop;
    };
    if (server.startsWith(JumpHost::SessionPrefix)) {
        const auto id = server.mid(JumpHost::SessionPrefix.size());
        const auto session = m_sessions.constFind(id);
        if (session == m_sessions.cend())
            return info;
        info.valid = true;
        info.user = session->username;
        info.host = session->host;
        info.port = session->port ? session->port : 22;
        info.label = QStringLiteral("%1 (%2)").arg(session->name, address(info.user, info.host, info.port));
        info.fromSession = true;
        // The session's chain of jump hosts, nearest first (names only).
        QString jump = effectiveJumpHost(*session);
        QSet<QString> seen{id};
        while (!jump.isEmpty() && jump != JumpHost::Direct && info.jumps.size() < 10) {
            info.jumps.prepend(describeHop(jump));
            if (!jump.startsWith(JumpHost::SessionPrefix))
                break;
            const auto next = m_sessions.constFind(jump.mid(JumpHost::SessionPrefix.size()));
            if (next == m_sessions.cend() || seen.contains(next->id))
                break;
            seen.insert(next->id);
            jump = effectiveJumpHost(*next);
        }
        return info;
    }
    QString user, host;
    quint16 port = 0;
    if (!JumpHost::parseAddress(server, &user, &host, &port))
        return info;
    info.valid = true;
    info.user = user;
    info.host = host;
    info.port = port ? port : 22;
    info.label = address(user, host, info.port);
    for (const auto &hop : via) {
        if (!hop.trimmed().isEmpty())
            info.jumps << describeHop(hop);
    }
    return info;
}

// The connection of an open, connected tab of a saved session.
SshSession *MainWindow::openTabConnection(const QString &sessionId) const
{
    for (auto *page : allTerminalPages()) {
        if (page->property("sessionId").toString().section(QLatin1Char('#'), 0, 0) == sessionId
            && page->isConnected() && page->sshSession())
            return page->sshSession();
    }
    return nullptr;
}

// Ping, traceroute and port scan, starting with the selected session's host.
void MainWindow::showNetworkTools()
{
    if (!m_networkTools)
        m_networkTools = new NetworkToolsWindow(this);
    if (auto *item = selectedItem(); item && !isFolder(item)) {
        const auto session = m_sessions.constFind(item->data(0, SessionIdRole).toString());
        if (session != m_sessions.cend())
            m_networkTools->setHost(session->host);
    }
    m_networkTools->show();
    m_networkTools->raise();
    m_networkTools->activateWindow();
}

void MainWindow::showRunCommand(QTreeWidgetItem *folder)
{
    QStringList ids;
    if (folder) {
        std::function<void(QTreeWidgetItem *)> collect = [&](QTreeWidgetItem *parent) {
            for (int i = 0; i < parent->childCount(); ++i) {
                auto *child = parent->child(i);
                if (isFolder(child))
                    collect(child);
                else if (const auto id = child->data(0, SessionIdRole).toString(); m_sessions.contains(id))
                    ids << id;
            }
        };
        collect(folder);
    } else {
        ids = m_sessions.keys();
    }
    QList<RunCommandWindow::Host> hosts;
    int skipped = 0;
    for (const auto &id : std::as_const(ids)) {
        const auto &session = m_sessions[id];
        if (session.protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            ++skipped;
            continue;
        }
        QString address = session.username.isEmpty() ? session.host
                                                     : QStringLiteral("%1@%2").arg(session.username, session.host);
        if (session.port != 0 && session.port != 22)
            address += QStringLiteral(":%1").arg(session.port);
        hosts.append({id, session.name, address});
    }
    const QString name = folder ? folder->text(0) : QStringLiteral("All sessions");
    if (hosts.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Run Command on Hosts"),
                                 QStringLiteral("%1 has no SSH sessions.").arg(name));
        return;
    }
    auto *window = new RunCommandWindow(
        name, hosts, skipped, [this](const QStringList &chosen) { return resolveCommandHosts(chosen); },
        [this] { return m_snippets; }, this);
    window->show();
}

QList<RunCommandWindow::Resolved> MainWindow::resolveCommandHosts(const QStringList &ids)
{
    QList<RunCommandWindow::Resolved> results;
    bool askedVault = false;
    for (const auto &id : ids) {
        RunCommandWindow::Resolved result;
        const auto session = m_sessions.constFind(id);
        if (session == m_sessions.cend()
            || session->protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0) {
            result.problem = QStringLiteral("No longer a saved SSH session.");
            results.append(result);
            continue;
        }
        result.settings = sshSettingsFor(session->host, session->port, session->username, session->ssh);
        if (!resolveJumpHosts(*session, &result.settings.jumps)) {
            result.problem = QStringLiteral("Its jump host cannot be used.");
            results.append(result);
            continue;
        }
        applyProxy(*session, &result.settings, result.settings.jumps);
        // Once per run: without the vault, keys and the log-on prompt still work.
        if (!askedVault && m_vault.needsUnlock(id)) {
            askedVault = true;
            ensureVaultUnlocked();
        }
        result.settings.password = m_vault.passwordForSession(id);
        results.append(result);
    }
    return results;
}

void MainWindow::showKeyManager()
{
    KeyManagerDialog dialog(
        [this] { return jumpHostChoices(); },
        [this](const QString &id, SshConnectionSettings *settings) {
            return resolveTunnelServer(JumpHost::SessionPrefix + id, settings);
        },
        [this](const QString &id, const QString &keyPath) {
            auto session = m_sessions.find(id);
            if (session == m_sessions.end())
                return;
            session->ssh.identityFile = keyPath;
            saveSessions();
        },
        [this](const QString &keyPath, const QString &passphrase) {
            if (passphrase.isEmpty() && (!m_vault.exists() || !m_vault.isUnlocked()))
                return true; // nothing could be stored
            if (m_vault.needsVault(keyPassphraseVaultKey(keyPath)) && !ensureVaultUnlocked())
                return false;
            QString error;
            const auto key = keyPassphraseVaultKey(keyPath);
            return passphrase.isEmpty() ? m_vault.removePassword(key, &error)
                                        : m_vault.setPassword(key, passphrase, &error);
        },
        [this](QWidget *over) { return createSshSession(over); }, this);
    dialog.exec();
}

void MainWindow::showTunnels()
{
    m_tunnelsWindow->show();
    m_tunnelsWindow->raise();
    m_tunnelsWindow->activateWindow();
}

// Saved SSH sessions that can be a jump host: (id, label).
QList<QPair<QString, QString>> MainWindow::jumpHostChoices() const
{
    QList<QPair<QString, QString>> choices;
    for (const auto &session : m_sessions) {
        if (session.protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0)
            continue;
        const QString target = session.username.isEmpty()
                                   ? session.host
                                   : QStringLiteral("%1@%2").arg(session.username, session.host);
        choices.append({session.id, QStringLiteral("%1 (%2)").arg(session.name, target)});
    }
    std::sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) {
        return a.second.localeAwareCompare(b.second) < 0;
    });
    return choices;
}

RdpOptionsEditor *MainWindow::addRdpOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                                 const RdpOptions &value)
{
    // The editor's own tabs; no frame around them.
    auto *group = new QWidget(form->parentWidget());
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *editor = new RdpOptionsEditor(value, true, loadRdpDefaults(), group);
    layout->addWidget(editor);
    form->addRow(group);
    auto updateVisible = [protocolCombo, group] {
        group->setVisible(protocolCombo->currentText() == QStringLiteral("RDP"));
        if (auto *dialog = group->window())
            QTimer::singleShot(0, dialog, [dialog] { dialog->adjustSize(); });
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, group, updateVisible);
    updateVisible();
    return editor;
}

// ---- Preferences API (used by the View menu and the Preferences window) ----

// Our own style first; the classic "Windows" (95) style is left out.
QStringList MainWindow::availableUiStyles() const
{
    QStringList styles{OpsRemoteStyle::key()};
    for (const auto &key : QStyleFactory::keys()) {
        if (key.compare(QStringLiteral("Windows"), Qt::CaseInsensitive) != 0)
            styles.append(key);
    }
    for (const auto &theme : KvantumThemes::available())
        styles.append(KvantumThemes::Prefix + theme);
    return styles;
}

// How a style key is shown: "Kvantum: KvArc" for Kvantum themes.
QString MainWindow::uiStyleLabel(const QString &styleKey) const
{
    if (KvantumThemes::isKvantumKey(styleKey))
        return QStringLiteral("Kvantum: %1").arg(styleKey.mid(KvantumThemes::Prefix.size()));
    return styleKey;
}

// ---- Looks ----

QList<Look> MainWindow::looks() const
{
    return builtInLooks() + m_userLooks;
}

Look MainWindow::currentLook() const
{
    Look look;
    look.colorTheme = m_colorThemeKey;
    look.customColors = m_customColors;
    look.style = m_selectedStyleKey;
    look.iconTheme = m_iconTheme;
    look.terminalScheme = m_terminalAppearance.colorScheme;
    look.darkTerminalScheme = m_darkTerminalColorScheme;
    look.terminalFontFamily = m_terminalAppearance.fontFamily;
    look.terminalFontSize = m_terminalAppearance.fontSize;
    const auto font = loadInterfaceFont();
    look.interfaceFontFamily = font.family;
    look.interfaceFontSize = font.pointSize;
    look.windowBorder = windowBordersEnabled();
    return look;
}

// The look the current settings match, or empty for "Custom".
QString MainWindow::matchingLookId() const
{
    const Look current = currentLook();
    for (const auto &look : looks()) {
        if (look.sameAppearance(current))
            return look.id;
    }
    return {};
}

void MainWindow::applyLook(const Look &look)
{
    {
        const QSignalBlocker blocker(this);
        if (look.colorTheme == QStringLiteral("custom"))
            m_customColors = look.customColors;
        setColorTheme(look.colorTheme);
        setUiStyle(look.style.isEmpty() ? QStringLiteral("system") : look.style);
        bool knownIcons = look.iconTheme.isEmpty();
        for (const auto &theme : availableIconThemes())
            knownIcons = knownIcons || theme.first == look.iconTheme;
        setIconThemeChoice(knownIcons ? look.iconTheme : QString());
        setInterfaceFont(look.interfaceFontFamily, look.interfaceFontSize);
        setWindowBorders(look.windowBorder);
        TerminalAppearance terminal;
        terminal.colorScheme = look.terminalScheme;
        terminal.fontFamily = look.terminalFontFamily;
        terminal.fontSize = look.terminalFontSize;
        setDefaultTerminalAppearance(terminal);
        setDarkTerminalColorScheme(look.darkTerminalScheme);
    }
    statusBar()->showMessage(QStringLiteral("Look: %1").arg(look.name), 3000);
    emit appearanceChanged();
    emit looksChanged();
}

// Saves the current settings as a new look; false if the limit is reached.
bool MainWindow::saveCurrentLookAs(const QString &name)
{
    if (m_userLooks.size() >= MaximumUserLooks)
        return false;
    Look look = currentLook();
    look.id = QStringLiteral("user:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    look.name = name;
    m_userLooks.append(look);
    saveUserLooks(m_userLooks);
    emit appearanceChanged();
    emit looksChanged();
    return true;
}

void MainWindow::renameLook(const QString &id, const QString &name)
{
    for (auto &look : m_userLooks) {
        if (look.id == id)
            look.name = name;
    }
    saveUserLooks(m_userLooks);
    emit appearanceChanged();
    emit looksChanged();
}

void MainWindow::deleteLook(const QString &id)
{
    m_userLooks.removeIf([&id](const Look &look) { return look.id == id; });
    saveUserLooks(m_userLooks);
    emit appearanceChanged();
    emit looksChanged();
}

void MainWindow::setUiStyle(const QString &styleKey)
{
    m_selectedStyleKey = styleKey;
    applyUiStyle(styleKey);
    saveUiStylePreference(styleKey);
    syncMenuChecks();    emit appearanceChanged();
}

QList<QPair<QString, QString>> MainWindow::colorThemeChoices() const
{
    QList<QPair<QString, QString>> choices{{QStringLiteral("auto"), QStringLiteral("Automatic (follow OS)")},
                                           {QStringLiteral("system"), QStringLiteral("System (or the style's own)")},
                                           {QStringLiteral("light"), QStringLiteral("Light")},
                                           {QStringLiteral("dark"), QStringLiteral("Dark")}};
    for (const auto &preset : PalettePresets)
        choices.append({QString::fromLatin1(preset.key), QString::fromLatin1(preset.label)});
    choices.append({QStringLiteral("custom"), QStringLiteral("Custom")});
    return choices;
}

void MainWindow::setColorTheme(const QString &themeKey)
{
    m_colorThemeKey = themeKey;
    applyColorTheme(themeKey);
    saveColorThemePreference(themeKey, m_customColors);
    applyTerminalAppearanceToTabs();
    syncMenuChecks();    emit appearanceChanged();
}

bool MainWindow::isDarkInterface() const
{
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

void MainWindow::setIconThemeChoice(const QString &themeName)
{
    m_iconTheme = themeName;
    setIconTheme(themeName);
    saveIconThemePreference(themeName);
    refreshIcons();
    syncMenuChecks();    emit appearanceChanged();
}

QString MainWindow::interfaceFontFamily() const
{
    return loadInterfaceFont().family;
}

int MainWindow::interfaceFontSize() const
{
    return loadInterfaceFont().pointSize;
}

void MainWindow::setInterfaceFont(const QString &family, int pointSize)
{
    const InterfaceFont font{family, pointSize};
    saveInterfaceFont(font);
    applyInterfaceFont(m_systemFont, font);    emit appearanceChanged();
}

bool MainWindow::windowBorders() const
{
    return windowBordersEnabled();
}

void MainWindow::setWindowBorders(bool enabled)
{
    saveWindowBorderPreference(enabled);
    setWindowBordersEnabled(enabled);    emit appearanceChanged();
}

void MainWindow::setDefaultTerminalAppearance(const TerminalAppearance &appearance)
{
    m_terminalAppearance = appearance;
    saveTerminalAppearance(appearance);
    applyTerminalAppearanceToTabs();    emit appearanceChanged();
}

void MainWindow::setDarkTerminalColorScheme(const QString &scheme)
{
    m_darkTerminalColorScheme = scheme;
    const auto path = settingsFilePath();
    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
    }
    settings["terminal"]["dark_color_scheme"] = scheme.toStdString();
    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly)) {
        QByteArray contents(emitter.c_str());
        contents.append('\n');
        if (file.write(contents) == contents.size())
            file.commit();
        else
            file.cancelWriting();
    }
    applyTerminalAppearanceToTabs();
    emit appearanceChanged();
}

// The default appearance, with the dark-mode colour scheme while the
// interface is dark.
TerminalAppearance MainWindow::effectiveTerminalAppearance() const
{
    auto appearance = m_terminalAppearance;
    if (!m_darkTerminalColorScheme.isEmpty() && isDarkInterface())
        appearance.colorScheme = m_darkTerminalColorScheme;
    return appearance;
}

void MainWindow::syncMenuChecks()
{
    if (m_styleGroup) {
        for (auto *action : m_styleGroup->actions())
            action->setChecked(action->data().toString().compare(m_selectedStyleKey, Qt::CaseInsensitive) == 0);
    }
    if (m_colorThemeGroup) {
        for (auto *action : m_colorThemeGroup->actions())
            action->setChecked(action->data().toString() == m_colorThemeKey);
    }
    if (m_iconThemeGroup) {
        for (auto *action : m_iconThemeGroup->actions())
            action->setChecked(action->data().toString() == m_iconTheme);
    }
}

void MainWindow::showPreferences(const QString &page)
{
    if (!m_preferencesWindow)
        m_preferencesWindow = new PreferencesWindow(this);
    m_preferencesWindow->showPage(page);
}

void MainWindow::applyTerminalAppearanceToTabs()
{
    for (auto *page : allTerminalPages()) {
        const auto session = m_sessions.constFind(
            page->property("sessionId").toString().section(QLatin1Char('#'), 0, 0));
        page->setAppearance(session == m_sessions.cend()
                                ? effectiveTerminalAppearance()
                                : effectiveTerminalAppearance().overriddenBy(session->terminal));
    }
}

void MainWindow::setThemedIcon(QAction *action, const QString &name,
                               QStyle::StandardPixmap fallback)
{
    m_themedActions.append({action, name, fallback});
    action->setIcon(themedIcon(name, fallback));
}

// Re-applies every icon after the icon theme changed.
void MainWindow::refreshIcons()
{
    for (const auto &themed : std::as_const(m_themedActions)) {
        if (themed.action)
            themed.action->setIcon(themedIcon(themed.name, themed.fallback));
    }
    std::function<void(QTreeWidgetItem *)> refreshItem = [this, &refreshItem](QTreeWidgetItem *item) {
        if (isQuickItem(item))
            return; // rebuilt below
        setItemKind(item, isFolder(item) ? ItemKind::Folder : ItemKind::Session);
        for (int index = 0; index < item->childCount(); ++index)
            refreshItem(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        refreshItem(m_sessionTree->topLevelItem(index));
    refreshSessionList();
    for (auto *page : allTerminalPages()) {
        if (page->sftpPanel())
            page->sftpPanel()->refreshIcons();
    }
    for (auto *page : m_sessionTabs->findChildren<FilesPage *>())
        page->refreshIcons();
}

void MainWindow::setItemKind(QTreeWidgetItem *item, ItemKind kind)
{
    item->setData(0, ItemKindRole, kind == ItemKind::Folder ? QStringLiteral("folder")
                                                           : QStringLiteral("session"));
    // Both can be renamed with F2.
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    if (kind == ItemKind::Folder) {
        item->setIcon(0, themedIcon(QStringLiteral("folder"), QStyle::SP_DirIcon));
    } else {
        item->setIcon(0, themedIcon(QStringLiteral("network-server"), QStyle::SP_ComputerIcon));
        // Sessions can be dragged, but nothing can be dropped onto them.
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
    }
}

void MainWindow::showSftpPanelForCurrentTab()
{
    auto *page = activeTerminal(m_sessionTabs->currentWidget());
    auto *panel = page ? page->sftpPanel() : nullptr;
    if (!panel) {
        m_sftpPlaceholder->setText(
            m_sessionTabs->count() == 0
                ? QStringLiteral("Open an SSH session to browse its files here.")
                : qobject_cast<FilesPage *>(m_sessionTabs->currentWidget())
                ? QStringLiteral("This file tab shows its files itself.")
                : QStringLiteral("The current tab has no remote files to show."));
        m_sftpStack->setCurrentWidget(m_sftpPlaceholder);
        return;
    }
    if (m_sftpStack->indexOf(panel) < 0)
        m_sftpStack->addWidget(panel);
    m_sftpStack->setCurrentWidget(panel);
}

bool MainWindow::isFolder(const QTreeWidgetItem *item) const
{
    return item && item->data(0, ItemKindRole).toString() == QStringLiteral("folder");
}

// ---- Tabs, startup, and shortcuts ----

void MainWindow::bindShortcut(QAction *action, const QString &id)
{
    m_shortcutActions.insert(id, action);
    action->setShortcut(m_shortcuts.value(id));
}

void MainWindow::setShortcuts(const QHash<QString, QKeySequence> &shortcuts)
{
    m_shortcuts = shortcuts;
    saveShortcuts(shortcuts);
    for (auto it = m_shortcutActions.cbegin(); it != m_shortcutActions.cend(); ++it)
        it.value()->setShortcut(shortcuts.value(it.key()));
    for (auto *page : allTerminalPages())
        page->setShortcuts(shortcuts);
}

void MainWindow::setTerminalBehaviour(const TerminalBehaviour &behaviour)
{
    m_behaviour = behaviour;
    saveTerminalBehaviour(behaviour);
    for (auto *page : allTerminalPages())
        page->setBehaviour(behaviour);
}

void MainWindow::setStartupPreferences(const StartupPreferences &preferences)
{
    m_startup = preferences;
    saveStartupPreferences(preferences);
}

// The host has its own column (Preferences → Appearance → Session list).
QString MainWindow::sessionLabel(const SessionDefinition &session) const
{
    return session.name;
}

void MainWindow::connectTerminalPage(TerminalPage *page)
{
    connect(page, &TerminalPage::statusMessage, this, [this](const QString &message) {
        statusBar()->showMessage(message, 5000);
    });
    // Background tabs with new output or a bell get the accent colour.
    auto mark = [this, page] {
        const int index = tabIndexOf(page);
        if (index >= 0 && index != m_sessionTabs->currentIndex())
            m_sessionTabs->tabBar()->setTabTextColor(index, palette().color(QPalette::Highlight));
    };
    connect(page, &TerminalPage::activity, this, mark);
    connect(page, &TerminalPage::bell, this, mark);
    connect(page, &TerminalPage::userInput, this, [this, page](const QByteArray &data) {
        if (m_broadcasting)
            return;
        // Broadcast: every terminal. Multi-exec: the chosen ones, when this is one.
        const bool multiExec = !m_broadcast && page->isMultiExecTarget();
        if (!m_broadcast && !multiExec)
            return;
        m_broadcasting = true;
        for (auto *other : allTerminalPages()) {
            if (other != page && (m_broadcast || other->isMultiExecTarget()))
                other->sendInput(data);
        }
        m_broadcasting = false;
    });
    connect(page, &TerminalPage::multiExecToggled, this, &MainWindow::updateMultiExecIndicator);
    // The terminal's own menu: split or duplicate this terminal.
    const auto makeActive = [this, page] {
        const int index = tabIndexOf(page);
        if (index < 0)
            return false;
        m_sessionTabs->setCurrentIndex(index);
        if (auto *split = qobject_cast<SplitTab *>(m_sessionTabs->widget(index)))
            split->setActivePane(page);
        return true;
    };
    connect(page, &TerminalPage::splitMenuRequested, this, [this, makeActive](QMenu *menu) {
        addSplitMenus(menu, makeActive);
    });
    connect(page, &TerminalPage::duplicateRequested, this, [this, page, makeActive] {
        if (makeActive())
            duplicateTab(tabIndexOf(page));
    });
    // A closed terminal leaves the multi-exec count.
    connect(page, &QObject::destroyed, this, [this] {
        QTimer::singleShot(0, this, &MainWindow::updateMultiExecIndicator);
    });
}

void MainWindow::addSessionTab(QWidget *page, const QString &title, const QString &toolTip)
{
    // A terminal opened for Split Right / Split Down goes into that tab.
    if (const auto pending = std::exchange(m_pendingSplit, {}); pending.beside) {
        if (auto *terminal = qobject_cast<TerminalPage *>(page)) {
            insertPane(pending.beside, terminal, pending.orientation);
            return;
        }
    }
    const int index = m_startup.newTabNextToCurrent && m_sessionTabs->currentIndex() >= 0
                          ? m_sessionTabs->insertTab(m_sessionTabs->currentIndex() + 1, page, title)
                          : m_sessionTabs->addTab(page, title);
    m_sessionTabs->setTabToolTip(index, toolTip);
    m_sessionTabs->setCurrentIndex(index);
    page->setFocus();
    // "Close Tab" on a tab whose session ended.
    const auto close = [this, page] {
        if (const int at = m_sessionTabs->indexOf(page); at >= 0)
            closeSessionTab(at);
    };
    if (auto *terminal = qobject_cast<TerminalPage *>(page))
        connect(terminal, &TerminalPage::closeRequested, this, [this, terminal] { closePane(terminal); });
    else if (auto *screen = qobject_cast<RemoteScreenPage *>(page))
        connect(screen, &RemoteScreenPage::closeRequested, this, close);
}

void MainWindow::duplicateTab(int index)
{
    QWidget *page = m_sessionTabs->widget(index);
    if (auto *split = qobject_cast<SplitTab *>(page))
        page = split->activePane();
    if (!page)
        return;
    const auto sessionId = page->property("sessionId").toString();
    if (sessionId.isEmpty()) {
        openLocalTerminal();
        return;
    }
    const auto session = m_sessions.constFind(sessionId);
    if (session == m_sessions.cend())
        return;
    // openSession() switches to an open tab of the same session, so open a
    // second copy under a distinct ID.
    auto copy = *session;
    copy.id = sessionId + QStringLiteral("#") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    openSession(copy);
}

// Send Keys and Save Screenshot for a remote desktop (RDP or VNC) tab.
void MainWindow::addRemoteScreenActions(QMenu *menu, RemoteScreenPage *screen)
{
    auto *keys = menu->addMenu(QStringLiteral("Send Keys"));
    keys->setEnabled(screen->isConnected());
    for (const auto &[combination, label] : RemoteScreenPage::keyCombinations()) {
        keys->addAction(label, screen, [screen, combination] {
            screen->sendKeyCombination(combination);
        });
    }
    if (screen->canRefresh()) {
        auto *refresh = menu->addAction(QStringLiteral("Refresh Screen"), screen,
                                        [screen] { screen->refreshScreen(); });
        refresh->setEnabled(screen->isConnected());
    }
    menu->addAction(QStringLiteral("Save Screenshot…"), this, [this, screen] {
        const QImage image = screen->screenshot();
        if (image.isNull()) {
            statusBar()->showMessage(QStringLiteral("Nothing to save yet: not connected."), 5000);
            return;
        }
        const QString name = QStringLiteral("%1 %2.png")
                                 .arg(screen->host(), QDateTime::currentDateTime().toString(
                                                          QStringLiteral("yyyy-MM-dd HH.mm.ss")));
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Screenshot"),
            QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).filePath(name),
            QStringLiteral("PNG image (*.png);;JPEG image (*.jpg)"));
        if (path.isEmpty())
            return;
        if (image.save(path))
            statusBar()->showMessage(QStringLiteral("Screenshot saved to %1")
                                         .arg(QDir::toNativeSeparators(path)),
                                     5000);
        else
            QMessageBox::warning(this, QStringLiteral("Save Screenshot"),
                                 QStringLiteral("Could not save %1.").arg(QDir::toNativeSeparators(path)));
    });
}

void MainWindow::showTabContextMenu(const QPoint &position)
{
    const int index = m_sessionTabs->tabBar()->tabAt(position);
    if (index < 0)
        return;
    QMenu menu(this);
    menu.addAction(QStringLiteral("Duplicate Tab"), this, [this, index] { duplicateTab(index); });
    if (activeTerminal(m_sessionTabs->widget(index))) {
        m_sessionTabs->setCurrentIndex(index);
        if (auto *terminal = activeTerminal(m_sessionTabs->widget(index))) {
            auto *include = menu.addAction(QStringLiteral("Multi-exec: Type Into This Terminal Too"));
            include->setCheckable(true);
            include->setChecked(terminal->isMultiExecTarget());
            connect(include, &QAction::toggled, this, [this, terminal](bool on) {
                terminal->setMultiExecTarget(on);
                updateMultiExecIndicator();
            });
        }
        addSplitMenus(&menu, [] { return true; });
        if (qobject_cast<SplitTab *>(m_sessionTabs->widget(index)))
            menu.addAction(QStringLiteral("Close Pane"), this, [this, index] {
                if (auto *pane = activeTerminal(m_sessionTabs->widget(index)))
                    closePane(pane);
            });
    }
    if (auto *screen = qobject_cast<RemoteScreenPage *>(m_sessionTabs->widget(index))) {
        menu.addSeparator();
        addRemoteScreenActions(&menu, screen);
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("Close Tab"), this, [this, index] { closeSessionTab(index); });
    menu.addAction(QStringLiteral("Close Other Tabs"), this, [this, index] {
        auto *keep = m_sessionTabs->widget(index);
        for (int other = m_sessionTabs->count() - 1; other >= 0; --other) {
            if (m_sessionTabs->widget(other) != keep)
                closeSessionTab(other);
        }
    });
    menu.exec(m_sessionTabs->tabBar()->mapToGlobal(position));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
        m_lastInput.restart();
        break;
    default:
        break;
    }
    if (watched == m_sessionSearch && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Return || key == Qt::Key_Enter) {
            if (auto *first = firstVisibleSession()) {
                m_sessionTree->setCurrentItem(first);
                if (key == Qt::Key_Down)
                    m_sessionTree->setFocus();
                else
                    openSelectedSession();
            }
            return true;
        }
        if (key == Qt::Key_Escape) {
            if (m_sessionSearch->text().isEmpty())
                m_sessionTree->setFocus();
            else
                m_sessionSearch->clear();
            return true;
        }
    }
    // Every dialog is wide enough for its title next to the window's own
    // buttons (the vault's prompt showed "Jnlock Credenti…").
    if (event->type() == QEvent::Show) {
        if (auto *dialog = qobject_cast<QDialog *>(watched);
            dialog && dialog->isWindow() && !dialog->property("opsremoteTitleFitted").toBool()) {
            dialog->setProperty("opsremoteTitleFitted", true);
            const QFontMetrics metrics(QApplication::font("QWorkspaceTitleBar"));
            // Icon, minimize/maximize/close, and margins.
            const int wanted = metrics.horizontalAdvance(dialog->windowTitle()) + 190;
            if (dialog->minimumWidth() < wanted)
                dialog->setMinimumWidth(qMin(wanted, 900));
            if (dialog->width() < wanted)
                dialog->resize(qMin(wanted, 900), dialog->height());
        }
    }
    // A search box hidden in the Preferences shows only while it is used.
    if (watched == m_sessionSearch && event->type() == QEvent::FocusOut
        && !m_sessionList.showSearch && m_sessionSearch->text().isEmpty())
        m_sessionSearch->hide();
    // Widgets watched from early in the constructor send events before the
    // tabs exist.
    if (m_sessionTabs && watched == m_sessionTabs->tabBar()
        && event->type() == QEvent::MouseButtonRelease
        && m_startup.middleClickCloses) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::MiddleButton) {
            const int index = m_sessionTabs->tabBar()->tabAt(mouse->position().toPoint());
            if (index >= 0) {
                closeSessionTab(index);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

QStringList MainWindow::openSessionIds() const
{
    QStringList ids;
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        // Duplicated tabs are stored under their session's ID; the panes of a
        // split tab come back as tabs of their own.
        auto *tab = m_sessionTabs->widget(index);
        QList<QWidget *> pages{tab};
        if (auto *split = qobject_cast<SplitTab *>(tab)) {
            pages.clear();
            for (auto *pane : split->panes())
                pages.append(pane);
        }
        for (auto *page : std::as_const(pages)) {
            const auto id = page->property("sessionId").toString().section(QLatin1Char('#'), 0, 0);
            ids.append(id.isEmpty() ? QStringLiteral("local") : id);
        }
    }
    return ids;
}

void MainWindow::restoreStartupTabs()
{
    if (m_security.unlockAtStartup && m_vault.exists() && !m_vault.isUnlocked())
        ensureVaultUnlocked();
    if (m_startup.onStartup == QStringLiteral("local")) {
        if (LocalTerminalSupported)
            openLocalTerminal();
    } else if (m_startup.onStartup == QStringLiteral("restore")) {
        for (const auto &id : loadOpenSessions()) {
            if (id == QStringLiteral("local")) {
                if (LocalTerminalSupported)
                    openLocalTerminal();
                continue;
            }
            const auto session = m_sessions.constFind(id);
            if (session != m_sessions.cend())
                openSession(*session);
        }
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange && isMinimized() && m_security.lockOnMinimize
        && m_vault.isUnlocked())
        lockVaultFor(QStringLiteral("when the window was minimized"));
    QMainWindow::changeEvent(event);
}

void MainWindow::lockVaultFor(const QString &reason)
{
    m_vault.lock();
    statusBar()->showMessage(reason.isEmpty()
                                 ? QStringLiteral("Credential vault locked")
                                 : QStringLiteral("Credential vault locked %1").arg(reason),
                             5000);
}

void MainWindow::applyTabSettings(TerminalPage *page, const QString &logName)
{
    page->setLogging(m_logging, logName);
    page->setHighlightRules(m_highlightRules);
    if (m_presentationState.active)
        page->setZoomSteps(m_presentation.zoomSteps);
}

void MainWindow::setSecurityPreferences(const SecurityPreferences &preferences)
{
    m_security = preferences;
    saveSecurityPreferences(preferences);
}

void MainWindow::setLoggingPreferences(const LoggingPreferences &preferences)
{
    m_logging = preferences;
    saveLoggingPreferences(preferences);
    // Open tabs start or stop logging now.
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        for (auto *page : terminalPagesIn(m_sessionTabs->widget(index)))
            page->setLogging(preferences, m_sessionTabs->tabText(index));
    }
}

void MainWindow::setPresentationPreferences(const PresentationPreferences &preferences)
{
    m_presentation = preferences;
    savePresentationPreferences(preferences);
}

void MainWindow::setHighlightRules(const QList<HighlightRule> &rules)
{
    m_highlightRules = rules;
    saveHighlightRules(rules);
    for (auto *page : allTerminalPages())
        page->setHighlightRules(rules);
}

void MainWindow::setSnippets(const QList<Snippet> &snippets)
{
    m_snippets = snippets;
    saveSnippets(snippets);
}

void MainWindow::setBroadcast(bool on)
{
    m_broadcast = on;
    m_broadcastIndicator->setVisible(on);
    if (m_broadcastAction->isChecked() != on)
        m_broadcastAction->setChecked(on);
    statusBar()->showMessage(on ? QStringLiteral("Typing now goes to every connected SSH tab")
                                : QStringLiteral("Broadcast off"),
                             3000);
}

// Larger text and nothing but terminals, for screen sharing and projectors.
void MainWindow::setPresentationMode(bool on)
{
    if (on == m_presentationState.active)
        return;
    auto *toolbar = findChild<QToolBar *>();
    if (on) {
        m_presentationState = {true, windowState(), m_leftTabs->isVisible(),
                               toolbar && toolbar->isVisible(), statusBar()->isVisible()};
        if (m_presentation.hidePanels) {
            m_leftTabs->hide();
            if (toolbar)
                toolbar->hide();
            statusBar()->hide();
        }
        if (m_presentation.fullScreen)
            setWindowState(windowState() | Qt::WindowFullScreen);
    } else {
        m_presentationState.active = false;
        m_leftTabs->setVisible(m_presentationState.sidePanel);
        if (toolbar)
            toolbar->setVisible(m_presentationState.toolbar);
        statusBar()->setVisible(m_presentationState.statusBar);
        setWindowState(m_presentationState.windowState);
    }
    for (auto *page : allTerminalPages())
        page->setZoomSteps(on ? m_presentation.zoomSteps : 0);
    if (m_presentationAction->isChecked() != on)
        m_presentationAction->setChecked(on);
}

// ---- The session list: Pinned, Recent, columns, and host status ----

namespace {

QStringList loadRecentSessions()
{
    QStringList ids;
    try {
        const auto list = YAML::LoadFile(stateFilePath(QStringLiteral("recent-sessions.yaml")).toStdString())["recent"];
        for (const auto &entry : list)
            ids.append(QString::fromStdString(entry.as<std::string>("")));
    } catch (const YAML::Exception &) {
        // None yet.
    }
    ids.removeAll(QString());
    return ids;
}

void saveRecentSessions(const QStringList &ids)
{
    YAML::Node root;
    root["recent"] = YAML::Node(YAML::NodeType::Sequence);
    for (const auto &id : ids)
        root["recent"].push_back(id.toStdString());
    YAML::Emitter out;
    out << root;
    QSaveFile file(stateFilePath(QStringLiteral("recent-sessions.yaml")));
    if (QDir().mkpath(stateDirectory()) && file.open(QIODevice::WriteOnly)) {
        file.write(out.c_str());
        file.commit();
    }
}

}

void MainWindow::recordRecentSession(const QString &id)
{
    if (!m_sessions.contains(id))
        return; // quick-connect targets are not saved sessions
    m_recentSessions.removeAll(id);
    m_recentSessions.prepend(id);
    m_recentSessions = m_recentSessions.mid(0, 20);
    saveRecentSessions(m_recentSessions);
    refreshSessionList();
}

void MainWindow::setSessionListPreferences(const SessionListPreferences &preferences)
{
    m_sessionList = preferences;
    saveSessionListPreferences(preferences);
    applySessionListPreferences();
}

void MainWindow::applySessionListPreferences()
{
    m_sessionTree->setColumnHidden(HostColumn, !m_sessionList.showHost);
    m_sessionTree->setColumnHidden(ProtocolColumn, !m_sessionList.showProtocol);
    m_sessionTree->setColumnHidden(SummaryColumn, summaryColumn() != SummaryColumn);
    m_sessionSearch->setVisible(m_sessionList.showSearch || !m_sessionSearch->text().isEmpty());
    // The header names the columns only when there are some.
    m_sessionTree->setHeaderHidden(false);
    if (m_sessionList.checkStatus) {
        m_statusTimer->start(m_sessionList.checkSeconds * 1000);
        QTimer::singleShot(0, this, &MainWindow::checkHostStatus);
    } else {
        m_statusTimer->stop();
        m_hostStatus->clear();
    }
    refreshSessionList();
}

// Where folder summaries go: -1 nowhere, 0 after the name, otherwise the
// first column after the name (its own when no other column is shown).
int MainWindow::summaryColumn() const
{
    if (m_sessionList.folderSummary == QStringLiteral("off"))
        return -1;
    if (m_sessionList.folderSummary != QStringLiteral("column"))
        return 0;
    if (m_sessionList.showHost)
        return HostColumn;
    if (m_sessionList.showProtocol)
        return ProtocolColumn;
    return SummaryColumn;
}

// How many sessions shown under a folder answer and how many do not.
QPair<int, int> MainWindow::folderStatusCounts(const QTreeWidgetItem *folder) const
{
    QPair<int, int> counts{0, 0};
    for (int index = 0; index < folder->childCount(); ++index) {
        const auto *child = folder->child(index);
        if (child->isHidden())
            continue; // left out by a search
        if (isFolder(child)) {
            const auto inner = folderStatusCounts(child);
            counts.first += inner.first;
            counts.second += inner.second;
            continue;
        }
        const auto status = m_hostStatus->status(child->data(0, SessionIdRole).toString());
        if (status == HostStatusChecker::Status::Up)
            ++counts.first;
        else if (status == HostStatusChecker::Status::Down)
            ++counts.second;
    }
    return counts;
}

// Rebuilds Pinned and Recent at the top, fills the columns, and fits the panel.
void MainWindow::refreshSessionList()
{
    if (!m_sessionTree)
        return;
    // Pinned and Recent look like other folders, with a star or a clock.
    const auto rebuild = [this](QTreeWidgetItem *&folder, bool show, const QString &name,
                                bool star, const QStringList &ids, int position) {
        const bool expanded = !folder || folder->isExpanded();
        delete folder;
        folder = nullptr;
        if (!show || ids.isEmpty())
            return;
        folder = new QTreeWidgetItem({name});
        m_sessionTree->insertTopLevelItem(position, folder);
        folder->setData(0, ItemKindRole, QStringLiteral("quick"));
        folder->setIcon(0, badgedFolderIcon(themedIcon(QStringLiteral("folder"), QStyle::SP_DirIcon),
                                            star));
        folder->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        for (const auto &id : ids) {
            const auto session = m_sessions.constFind(id);
            if (session == m_sessions.cend())
                continue;
            auto *copy = new QTreeWidgetItem(folder, {sessionLabel(*session)});
            copy->setData(0, ItemKindRole, QStringLiteral("clone"));
            copy->setData(0, SessionIdRole, id);
            copy->setIcon(0, themedIcon(QStringLiteral("network-server"), QStyle::SP_ComputerIcon));
            copy->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        folder->setExpanded(expanded);
    };
    QStringList pinned;
    for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
        if ((*it)->data(0, ItemKindRole).toString() == QStringLiteral("session")) {
            const auto id = (*it)->data(0, SessionIdRole).toString();
            if (m_sessions.value(id).pinned)
                pinned.append(id);
        }
    }
    QStringList recent;
    for (const auto &id : std::as_const(m_recentSessions)) {
        if (m_sessions.contains(id) && recent.size() < m_sessionList.recentCount)
            recent.append(id);
    }
    rebuild(m_pinnedFolder, m_sessionList.showPinned, QStringLiteral("Pinned"), true, pinned, 0);
    rebuild(m_recentFolder, m_sessionList.showRecent, QStringLiteral("Recent"), false, recent,
            m_pinnedFolder ? 1 : 0);

    // Columns: host (with a port that is not the protocol's usual one) and protocol.
    for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
        auto *item = *it;
        if (isFolder(item) || item->data(0, ItemKindRole).toString() == QStringLiteral("quick"))
            continue;
        const auto session = m_sessions.constFind(item->data(0, SessionIdRole).toString());
        if (session == m_sessions.cend())
            continue;
        static const QHash<QString, int> usualPorts{{QStringLiteral("SSH"), 22},
                                                    {QStringLiteral("MOSH"), 22},
                                                    {QStringLiteral("TELNET"), 23},
                                                    {QStringLiteral("RDP"), 3389},
                                                    {QStringLiteral("VNC"), 5900},
                                                    {QStringLiteral("FTP"), 21},
                                                    {QStringLiteral("SFTP"), 22},
                                                    {QStringLiteral("SCP"), 22},
                                                    {QStringLiteral("RLOGIN"), 513},
                                                    {QStringLiteral("RSH"), 514},
                                                    {QStringLiteral("XDMCP"), 177}};
        const bool usual = session->port == 0
                           || session->port == usualPorts.value(session->protocol.toUpper(), -1);
        item->setText(HostColumn, usual ? session->host
                                        : QStringLiteral("%1:%2").arg(session->host).arg(session->port));
        item->setText(ProtocolColumn, session->protocol);
    }
    applySessionFilter();
    m_sessionTree->viewport()->update();
    fitSessionPanel();
}

// Search Sessions (Ctrl+Shift+S): the Sessions tab, with the search box ready.
void MainWindow::focusSessionSearch()
{
    if (!m_leftTabs->isVisible())
        m_leftTabs->show();
    m_leftTabs->setCurrentIndex(0);
    m_sessionSearch->show();
    m_sessionSearch->setFocus();
    m_sessionSearch->selectAll();
}

// The first session shown in the list (Pinned and Recent count), or nullptr.
QTreeWidgetItem *MainWindow::firstVisibleSession() const
{
    for (QTreeWidgetItemIterator it(m_sessionTree, QTreeWidgetItemIterator::NotHidden); *it; ++it) {
        const auto kind = (*it)->data(0, ItemKindRole).toString();
        if (kind != QStringLiteral("session") && kind != QStringLiteral("clone"))
            continue;
        // Not inside a folder that was closed by hand.
        bool open = true;
        for (auto *parent = (*it)->parent(); parent && open; parent = parent->parent())
            open = parent->isExpanded();
        if (open)
            return *it;
    }
    return nullptr;
}

// Shows only the sessions matching every word of the search (in their name,
// host, user, protocol, or folder path) and the folders leading to them.
void MainWindow::applySessionFilter()
{
    if (!m_sessionSearch || !m_sessionTree)
        return;
    const QStringList words = m_sessionSearch->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const bool filtering = !words.isEmpty();
    if (filtering && !m_filtering) {
        m_pinnedExpandedBeforeSearch = !m_pinnedFolder || m_pinnedFolder->isExpanded();
        m_recentExpandedBeforeSearch = !m_recentFolder || m_recentFolder->isExpanded();
    }
    const bool ending = !filtering && m_filtering;
    m_filtering = filtering || ending; // expansions below are not remembered
    int shown = 0;
    std::function<bool(QTreeWidgetItem *, const QString &)> apply =
        [&](QTreeWidgetItem *item, const QString &path) -> bool {
        const auto kind = item->data(0, ItemKindRole).toString();
        bool visible = true;
        if (kind == QStringLiteral("folder") || kind == QStringLiteral("quick")) {
            const QString inner = path + item->text(0) + QLatin1Char('/');
            bool any = false;
            for (int index = 0; index < item->childCount(); ++index)
                any = apply(item->child(index), inner) || any;
            visible = !filtering || any;
            if (filtering && any && !item->isExpanded())
                item->setExpanded(true);
        } else if (filtering) {
            const auto session = m_sessions.value(item->data(0, SessionIdRole).toString());
            const QString text = QStringList{path, session.name, session.host, session.username,
                                             session.protocol}
                                     .join(QLatin1Char(' '));
            for (const auto &word : words)
                visible = visible && text.contains(word, Qt::CaseInsensitive);
            shown += visible ? 1 : 0;
        }
        item->setHidden(!visible);
        return visible;
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        apply(m_sessionTree->topLevelItem(index), QString());
    if (ending) {
        // Back to the folders as they were before searching.
        restoreFolderExpansion();
        if (m_pinnedFolder)
            m_pinnedFolder->setExpanded(m_pinnedExpandedBeforeSearch);
        if (m_recentFolder)
            m_recentFolder->setExpanded(m_recentExpandedBeforeSearch);
        if (auto *current = m_sessionTree->currentItem())
            m_sessionTree->scrollToItem(current);
    }
    m_filtering = filtering;
    m_noSessionMatches->setVisible(filtering && shown == 0);
}

// Grows or shrinks the side panel to show the list without cutting it off.
void MainWindow::fitSessionPanel()
{
    if (!m_leftTabs || !m_leftTabs->isVisible())
        return;
    // The columns always fit their contents, so names are not cut off; the
    // panel follows only with "Always fit the panel to the list".
    // The width wanted is the contents', measured with the last column not
    // stretched: it fills the panel otherwise, and reading that back made
    // every fit a little wider than the one before.
    int width = 0;
    auto *header = m_sessionTree->header();
    header->setStretchLastSection(false);
    for (int column = 0; column < m_sessionTree->columnCount(); ++column) {
        if (m_sessionTree->isColumnHidden(column))
            continue;
        m_sessionTree->resizeColumnToContents(column);
        width += m_sessionTree->columnWidth(column);
    }
    header->setStretchLastSection(true);
    auto *splitter = qobject_cast<QSplitter *>(m_leftTabs->parentWidget());
    if (!m_sessionList.fitPanel || !splitter)
        return;
    width += 2 * m_sessionTree->frameWidth() + 8;
    if (m_sessionTree->verticalScrollBar()->isVisible())
        width += m_sessionTree->verticalScrollBar()->width();
    width += m_leftTabs->width() - m_sessionTree->width();
    const int total = splitter->width();
    const int wanted = qBound(m_leftTabs->minimumWidth(), width, qMax(m_leftTabs->minimumWidth(), total / 2));
    auto sizes = splitter->sizes();
    if (sizes.size() == 2 && sizes[0] != wanted && total > 0) {
        sizes[1] = total - wanted - splitter->handleWidth();
        sizes[0] = wanted;
        splitter->setSizes(sizes);
    }
}

// Knocks on every saved session's port. Sessions through a jump host or a
// proxy are not checked: this computer may not reach them directly.
void MainWindow::checkHostStatus()
{
    if (!m_sessionList.checkStatus)
        return;
    QList<HostStatusChecker::Target> targets;
    for (const auto &session : std::as_const(m_sessions)) {
        if (session.protocol.compare(QStringLiteral("Serial"), Qt::CaseInsensitive) == 0) {
            targets.append({session.id, session.host, 0, true});
            continue;
        }
        // XDMCP is UDP: a TCP check cannot tell.
        if (session.protocol.compare(QStringLiteral("XDMCP"), Qt::CaseInsensitive) == 0)
            continue;
        const auto jump = effectiveJumpHost(session);
        if ((!jump.isEmpty() && jump != JumpHost::Direct)
            || effectiveProxy(session).type != ProxySettings::Type::None)
            continue;
        QString host = session.host;
        quint16 port = session.port;
        const QString protocol = session.protocol.toUpper();
        if (port == 0) {
            if (protocol == QStringLiteral("RDP"))
                port = 3389;
            else if (protocol == QStringLiteral("VNC")) {
                port = 5900;
                // "host:1" is VNC display 1.
                if (host.count(QLatin1Char(':')) == 1) {
                    port = quint16(5900 + host.section(QLatin1Char(':'), 1).toInt());
                    host = host.section(QLatin1Char(':'), 0, 0);
                }
            } else if (protocol == QStringLiteral("TELNET"))
                port = 23;
            else if (protocol == QStringLiteral("RLOGIN"))
                port = 513;
            else if (protocol == QStringLiteral("RSH"))
                port = 514;
            else if (protocol == QStringLiteral("FTP"))
                port = session.ftp.security == QStringLiteral("implicit")
                           || (session.ftp.security.isEmpty()
                               && loadFtpDefaults().security == QStringLiteral("implicit"))
                    ? 990
                    : 21;
            else
                port = 22; // SSH, Mosh
        }
        if (!host.isEmpty())
            targets.append({session.id, host, port, false});
    }
    m_hostStatus->check(targets);
}

// ---- Split tabs ----

QList<TerminalPage *> MainWindow::terminalPagesIn(QWidget *tab) const
{
    if (auto *split = qobject_cast<SplitTab *>(tab))
        return split->panes();
    if (auto *terminal = qobject_cast<TerminalPage *>(tab))
        return {terminal};
    return {};
}

QList<TerminalPage *> MainWindow::allTerminalPages() const
{
    QList<TerminalPage *> pages;
    for (int index = 0; index < m_sessionTabs->count(); ++index)
        pages += terminalPagesIn(m_sessionTabs->widget(index));
    return pages;
}

// The terminal a tab's commands act on: the tab's, or its active pane.
TerminalPage *MainWindow::activeTerminal(QWidget *tab) const
{
    if (auto *split = qobject_cast<SplitTab *>(tab))
        return split->activePane();
    return qobject_cast<TerminalPage *>(tab);
}

// The tab showing this page, as itself or as a pane; -1 if none.
int MainWindow::tabIndexOf(QWidget *page) const
{
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *tab = m_sessionTabs->widget(index);
        if (tab == page || (qobject_cast<SplitTab *>(tab) && tab->isAncestorOf(page)))
            return index;
    }
    return -1;
}

// Split Right / Split Down: asks what to open, then opens it the usual way;
// addSessionTab() puts the new terminal next to the active pane.
// what: "same" (the active pane's session, or a local terminal for a local
// one), "local", or "choose" (the New Pane chooser).
void MainWindow::splitActivePane(Qt::Orientation orientation, const QString &what)
{
    auto *beside = activeTerminal(m_sessionTabs->currentWidget());
    if (!beside) {
        statusBar()->showMessage(QStringLiteral("Only terminal tabs can be split"), 4000);
        return;
    }
    const QString baseId = beside->property("sessionId").toString().section(QLatin1Char('#'), 0, 0);
    if (what == QStringLiteral("local")) {
        openInSplit(orientation, QStringLiteral("local"));
    } else if (what == QStringLiteral("same") && (baseId.isEmpty() || m_sessions.contains(baseId))) {
        // A local terminal (no session) splits into another local terminal.
        openInSplit(orientation, baseId.isEmpty() ? QStringLiteral("local") : baseId);
    } else if (const auto choice = choosePaneSession(baseId)) {
        openInSplit(orientation, *choice);
    }
}

// Split Right ▸ / Split Down ▸ for the right-click menus.
void MainWindow::addSplitMenus(QMenu *menu, const std::function<bool()> &makeActive)
{
    for (const auto &[label, orientation] : {std::pair{QStringLiteral("Split Right"), Qt::Horizontal},
                                             std::pair{QStringLiteral("Split Down"), Qt::Vertical}}) {
        auto *sub = menu->addMenu(label);
        const auto add = [this, sub, makeActive, orientation](const QString &text, const QString &what) {
            sub->addAction(text, this, [this, makeActive, orientation, what] {
                if (makeActive())
                    splitActivePane(orientation, what);
            });
        };
        add(QStringLiteral("Same Session"), QStringLiteral("same"));
        if (LocalTerminalSupported)
            add(QStringLiteral("Local Terminal"), QStringLiteral("local"));
        add(QStringLiteral("Other Session…"), QStringLiteral("choose"));
    }
}

// Opens "local" or a saved session's id next to the current tab's active
// terminal.
void MainWindow::openInSplit(Qt::Orientation orientation, const QString &choice)
{
    auto *beside = activeTerminal(m_sessionTabs->currentWidget());
    if (!beside)
        return;
    m_pendingSplit = {beside, orientation};
    if (choice == QStringLiteral("local")) {
        openLocalTerminal();
    } else if (choice == QStringLiteral("local-administrator")) {
        openLocalTerminalAsAdministrator();
    } else if (const auto session = m_sessions.constFind(choice); session != m_sessions.cend()) {
        // A copy under its own id, so the open tab of the session is not
        // simply brought forward.
        auto copy = *session;
        copy.id = session->id + QStringLiteral("#") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        openSession(copy);
    }
    // Nothing opened (a locked vault, a cancelled prompt): no split waits.
    m_pendingSplit = {};
}

// What a new pane shows: the same session (the default), a local terminal,
// or another saved terminal session. nullopt when cancelled.
std::optional<QString> MainWindow::choosePaneSession(const QString &currentId)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New Pane"));
    dialog.setMinimumSize(420, 380);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(QStringLiteral("Search sessions"));
    filter->setClearButtonEnabled(true);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Open in the new pane:"), &dialog));
    layout->addWidget(filter);
    layout->addWidget(list, 1);
    const auto add = [list](const QString &text, const QString &value, const QIcon &icon) {
        auto *item = new QListWidgetItem(icon, text, list);
        item->setData(Qt::UserRole, value);
    };
    const QIcon terminalIcon = themedIcon(QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    const QIcon sessionIcon = themedIcon(QStringLiteral("network-server"), QStyle::SP_ComputerIcon);
    if (m_sessions.contains(currentId))
        add(QStringLiteral("Same session: %1").arg(m_sessions.value(currentId).name), currentId, sessionIcon);
    if (LocalTerminalSupported)
        add(QStringLiteral("Local terminal"), QStringLiteral("local"), terminalIcon);
#ifdef Q_OS_WIN
    if (ConPtyConnection::isAvailable() && m_localShell.windowsShell != QStringLiteral("wsl"))
        add(QStringLiteral("Local terminal as administrator"), QStringLiteral("local-administrator"),
            style()->standardIcon(QStyle::SP_VistaShield));
#endif
    // Saved terminal sessions, in the list's order, with their folder.
    for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
        if ((*it)->data(0, ItemKindRole).toString() != QStringLiteral("session"))
            continue;
        const auto session = m_sessions.value((*it)->data(0, SessionIdRole).toString());
        if (!isTerminalProtocol(session.protocol))
            continue;
        QStringList path;
        for (auto *parent = (*it)->parent(); parent; parent = parent->parent())
            path.prepend(parent->text(0));
        path.append(session.name);
        add(QStringLiteral("%1  (%2)").arg(path.join(QStringLiteral(" / ")), session.protocol),
            session.id, sessionIcon);
    }
    list->setCurrentRow(0);
    connect(filter, &QLineEdit::textChanged, list, [list](const QString &text) {
        const auto words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QListWidgetItem *first = nullptr;
        for (int row = 0; row < list->count(); ++row) {
            auto *item = list->item(row);
            bool match = true;
            for (const auto &word : words)
                match = match && item->text().contains(word, Qt::CaseInsensitive);
            item->setHidden(!match);
            if (match && !first)
                first = item;
        }
        list->setCurrentItem(first);
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
    // Up and Down in the search box move through the list.
    filter->installEventFilter(&dialog);
    struct Keys : QObject
    {
        QListWidget *list;
        bool eventFilter(QObject *, QEvent *event) override
        {
            if (event->type() != QEvent::KeyPress)
                return false;
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key != Qt::Key_Down && key != Qt::Key_Up)
                return false;
            int row = list->currentRow();
            do
                row += key == Qt::Key_Down ? 1 : -1;
            while (row >= 0 && row < list->count() && list->item(row)->isHidden());
            if (row >= 0 && row < list->count())
                list->setCurrentRow(row);
            return true;
        }
    };
    auto *keys = new Keys;
    keys->setParent(&dialog);
    keys->list = list;
    filter->removeEventFilter(&dialog);
    filter->installEventFilter(keys);
    filter->setFocus();
    if (dialog.exec() != QDialog::Accepted || !list->currentItem())
        return std::nullopt;
    return list->currentItem()->data(Qt::UserRole).toString();
}

// Puts a new terminal next to beside, turning beside's tab into a split tab.
void MainWindow::insertPane(TerminalPage *beside, TerminalPage *added, Qt::Orientation orientation)
{
    const int index = tabIndexOf(beside);
    if (index < 0) {
        added->deleteLater();
        return;
    }
    auto *split = qobject_cast<SplitTab *>(m_sessionTabs->widget(index));
    if (!split) {
        // The tab's terminal moves into a split tab in the same place.
        const QString title = m_sessionTabs->tabText(index);
        const QString toolTip = m_sessionTabs->tabToolTip(index);
        const QIcon icon = m_sessionTabs->tabIcon(index);
        m_sessionTabs->removeTab(index);
        split = new SplitTab(beside, m_sessionTabs);
        connect(split, &SplitTab::activePaneChanged, this, [this, split] {
            if (m_sessionTabs->currentWidget() == split)
                showSftpPanelForCurrentTab();
        });
        m_sessionTabs->insertTab(index, split, icon, title);
        m_sessionTabs->setTabToolTip(index, toolTip);
    }
    split->split(beside, added, orientation);
    connect(added, &TerminalPage::closeRequested, this, [this, added] { closePane(added); });
    m_sessionTabs->setCurrentIndex(index);
    showSftpPanelForCurrentTab();
}

// Closes one terminal: the whole tab when it is alone, otherwise its pane
// (asking first when it is connected, like closing a tab).
void MainWindow::closePane(TerminalPage *pane)
{
    const int index = tabIndexOf(pane);
    if (index < 0)
        return;
    auto *split = qobject_cast<SplitTab *>(m_sessionTabs->widget(index));
    if (!split || split->panes().size() <= 1) {
        closeSessionTab(index);
        return;
    }
    const QString host = pane->sshSettings().host;
    if (m_startup.confirmCloseConnected && pane->isConnected() && !host.isEmpty()
        && QMessageBox::question(this, QStringLiteral("Close Pane"),
                                 QStringLiteral("Disconnect from %1 and close the pane?").arg(host))
               != QMessageBox::Yes)
        return;
    split->removePane(pane);
    pane->deleteLater();
    // One pane left: the tab becomes a plain terminal tab again.
    if (const auto remaining = split->panes(); remaining.size() == 1) {
        auto *last = remaining.constFirst();
        const QString title = m_sessionTabs->tabText(index);
        const QString toolTip = m_sessionTabs->tabToolTip(index);
        const QIcon icon = m_sessionTabs->tabIcon(index);
        split->removePane(last);
        m_sessionTabs->removeTab(index);
        m_sessionTabs->insertTab(index, last, icon, title);
        m_sessionTabs->setTabToolTip(index, toolTip);
        m_sessionTabs->setCurrentIndex(index);
        split->deleteLater();
        last->setFocus();
    }
    showSftpPanelForCurrentTab();
}

// ---- Multi-exec ----

// Tabs → Multi-exec…: which terminals (tabs and split panes) get the typing.
void MainWindow::chooseMultiExecTargets()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Multi-exec"));
    dialog.setMinimumSize(420, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(QStringLiteral("Typing into one of the chosen terminals goes to all of "
                                            "them. Chosen terminals have an accent bar on top."),
                             &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(list, 1);
    QList<QPointer<TerminalPage>> pages;
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        const auto terminals = terminalPagesIn(m_sessionTabs->widget(index));
        for (int pane = 0; pane < terminals.size(); ++pane) {
            auto *terminal = terminals.at(pane);
            QString text = m_sessionTabs->tabText(index);
            if (terminals.size() > 1)
                text += QStringLiteral("  (pane %1)").arg(pane + 1);
            if (!terminal->sshSettings().host.isEmpty())
                text += QStringLiteral(" — %1").arg(terminal->sshSettings().host);
            auto *item = new QListWidgetItem(text, list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(terminal->isMultiExecTarget() ? Qt::Checked : Qt::Unchecked);
            pages.append(terminal);
        }
    }
    if (pages.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Multi-exec"),
                                 QStringLiteral("Open some terminals first."));
        return;
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *all = buttons->addButton(QStringLiteral("All"), QDialogButtonBox::ActionRole);
    auto *none = buttons->addButton(QStringLiteral("None"), QDialogButtonBox::ActionRole);
    const auto checkAll = [list](Qt::CheckState state) {
        for (int row = 0; row < list->count(); ++row)
            list->item(row)->setCheckState(state);
    };
    connect(all, &QPushButton::clicked, list, [checkAll] { checkAll(Qt::Checked); });
    connect(none, &QPushButton::clicked, list, [checkAll] { checkAll(Qt::Unchecked); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    for (int row = 0; row < list->count() && row < pages.size(); ++row) {
        if (pages.at(row))
            pages.at(row)->setMultiExecTarget(list->item(row)->checkState() == Qt::Checked);
    }
    updateMultiExecIndicator();
}

void MainWindow::updateMultiExecIndicator()
{
    int chosen = 0;
    for (auto *page : allTerminalPages())
        chosen += page->isMultiExecTarget() ? 1 : 0;
    m_multiExecIndicator->setText(QStringLiteral("MULTI-EXEC · %1  ✕").arg(chosen));
    m_multiExecIndicator->setVisible(chosen > 0);
}

// Preferences → Security → Password storage: copies saved passwords from the
// vault to the system store, or the saved sessions' ones back. Returns what
// happened, for a message.
QString MainWindow::copyPasswords(bool toSystem)
{
    QString why;
    if (!SystemCredentials::isAvailable(&why))
        return why;
    if (m_vault.exists() && !m_vault.isUnlocked() && !ensureVaultUnlocked())
        return QStringLiteral("The vault stays locked; nothing was copied.");
    if (!toSystem && !m_vault.exists() && !ensureVaultUnlocked())
        return QStringLiteral("There is no vault to copy into.");
    int copied = 0;
    QStringList problems;
    if (toSystem) {
        for (const auto &key : m_vault.vaultKeys()) {
            QString error;
            if (SystemCredentials::write(key, m_vault.vaultPassword(key), &error))
                ++copied;
            else
                problems.append(error);
        }
    } else {
        for (const auto &session : std::as_const(m_sessions)) {
            const QString password = SystemCredentials::read(session.id);
            if (password.isEmpty())
                continue;
            QString error;
            if (m_vault.setVaultPassword(session.id, password, &error))
                ++copied;
            else
                problems.append(error);
        }
    }
    problems.removeDuplicates();
    QString message = QStringLiteral("Copied %1 password(s) to %2.")
                          .arg(copied)
                          .arg(toSystem ? SystemCredentials::name() : QStringLiteral("the vault"));
    if (!problems.isEmpty())
        message += QStringLiteral("\n\n") + problems.join(QLatin1Char('\n'));
    return message;
}

// "Keep the password in": the session's own store, or the Preferences'.
QComboBox *MainWindow::passwordStoreChoice(const QString &value, QWidget *parent) const
{
    static const QHash<QString, QString> labels{{QStringLiteral("vault"), QStringLiteral("OpsRemote vault")},
                                                {QStringLiteral("system"), QStringLiteral("System credential store")},
                                                {QStringLiteral("both"), QStringLiteral("Both")}};
    auto *box = new QComboBox(parent);
    box->addItem(QStringLiteral("Default (%1)").arg(labels.value(m_security.passwordStore)), QString());
    for (const auto *key : {"vault", "system", "both"})
        box->addItem(labels.value(QLatin1String(key)), QLatin1String(key));
    box->setCurrentIndex(qMax(0, box->findData(value)));
    QString why;
    const bool available = SystemCredentials::isAvailable(&why);
    box->setToolTip(QStringLiteral("The vault travels with the portable folder; the system store (%1) "
                                   "needs no master password but stays on this computer.%2")
                        .arg(SystemCredentials::name(),
                             available ? QString() : QStringLiteral("\n\nNot available here: ") + why));
    // Without a system store, its choices are off (a session's current one stays).
    if (!available) {
        if (auto *model = qobject_cast<QStandardItemModel *>(box->model())) {
            for (const auto *key : {"system", "both"}) {
                const int row = box->findData(QLatin1String(key));
                if (auto *item = model->item(row); item && row != box->currentIndex())
                    item->setEnabled(false);
            }
        }
    }
    return box;
}

// Save in the session window: checks the fields first and keeps the window
// open with a message in it (and the missing fields marked) until they are
// complete, instead of closing and complaining in another window.
void MainWindow::validateSessionDialog(QDialog *dialog, QFormLayout *form, QDialogButtonBox *buttons,
                                       QComboBox *protocolCombo, QLineEdit *nameEdit,
                                       QLineEdit *hostEdit, QLineEdit *portEdit,
                                       WakeOnLanEditor *wakeEditor)
{
    auto *message = new QLabel(dialog);
    message->setWordWrap(true);
    message->setStyleSheet(QStringLiteral("color: #c0392b; font-weight: bold;"));
    message->hide();
    form->insertRow(form->rowCount() - 1, message);
    const QString marked = QStringLiteral("QLineEdit { border: 1px solid #d9534f; border-radius: 3px; }");
    for (auto *field : {nameEdit, hostEdit, portEdit}) {
        connect(field, &QLineEdit::textEdited, field, [field, message] {
            field->setStyleSheet(QString());
            message->hide();
        });
    }
    connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        const bool serial = protocolCombo->currentText() == QStringLiteral("Serial");
        QStringList missing;
        QLineEdit *first = nullptr;
        const auto require = [&](QLineEdit *field, bool ok, const QString &label) {
            field->setStyleSheet(ok ? QString() : marked);
            if (!ok) {
                missing.append(label);
                if (!first)
                    first = field;
            }
        };
        require(nameEdit, !nameEdit->text().trimmed().isEmpty(), QStringLiteral("Name"));
        require(hostEdit, !hostEdit->text().trimmed().isEmpty(),
                serial ? QStringLiteral("Port (the serial port)") : QStringLiteral("Host"));
        QString text;
        if (!missing.isEmpty())
            text = QStringLiteral("Fill in the required fields to save the connection: %1.")
                       .arg(missing.join(QStringLiteral(", ")));
        bool portOk = true;
        const QString port = portEdit->text().trimmed();
        if (!serial && !port.isEmpty()) {
            const uint number = port.toUInt(&portOk);
            portOk = portOk && number >= 1 && number <= 65535;
        }
        if (!portOk) {
            portEdit->setStyleSheet(marked);
            if (!first)
                first = portEdit;
            text += (text.isEmpty() ? QString() : QStringLiteral(" "))
                    + QStringLiteral("The port must be a number from 1 to 65535, or empty for the "
                                     "protocol's usual one.");
        }
        if (text.isEmpty()) {
            if (const auto problem = wakeEditor->problem(); !problem.isEmpty())
                text = QStringLiteral("Wake-on-LAN: %1").arg(problem);
        }
        if (text.isEmpty()) {
            dialog->accept();
            return;
        }
        message->setText(text);
        message->show();
        if (first)
            first->setFocus();
    });
}

