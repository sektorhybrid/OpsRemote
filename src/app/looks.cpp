// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "looks.h"
#include "appdirs.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <yaml-cpp/yaml.h>

namespace {

QString looksFilePath()
{
    return QDir(portableFilePath(QStringLiteral("settings"))).filePath(QStringLiteral("looks.yaml"));
}

QString text(const YAML::Node &node, const char *key)
{
    return QString::fromStdString(node[key].as<std::string>(""));
}

Look builtIn(const char *id, const char *name, const char *colorTheme, const char *style,
             const char *iconTheme, const char *terminalScheme, const char *darkTerminalScheme)
{
    Look look;
    look.id = QStringLiteral("builtin:") + QLatin1String(id);
    look.name = QString::fromUtf8(name);
    look.colorTheme = QLatin1String(colorTheme);
    look.style = QLatin1String(style);
    look.iconTheme = QLatin1String(iconTheme);
    look.terminalScheme = QString::fromUtf8(terminalScheme);
    look.darkTerminalScheme = QString::fromUtf8(darkTerminalScheme);
    return look;
}

}

bool Look::sameAppearance(const Look &other) const
{
    // Custom colours only matter for the custom colour mode.
    return colorTheme == other.colorTheme
           && (colorTheme != QStringLiteral("custom") || customColors == other.customColors)
           && style.compare(other.style, Qt::CaseInsensitive) == 0 && iconTheme == other.iconTheme
           && terminalScheme == other.terminalScheme
           && darkTerminalScheme == other.darkTerminalScheme
           && terminalFontFamily == other.terminalFontFamily
           && terminalFontSize == other.terminalFontSize
           && interfaceFontFamily == other.interfaceFontFamily
           && interfaceFontSize == other.interfaceFontSize && windowBorder == other.windowBorder;
}

QList<Look> builtInLooks()
{
    QList<Look> looks{
        // Follows the operating system's light or dark setting.
        builtIn("opsremote", "OpsRemote", "auto", "opsremote", "Papirus", "Atom One Light",
                "Atom One Dark"),
        builtIn("opsremote-light", "OpsRemote Light Blue", "light", "opsremote", "Papirus",
                "Atom One Light", ""),
        builtIn("opsremote-light-orange", "OpsRemote Light Orange", "light", "opsremote",
                "Papirus-Orange", "Nord", ""),
        builtIn("opsremote-dark", "OpsRemote Dark", "dark", "opsremote", "Papirus-Dark",
                "Atom One Dark", ""),
        builtIn("nord", "Nord", "nord", "opsremote", "Papirus-Dark", "Nord", ""),
        builtIn("dracula", "Dracula", "dracula", "opsremote", "Papirus-Dark", "Dracula", ""),
        builtIn("gruvbox", "Gruvbox", "gruvbox-dark", "opsremote", "Papirus-Dark", "Gruvbox Dark", ""),
        builtIn("solarized", "Solarized", "solarized-light", "opsremote", "Papirus",
                "SolarizedLight", ""),
        // Plain Qt: the system's colours and Fusion.
        builtIn("classic", "Classic", "system", "fusion", "", "", ""),
        // Kvantum themes, with their own colours.
        builtIn("kvantum-arc", "Arc", "system", "kvantum:KvArc", "Papirus", "Atom One Light", ""),
        builtIn("kvantum-arc-dark", "Arc Dark", "system", "kvantum:KvArcDark", "Papirus-Dark",
                "Atom One Dark", ""),
        builtIn("kvantum-mojave", "Mojave", "system", "kvantum:KvMojaveLight", "WhiteSur",
                "Atom One Light", ""),
        builtIn("kvantum-mojave-orange", "Mojave Orange", "system", "kvantum:KvMojaveLightOrange",
                "Papirus-Orange", "Nord", ""),
        builtIn("kvantum-adapta-dark", "Adapta Dark", "system", "kvantum:KvAdaptaDark",
                "Papirus-Dark", "Atom One Dark", ""),
    };
#ifdef Q_OS_WIN
    looks.append(builtIn("windows11", "Windows 11", "auto", "windows11", "Fluent", "Atom One Light",
                         "Atom One Dark"));
#endif
    for (auto &look : looks) {
        // Classic and Windows 11 look like the system's own windows.
        if (look.id == QStringLiteral("builtin:classic") || look.style == QStringLiteral("windows11"))
            look.windowBorder = false;
    }
    return looks;
}

QList<Look> loadUserLooks()
{
    QList<Look> looks;
    try {
        for (const auto &node : YAML::LoadFile(looksFilePath().toStdString())["looks"]) {
            Look look;
            look.id = text(node, "id");
            look.name = text(node, "name");
            if (!look.id.startsWith(QStringLiteral("user:")) || look.name.isEmpty())
                continue;
            look.colorTheme = text(node, "color_theme");
            for (const auto &color : node["custom_colors"])
                look.customColors.insert(QString::fromStdString(color.first.as<std::string>()),
                                         QColor(QString::fromStdString(color.second.as<std::string>())));
            look.style = text(node, "style");
            look.iconTheme = text(node, "icon_theme");
            look.terminalScheme = text(node, "terminal_scheme");
            look.darkTerminalScheme = text(node, "dark_terminal_scheme");
            look.terminalFontFamily = text(node, "terminal_font_family");
            look.terminalFontSize = qBound(0, node["terminal_font_size"].as<int>(0), 72);
            look.interfaceFontFamily = text(node, "interface_font_family");
            look.interfaceFontSize = qBound(0, node["interface_font_size"].as<int>(0), 48);
            look.windowBorder = node["window_border"].as<bool>(true);
            looks.append(look);
            if (looks.size() == MaximumUserLooks)
                break;
        }
    } catch (const YAML::Exception &) {
        // No saved looks yet, or an unreadable file.
    }
    return looks;
}

void saveUserLooks(const QList<Look> &looks)
{
    YAML::Node list(YAML::NodeType::Sequence);
    for (const auto &look : looks) {
        YAML::Node node;
        node["id"] = look.id.toStdString();
        node["name"] = look.name.toStdString();
        node["color_theme"] = look.colorTheme.toStdString();
        if (look.colorTheme == QStringLiteral("custom")) {
            for (auto it = look.customColors.cbegin(); it != look.customColors.cend(); ++it)
                node["custom_colors"][it.key().toStdString()] = it.value().name().toStdString();
        }
        node["style"] = look.style.toStdString();
        node["icon_theme"] = look.iconTheme.toStdString();
        node["terminal_scheme"] = look.terminalScheme.toStdString();
        node["dark_terminal_scheme"] = look.darkTerminalScheme.toStdString();
        node["terminal_font_family"] = look.terminalFontFamily.toStdString();
        node["terminal_font_size"] = look.terminalFontSize;
        node["interface_font_family"] = look.interfaceFontFamily.toStdString();
        node["interface_font_size"] = look.interfaceFontSize;
        node["window_border"] = look.windowBorder;
        list.push_back(node);
    }
    YAML::Node document;
    document["looks"] = list;
    const auto path = looksFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;
    YAML::Emitter emitter;
    emitter << document;
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
