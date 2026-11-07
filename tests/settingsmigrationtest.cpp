// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
//
// Settings files from earlier versions brought to the current shape. Each
// version has one file, tests/settings-migration/<version>.yaml, holding its
// cases by name; each case's expectations are below. Upgrades are tested for
// the last 10 releases: an older version's file and checks are removed
// together (its migration steps stay in the program). Run with
// ctest, or: settings-migration-test <tests/settings-migration folder>.

#include "settingsmigrations.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <functional>

// migrateSettings() needs these from the program; here they point at a
// temporary copy of a case.
static QString testHome;
QString portableFilePath(const QString &name)
{
    return QDir(testHome).filePath(name);
}
QString createBackup(QString *)
{
    const auto target = QDir(testHome).filePath(QStringLiteral("backups/test"));
    QDir().mkpath(target);
    QFile::copy(portableFilePath(QStringLiteral("settings/settings.yaml")),
                QDir(target).filePath(QStringLiteral("settings.yaml")));
    return target;
}

static int failures = 0;

static void check(const char *testCase, const char *what, bool ok)
{
    std::printf("%s %s: %s\n", ok ? "ok  " : "FAIL", testCase, what);
    if (!ok)
        ++failures;
}

template <typename T>
static bool is(const YAML::Node &node, const T &value)
{
    try {
        return node && node.as<T>() == value;
    } catch (const YAML::Exception &) {
        return false;
    }
}

// Migrates a case (a settings.yaml) of a version's file through
// migrateSettings() (as at start), then checks the file it wrote.
static void run(const QString &cases, const char *file, const char *caseName,
                const std::function<void(const YAML::Node &, const QStringList &)> &expect)
{
    const auto label = QStringLiteral("%1/%2").arg(QString::fromUtf8(file).chopped(5), QString::fromUtf8(caseName));
    const auto nameBytes = label.toUtf8();
    const char *name = nameBytes.constData();
    QTemporaryDir home;
    testHome = home.path();
    QDir().mkpath(QDir(testHome).filePath(QStringLiteral("settings")));
    const auto source = QDir(cases).filePath(QString::fromUtf8(file));
    const auto target = portableFilePath(QStringLiteral("settings/settings.yaml"));
    YAML::Node sample;
    try {
        sample = YAML::LoadFile(source.toStdString())[caseName];
    } catch (const YAML::Exception &) {
    }
    if (!sample || !sample.IsMap()) {
        check(name, "case found in its version's file", false);
        return;
    }
    YAML::Emitter emitter;
    emitter << sample;
    // "settings_version: current" stands for this build's version.
    QByteArray text(emitter.c_str());
    text.replace("settings_version: current", "settings_version: " + QByteArray::number(CurrentSettingsVersion));
    QFile copy(target);
    if (!copy.open(QIODevice::WriteOnly) || copy.write(text) != text.size()) {
        check(name, "sample file copied", false);
        return;
    }
    copy.close();
    const auto report = migrateSettings();
    for (const auto &line : report)
        std::printf("     | %s\n", qUtf8Printable(line));
    expect(YAML::LoadFile(target.toStdString()), report);
    // Running again changes nothing (a second start).
    QFile before(target);
    const auto written = before.open(QIODevice::ReadOnly) ? before.readAll() : QByteArray();
    before.close();
    migrateSettings();
    QFile after(target);
    check(name, "a second start changes nothing",
          after.open(QIODevice::ReadOnly) && after.readAll() == written);
}

