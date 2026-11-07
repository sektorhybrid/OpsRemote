// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "settingsmigrations.h"

#include "appdirs.h"
#include "preferences.h"

#include <QFile>
#include <QSaveFile>

#include <yaml-cpp/yaml.h>

namespace {

// One change of the settings' shape: from version - 1 to version. Each step
// works on whatever it finds (a key may be missing: the user never changed
// it, or an earlier step already moved it) and never fails.
struct Step
{
    int version;
    const char *what; // for the log
    void (*apply)(YAML::Node &settings);
};

YAML::Node section(YAML::Node &settings, const char *name)
{
    auto node = settings[name];
    return node && node.IsMap() ? node : YAML::Node();
}

bool readBool(const YAML::Node &node, bool fallback)
{
    try {
        return node ? node.as<bool>() : fallback;
    } catch (const YAML::Exception &) {
        return fallback;
    }
}

// 0.6.1 → 0.6.2: behaviour.copy_on_select became one setting per system,
// as Linux and Windows share settings.yaml. It was saved also when left at
// its default (off): "on" was chosen by hand and stays on for both; "off"
// lets each system take its new default (on for both since 0.6.2; each can
// be turned off on its own).
void splitCopyOnSelect(YAML::Node &settings)
{
    auto behaviour = section(settings, "behaviour");
    if (!behaviour || !behaviour["copy_on_select"])
        return;
    if (!behaviour["copy_on_select_linux"] && !behaviour["copy_on_select_windows"]
        && readBool(behaviour["copy_on_select"], false)) {
        behaviour["copy_on_select_linux"] = true;
        behaviour["copy_on_select_windows"] = true;
    }
    behaviour.remove("copy_on_select");
}

// 0.6.1 → 0.6.2: asking before several lines was two settings, one for
// pasting into a terminal (behaviour.confirm_multiline_paste) and one for
// commands from the Commands tab (commands.confirm_multiline; in dev builds
// command_bar.confirm_multiline). One setting now covers both, the
// terminal's. Both were on by default; it stays on unless every one found
// was turned off: asking is the safe side, and a script pasted by accident
// runs line by line.
void mergeMultilineConfirmation(YAML::Node &settings)
{
    auto behaviour = section(settings, "behaviour");
    bool anyOn = false, found = false;
    for (const auto &[name, key] : {std::pair{"behaviour", "confirm_multiline_paste"},
                                    std::pair{"commands", "confirm_multiline"},
                                    std::pair{"command_bar", "confirm_multiline"}}) {
        auto node = section(settings, name);
        if (node && node[key]) {
            found = true;
            anyOn = anyOn || readBool(node[key], true);
        }
    }
    if (found && behaviour)
        behaviour["confirm_multiline_paste"] = anyOn;
    else if (found && !anyOn)
        settings["behaviour"]["confirm_multiline_paste"] = false;
    if (auto commands = section(settings, "commands"))
        commands.remove("confirm_multiline");
}

// 0.6.1 → 0.6.2: keys earlier versions left behind, which nothing reads.
// session_list.summary_in_column: true was folder_summary: column (read as
// such until now); startup.show_host_in_list became session_list.show_host
// (its old default, off, was saved anyway, so it is not carried over);
// command_bar: came from development builds and was replaced by commands:.
void removeLeftovers(YAML::Node &settings)
{
    if (auto list = section(settings, "session_list"); list && list["summary_in_column"]) {
        if (readBool(list["summary_in_column"], false) && !list["folder_summary"])
            list["folder_summary"] = "column";
        list.remove("summary_in_column");
    }
    if (auto startup = section(settings, "startup"))
        startup.remove("show_host_in_list");
    settings.remove("command_bar");
}

// 0.6.1 → 0.6.2: behaviour.links_enabled and ctrl_click_opens_links became one
// choice, behaviour.links: off when links were off; "menu" when they were on
// but Ctrl+click did not open them; else "ctrl-click" (both were on by
// default). Sessions keep their own on/off (sessions.yaml, unchanged).
void mergeLinkSettings(YAML::Node &settings)
{
    auto behaviour = section(settings, "behaviour");
    if (!behaviour || (!behaviour["links_enabled"] && !behaviour["ctrl_click_opens_links"]))
        return;
    if (!behaviour["links"]) {
        if (!readBool(behaviour["links_enabled"], true))
            behaviour["links"] = "off";
        else if (!readBool(behaviour["ctrl_click_opens_links"], true))
            behaviour["links"] = "menu";
        else
            behaviour["links"] = "ctrl-click";
    }
    behaviour.remove("links_enabled");
    behaviour.remove("ctrl_click_opens_links");
}

// 0.6.1 → 0.6.2: selecting, copying and pasting became one place for
// terminals and text fields, mouse_clipboard (Preferences → Workspace → Mouse
// and clipboard). Values move unchanged, with these rules:
// - copy_on_select_linux / _windows become one copy_on_select: on when either
//   was on (a choice made by hand), else not written (the default, on);
// - middle_click "clipboard" (paste the clipboard) becomes middle_click
//   "paste" with paste "clipboard"; "paste" stays and now follows the paste
//   rule (default: the newest of selection and clipboard);
// - startup.middle_click_closes becomes middle_click_closes_tab, and
//   startup.wheel_on_fields moves.
// A key already in mouse_clipboard (a second run) is not overwritten.
void moveMouseClipboard(YAML::Node &settings)
{
    auto behaviour = section(settings, "behaviour");
    auto startup = section(settings, "startup");
    const auto movable = [](const YAML::Node &from, const char *const *keys) {
        for (const auto *const *key = keys; *key; ++key)
            if (from && from[*key])
                return true;
        return false;
    };
    static const char *const behaviourKeys[] = {"copy_on_select_linux", "copy_on_select_windows", "right_click",
                                                "middle_click", "confirm_multiline_paste",
                                                "edit_multiline_paste", "trim_pasted_newlines",
                                                "word_characters", "links", nullptr};
    static const char *const startupKeys[] = {"middle_click_closes", "wheel_on_fields", nullptr};
    if (!movable(behaviour, behaviourKeys) && !movable(startup, startupKeys))
        return;
    auto target = settings["mouse_clipboard"];
    if (!target || !target.IsMap())
        target = YAML::Node(YAML::NodeType::Map);
    const auto move = [&target](YAML::Node from, const char *key, const char *to) {
        if (from && from[key]) {
            if (!target[to])
                target[to] = YAML::Clone(from[key]);
            from.remove(key);
        }
    };
    if (behaviour) {
        if (behaviour["copy_on_select_linux"] || behaviour["copy_on_select_windows"]) {
            if (!target["copy_on_select"] && (readBool(behaviour["copy_on_select_linux"], false)
                                              || readBool(behaviour["copy_on_select_windows"], false)))
                target["copy_on_select"] = true;
            behaviour.remove("copy_on_select_linux");
            behaviour.remove("copy_on_select_windows");
        }
        if (behaviour["middle_click"]) {
            std::string middle;
            try {
                middle = behaviour["middle_click"].as<std::string>();
            } catch (const YAML::Exception &) {
            }
            if (middle == "clipboard") {
                if (!target["middle_click"])
                    target["middle_click"] = "paste";
                if (!target["paste"])
                    target["paste"] = "clipboard";
                behaviour.remove("middle_click");
            }
        }
        for (const auto *key : {"right_click", "middle_click", "confirm_multiline_paste",
                                "edit_multiline_paste", "trim_pasted_newlines", "word_characters", "links"})
            move(behaviour, key, key);
    }
    move(startup, "middle_click_closes", "middle_click_closes_tab");
    move(startup, "wheel_on_fields", "wheel_on_fields");
    settings["mouse_clipboard"] = target;
}

// In order; the last one's version is CurrentSettingsVersion.
const Step steps[] = {
    {1, "behaviour.copy_on_select split into copy_on_select_linux and copy_on_select_windows",
     splitCopyOnSelect},
    {2, "commands.confirm_multiline merged into behaviour.confirm_multiline_paste",
     mergeMultilineConfirmation},
    {3, "left-over keys removed (summary_in_column, show_host_in_list, command_bar)",
     removeLeftovers},
    {4, "behaviour.links_enabled and ctrl_click_opens_links merged into behaviour.links",
     mergeLinkSettings},
    {5, "copy, paste, mouse buttons, links and the mouse wheel moved to mouse_clipboard",
     moveMouseClipboard},
};
static_assert(sizeof steps / sizeof *steps == CurrentSettingsVersion,
              "one step per settings version");

int versionOf(const YAML::Node &settings)
{
    try {
        if (const auto node = settings["settings_version"])
            return node.as<int>();
    } catch (const YAML::Exception &) {
    }
    return 0;
}

} // namespace

