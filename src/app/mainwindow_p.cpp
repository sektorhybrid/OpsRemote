// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "mainwindow_p.h"

namespace mainwindow_p {

QString stateDirectory()
{
    return portableFilePath(QStringLiteral("settings"));
}

QString stateFilePath(const QString &name)
{
    return QDir(stateDirectory()).filePath(name);
}

QStringList sessionProtocols()
{
    return QStringList{QStringLiteral("SSH"), QStringLiteral("VNC"), QStringLiteral("RDP"),
                       QStringLiteral("Telnet"), QStringLiteral("Mosh"), QStringLiteral("Serial"),
                       QStringLiteral("FTP"), QStringLiteral("SFTP"), QStringLiteral("SCP"),
                       QStringLiteral("Rlogin"), QStringLiteral("RSH"), QStringLiteral("XDMCP")}
           + localSessionProtocols();
}

bool isSshProtocol(const QString &protocol)
{
    for (const auto *name : {"SSH", "SFTP", "SCP"}) {
        if (protocol.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool isTerminalProtocol(const QString &protocol)
{
    for (const auto *name : {"SSH", "Telnet", "Mosh", "Serial", "Rlogin", "RSH"}) {
        if (protocol.compare(QLatin1String(name), Qt::CaseInsensitive) == 0)
            return true;
    }
    return isLocalSessionProtocol(protocol);
}

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
    const auto ui = loadPreferencesSection("ui");
    if (ui["style"])
        return QString::fromStdString(ui["style"].as<std::string>("system"));
    return QStringLiteral("system");
}

QString loadColorThemePreference()
{
    const auto ui = loadPreferencesSection("ui");
    if (ui["color_theme"])
        return QString::fromStdString(ui["color_theme"].as<std::string>("system"));
    return QStringLiteral("system");
}

void disableLocalTerminalWhereUnsupported(QAction *action)
{
    if (const auto problem = Platform::problem(Platform::Feature::LocalTerminals); !problem.isEmpty()) {
        action->setEnabled(false);
        action->setToolTip(problem);
    }
}

QString loadIconThemePreference()
{
    const auto ui = loadPreferencesSection("ui");
    if (ui["icon_theme"])
        return QString::fromStdString(ui["icon_theme"].as<std::string>(""));
    return QString();
}

void saveIconThemePreference(const QString &themeName)
{
    auto ui = loadPreferencesSection("ui");
    ui["icon_theme"] = themeName.toStdString();
    savePreferencesSection("ui", ui);
}

QHash<QString, QColor> loadCustomColors()
{
    QHash<QString, QColor> colors;
    try {
        const auto values = loadPreferencesSection("ui")["custom_colors"];
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
    auto ui = loadPreferencesSection("ui");
    ui["color_theme"] = themeKey.toStdString();
    for (auto iterator = customColors.cbegin(); iterator != customColors.cend(); ++iterator)
        ui["custom_colors"][iterator.key().toStdString()] =
            iterator.value().name(QColor::HexRgb).toStdString();
    savePreferencesSection("ui", ui);
}

void saveUiStylePreference(const QString &styleKey)
{
    auto ui = loadPreferencesSection("ui");
    ui["style"] = styleKey.toStdString();
    savePreferencesSection("ui", ui);
}

QHash<QString, bool> loadFolderExpansionStates()
{
    QHash<QString, bool> states;
    try {
        const auto folders = loadPreferencesSection("session_tree")["folder_expansion"];
        if (!folders || !folders.IsMap())
            return states;
        for (const auto &entry : folders)
            states.insert(QString::fromStdString(entry.first.as<std::string>()),
                          entry.second.as<bool>(true));
    } catch (const YAML::Exception &) {
        // Missing or invalid tree state: every folder starts collapsed.
    }
    return states;
}

void saveFolderExpansionStates(const QHash<QString, bool> &states)
{
    YAML::Node folders(YAML::NodeType::Map);
    for (auto iterator = states.cbegin(); iterator != states.cend(); ++iterator)
        folders[iterator.key().toStdString()] = iterator.value();
    auto tree = loadPreferencesSection("session_tree");
    tree["folder_expansion"] = folders;
    savePreferencesSection("session_tree", tree);
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
        if (session.terminal.linksEnabled >= 0)
            terminal["links_enabled"] = session.terminal.linksEnabled == 1;
        if (session.terminal.confirmMultilinePaste >= 0)
            terminal["confirm_multiline_paste"] = session.terminal.confirmMultilinePaste == 1;
        if (session.terminal.uploadDroppedFiles >= 0)
            terminal["upload_dropped_files"] = session.terminal.uploadDroppedFiles == 1;
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
    if (!session.telnet.isEmpty()) {
        YAML::Node telnet(YAML::NodeType::Map);
        telnetOptionsToYaml(session.telnet, telnet);
        node["telnet"] = telnet;
    }
    if (!session.xdmcp.isEmpty()) {
        YAML::Node xdmcp(YAML::NodeType::Map);
        xdmcpOptionsToYaml(session.xdmcp, xdmcp);
        node["xdmcp"] = xdmcp;
    }
    if (!session.local.isEmpty()) {
        YAML::Node local(YAML::NodeType::Map);
        localSessionOptionsToYaml(session.local, local);
        node["local"] = local;
    }
    if (!session.automation.isEmpty()) {
        YAML::Node automation(YAML::NodeType::Map);
        automationToYaml(session.automation, automation);
        node["automation"] = automation;
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
    if (node["links_enabled"])
        appearance.linksEnabled = node["links_enabled"].as<bool>() ? 1 : 0;
    if (node["confirm_multiline_paste"])
        appearance.confirmMultilinePaste = node["confirm_multiline_paste"].as<bool>() ? 1 : 0;
    if (node["upload_dropped_files"])
        appearance.uploadDroppedFiles = node["upload_dropped_files"].as<bool>() ? 1 : 0;
    return appearance;
}

InterfaceFont loadInterfaceFont()
{
    InterfaceFont font;
    try {
        const auto ui = loadPreferencesSection("ui");
        font.family = QString::fromStdString(ui["font_family"].as<std::string>(""));
        font.pointSize = qBound(0, ui["font_size"].as<int>(0), 48);
    } catch (const YAML::Exception &) {
        // Invalid values keep the system font.
    }
    return font;
}

void saveInterfaceFont(const InterfaceFont &font)
{
    auto ui = loadPreferencesSection("ui");
    ui["font_family"] = font.family.toStdString();
    ui["font_size"] = font.pointSize;
    savePreferencesSection("ui", ui);
}

QString loadBaseLookId()
{
    try {
        if (const auto ui = loadPreferencesSection("ui"); ui["base_look"])
            return QString::fromStdString(ui["base_look"].as<std::string>());
    } catch (const YAML::Exception &) {
        // An invalid value: none.
    }
    return QString();
}

void applyInterfaceFont(const QFont &systemFont, const InterfaceFont &choice)
{
    QFont font = systemFont;
    if (!choice.family.isEmpty())
        font.setFamily(choice.family);
    if (choice.pointSize > 0)
        font.setPointSize(choice.pointSize);
    QApplication::setFont(font);
    ::refreshWidgetFonts();
}

bool loadWindowBorderPreference()
{
    try {
        if (const auto ui = loadPreferencesSection("ui"); ui["window_border"])
            return ui["window_border"].as<bool>();
    } catch (const YAML::Exception &) {
        // An invalid value uses the platform default.
    }
    return defaultWindowBorders();
}

void saveWindowBorderPreference(bool enabled)
{
    auto ui = loadPreferencesSection("ui");
    ui["window_border"] = enabled;
    savePreferencesSection("ui", ui);
}

int loadCornerRadiusPreference()
{
    try {
        if (const auto ui = loadPreferencesSection("ui"); ui["corner_radius"])
            return validCornerRadius(ui["corner_radius"].as<int>());
    } catch (const YAML::Exception &) {
        // An invalid value uses the default.
    }
    return Look().cornerRadius;
}

void saveCornerRadiusPreference(int radius)
{
    auto ui = loadPreferencesSection("ui");
    ui["corner_radius"] = radius;
    savePreferencesSection("ui", ui);
}

bool loadSeeThroughListsPreference()
{
    try {
        if (const auto ui = loadPreferencesSection("ui"); ui["see_through_lists"])
            return ui["see_through_lists"].as<bool>();
    } catch (const YAML::Exception &) {
        // An invalid value uses the default.
    }
    return Look().seeThroughLists;
}

void saveSeeThroughListsPreference(bool seeThrough)
{
    auto ui = loadPreferencesSection("ui");
    ui["see_through_lists"] = seeThrough;
    savePreferencesSection("ui", ui);
}

void saveDarkTerminalColorScheme(const QString &scheme)
{
    auto terminal = loadPreferencesSection("terminal");
    terminal["dark_color_scheme"] = scheme.toStdString();
    savePreferencesSection("terminal", terminal);
}

QString loadDarkTerminalColorScheme()
{
    const auto terminal = loadPreferencesSection("terminal");
    if (terminal["dark_color_scheme"])
        return QString::fromStdString(terminal["dark_color_scheme"].as<std::string>(""));
    return QString();
}

TerminalAppearance loadTerminalAppearance()
{
    try {
        return terminalAppearanceFromYaml(loadPreferencesSection("terminal"));
    } catch (const YAML::Exception &) {
        // Invalid values use the built-in terminal appearance.
    }
    return {};
}

void saveTerminalAppearance(const TerminalAppearance &appearance)
{
    // Only its own keys: others in terminal: (dark_color_scheme) stay.
    auto terminal = loadPreferencesSection("terminal");
    terminal["color_scheme"] = appearance.colorScheme.toStdString();
    terminal["font_family"] = appearance.fontFamily.toStdString();
    terminal["font_size"] = appearance.fontSize;
    savePreferencesSection("terminal", terminal);
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
    if (const auto kerberos = folder->data(0, FolderKerberosRole); kerberos.isValid())
        node["kerberos"] = kerberos.toInt() == 1;
    if (const auto automation = automationFromText(folder->data(0, FolderAutomationRole).toString());
        !automation.isEmpty()) {
        YAML::Node value(YAML::NodeType::Map);
        automationToYaml(automation, value);
        node["automation"] = value;
    }
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

// ---- The session list: Pinned, Recent, columns, and host status ----

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

} // namespace mainwindow_p