int main(int argc, char **argv)
{
    const QString cases = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("tests/settings-migration");

    run(cases, "0.6.1.yaml", "default", [](const YAML::Node &s, const QStringList &report) {
        check("0.6.1/default", "now the current version", is(s["settings_version"], CurrentSettingsVersion));
        check("0.6.1/default", "old copy_on_select removed", !s["behaviour"]["copy_on_select"]);
        check("0.6.1/default", "copy on select not written: the default (on) applies",
              !s["behaviour"]["copy_on_select_linux"] && !s["behaviour"]["copy_on_select_windows"]
                  && !s["mouse_clipboard"]["copy_on_select"]);
        check("0.6.1/default", "links: both were on, now ctrl-click",
              is(s["mouse_clipboard"]["links"], std::string("ctrl-click")) && !s["behaviour"]["links"]
                  && !s["behaviour"]["links_enabled"] && !s["behaviour"]["ctrl_click_opens_links"]);
        check("0.6.1/default", "asking before several lines stays as it was",
              is(s["mouse_clipboard"]["confirm_multiline_paste"], true) && !s["behaviour"]["confirm_multiline_paste"]);
        check("0.6.1/default", "other settings kept",
              is(s["behaviour"]["scrollback_lines"], 1000000) && is(s["startup"]["single_instance"], true));
        check("0.6.1/default", "backed up first", report.join(QLatin1Char('\n')).contains(QStringLiteral("backed up")));
    });
    run(cases, "0.6.1.yaml", "copy-on-select", [](const YAML::Node &s, const QStringList &) {
        check("0.6.1/copy-on-select", "turned on by hand: stays on",
              is(s["mouse_clipboard"]["copy_on_select"], true) && !s["behaviour"]["copy_on_select_linux"]
                  && !s["behaviour"]["copy_on_select_windows"]);
        check("0.6.1/copy-on-select", "old copy_on_select removed", !s["behaviour"]["copy_on_select"]);
    });
    run(cases, "0.6.1.yaml", "leftovers", [](const YAML::Node &s, const QStringList &) {
        check("0.6.1/leftovers", "startup.show_host_in_list removed", !s["startup"]["show_host_in_list"]);
        check("0.6.1/leftovers", "command_bar removed", !s["command_bar"]);
        check("0.6.1/leftovers", "summary_in_column became folder_summary: column",
              !s["session_list"]["summary_in_column"] && is(s["session_list"]["folder_summary"], std::string("column")));
        check("0.6.1/leftovers", "show_host and single_instance kept",
              is(s["session_list"]["show_host"], true) && is(s["startup"]["single_instance"], true));
        check("0.6.1/leftovers", "asking before several lines stays on", is(s["mouse_clipboard"]["confirm_multiline_paste"], true));
    });
    run(cases, "0.6.1.yaml", "multiline-both-off", [](const YAML::Node &s, const QStringList &) {
        check("0.6.1/multiline-both-off", "both off: stays off", is(s["mouse_clipboard"]["confirm_multiline_paste"], false));
        check("0.6.1/multiline-both-off", "commands.confirm_multiline removed, other commands keys kept",
              !s["commands"]["confirm_multiline"] && is(s["commands"]["show_files"], true));
    });
    run(cases, "0.6.1.yaml", "multiline-one-off", [](const YAML::Node &s, const QStringList &) {
        check("0.6.1/multiline-one-off", "one still on: asking stays on", is(s["mouse_clipboard"]["confirm_multiline_paste"], true));
        check("0.6.1/multiline-one-off", "commands.confirm_multiline removed", !s["commands"]["confirm_multiline"]);
    });
    run(cases, "0.6.1.yaml", "links-off", [](const YAML::Node &s, const QStringList &) {
        check("0.6.1/links-off", "links off stay off", is(s["mouse_clipboard"]["links"], std::string("off")));
    });
    run(cases, "0.6.1.yaml", "links-menu", [](const YAML::Node &s, const QStringList &) {
        check("0.6.1/links-menu", "Ctrl+click off: menu only", is(s["mouse_clipboard"]["links"], std::string("menu")));
        check("0.6.1/links-menu", "old keys removed",
              !s["behaviour"]["links_enabled"] && !s["behaviour"]["ctrl_click_opens_links"]);
    });
    run(cases, "current-and-newer.yaml", "current", [](const YAML::Node &s, const QStringList &report) {
        check("current-and-newer/current", "nothing done", report.isEmpty());
        check("current-and-newer/current", "values unchanged",
              is(s["mouse_clipboard"]["copy_on_select"], false) && is(s["mouse_clipboard"]["paste"], std::string("clipboard")));
    });
    run(cases, "0.6.5-dev.yaml", "mouse", [](const YAML::Node &s, const QStringList &) {
        const auto m = s["mouse_clipboard"];
        check("0.6.5-dev/mouse", "copy on select on for one system: on",
              is(m["copy_on_select"], true) && !s["behaviour"]["copy_on_select_linux"]);
        check("0.6.5-dev/mouse", "middle-click 'clipboard': paste, with the paste rule 'clipboard'",
              is(m["middle_click"], std::string("paste")) && is(m["paste"], std::string("clipboard")));
        check("0.6.5-dev/mouse", "values set by hand moved unchanged",
              is(m["right_click"], std::string("paste")) && is(m["word_characters"], std::string("-_."))
                  && is(m["trim_pasted_newlines"], true) && is(m["edit_multiline_paste"], true));
        check("0.6.5-dev/mouse", "tab and wheel settings moved from startup",
              is(m["middle_click_closes_tab"], false) && is(m["wheel_on_fields"], std::string("never"))
                  && !s["startup"]["middle_click_closes"] && !s["startup"]["wheel_on_fields"]);
        check("0.6.5-dev/mouse", "other settings kept",
              is(s["behaviour"]["bell"], std::string("sound")) && is(s["startup"]["tab_title"], std::string("both")));
    });
    run(cases, "current-and-newer.yaml", "newer", [](const YAML::Node &s, const QStringList &report) {
        check("current-and-newer/newer", "left as it is", is(s["settings_version"], 99) && is(s["behaviour"]["copy_on_select"], true)
                                            && is(s["behaviour"]["something_new"], 1));
        check("current-and-newer/newer", "says why", report.join(QLatin1Char('\n')).contains(QStringLiteral("newer OpsRemote")));
    });

    // Another OpsRemote's lock (the other system on a shared folder): a fresh
    // one is waited for (3 s), then the change is saved anyway; one older
    // than 10 s, left by a program that crashed, is taken over at once.
    for (const bool stale : {false, true}) {
        const char *name = stale ? "lock-stale" : "lock-fresh";
        QTemporaryDir home;
        testHome = home.path();
        QDir().mkpath(QDir(testHome).filePath(QStringLiteral("settings")));
        const auto target = portableFilePath(QStringLiteral("settings/settings.yaml"));
        QFile old(target);
        if (!old.open(QIODevice::WriteOnly) || old.write("behaviour:\n  copy_on_select: true\n") <= 0) {
            check(name, "sample file written", false);
            continue;
        }
        old.close();
        QFile lock(target + QStringLiteral(".lock"));
        if (!lock.open(QIODevice::WriteOnly) || lock.write("99999\nopsremote\nanother-computer\n") <= 0) {
            check(name, "lock file written", false);
            continue;
        }
        lock.close();
        if (stale) {
            const auto past = QDateTime::currentDateTime().addSecs(-60);
            if (lock.open(QIODevice::ReadWrite)) {
                lock.setFileTime(past, QFileDevice::FileModificationTime);
                lock.close();
            }
        }
        QElapsedTimer timer;
        timer.start();
        migrateSettings();
        const auto waited = timer.elapsed();
        std::printf("     | waited %lld ms\n", static_cast<long long>(waited));
        check(name, stale ? "taken over at once" : "waited for the other program (about 3 s)",
              stale ? waited < 1000 : waited >= 2900);
        check(name, "the change is saved anyway",
              is(YAML::LoadFile(target.toStdString())["settings_version"], CurrentSettingsVersion));
    }

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