int migrateSettingsDocument(YAML::Node &settings, QStringList *done)
{
    const int from = versionOf(settings);
    if (from >= CurrentSettingsVersion)
        return from;
    for (const auto &step : steps) {
        if (step.version <= from)
            continue;
        step.apply(settings);
        if (done)
            done->append(QStringLiteral("%1: %2").arg(step.version).arg(QString::fromUtf8(step.what)));
    }
    settings["settings_version"] = CurrentSettingsVersion;
    return from;
}

QStringList migrateSettings()
{
    QStringList report;
    const auto path = portableFilePath(QStringLiteral("settings/settings.yaml"));
    if (!QFile::exists(path))
        return report; // a first start: saving writes the current version
    const SettingsFileLock lock(path); // from reading to writing
    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &error) {
        report << QStringLiteral("settings.yaml cannot be read (%1); not migrated").arg(QString::fromUtf8(error.what()));
        return report;
    }
    if (!settings.IsMap())
        return report;
    const int version = versionOf(settings);
    if (version > CurrentSettingsVersion) {
        report << QStringLiteral("settings.yaml is version %1, from a newer OpsRemote (this one knows %2); "
                                 "left as it is")
                      .arg(version)
                      .arg(CurrentSettingsVersion);
        return report;
    }
    if (version == CurrentSettingsVersion)
        return report;

    // A copy as it was, to go back to (Preferences → Data and backup).
    QString error;
    const auto backup = createBackup(&error);
    report << (backup.isEmpty() ? QStringLiteral("no backup before migrating: %1").arg(error)
                                : QStringLiteral("backed up to %1").arg(backup));
    QStringList done;
    migrateSettingsDocument(settings, &done);
    for (const auto &line : done)
        report << QStringLiteral("step %1").arg(line);

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
        report << QStringLiteral("could not write settings.yaml; the steps run again next time");
        return report;
    }
    report << QStringLiteral("settings.yaml is now version %1 (was %2)").arg(CurrentSettingsVersion).arg(version);
    return report;
}
