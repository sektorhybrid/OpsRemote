// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "rshoptions.h"
#include "appdirs.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSaveFile>

#include <yaml-cpp/yaml.h>

namespace {
QString settingsPath()
{
    return portableFilePath(QStringLiteral("settings/settings.yaml"));
}

QString labelFor(const RdpChoices &choices, const QString &value)
{
    for (const auto &choice : choices) {
        if (choice.first == value)
            return choice.second;
    }
    return value;
}
}

RshOptions RshOptions::overriddenBy(const RshOptions &session) const
{
    RshOptions result = *this;
    if (!session.localUser.isEmpty())
        result.localUser = session.localUser;
    if (!session.sourcePort.isEmpty())
        result.sourcePort = session.sourcePort;
    if (!session.command.isEmpty())
        result.command = session.command;
    return result;
}

RshOptions RshOptions::builtIn()
{
    RshOptions options;
    options.sourcePort = QStringLiteral("auto");
    return options;
}

RdpChoices rshSourcePortChoices()
{
    return {{QStringLiteral("auto"), QStringLiteral("Reserved port when allowed, else any")},
            {QStringLiteral("reserved"), QStringLiteral("Reserved port only (below 1024)")},
            {QStringLiteral("any"), QStringLiteral("Any port")}};
}

QString currentUserName()
{
    QString name = qEnvironmentVariable("USER");
    if (name.isEmpty())
        name = qEnvironmentVariable("USERNAME");
    if (name.isEmpty())
        name = qEnvironmentVariable("LOGNAME");
    return name;
}

RshOptions rshOptionsFromYaml(const YAML::Node &node)
{
    RshOptions options;
    if (!node || !node.IsMap())
        return options;
    try {
        if (node["local_user"])
            options.localUser = QString::fromStdString(node["local_user"].as<std::string>()).trimmed();
        if (node["source_port"]) {
            const auto value = QString::fromStdString(node["source_port"].as<std::string>());
            for (const auto &choice : rshSourcePortChoices()) {
                if (choice.first == value)
                    options.sourcePort = value;
            }
        }
        if (node["command"])
            options.command = QString::fromStdString(node["command"].as<std::string>());
    } catch (const YAML::Exception &) {
        // Keep what was read; invalid values use the defaults.
    }
    return options;
}

void rshOptionsToYaml(const RshOptions &options, YAML::Node &node)
{
    const auto text = [&node](const char *key, const QString &value) {
        if (!value.isEmpty())
            node[key] = value.toStdString();
    };
    text("local_user", options.localUser);
    text("source_port", options.sourcePort);
    text("command", options.command);
}

RshOptions loadRshDefaults()
{
    try {
        auto defaults = rshOptionsFromYaml(YAML::LoadFile(settingsPath().toStdString())["rlogin"]);
        defaults.command.clear(); // a session's own
        return RshOptions::builtIn().overriddenBy(defaults);
    } catch (const YAML::Exception &) {
        return RshOptions::builtIn();
    }
}

void saveRshDefaults(const RshOptions &options)
{
    const auto path = settingsPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;
    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
    }
    if (!settings.IsMap())
        settings = YAML::Node(YAML::NodeType::Map);
    auto defaults = options;
    defaults.command.clear();
    YAML::Node node(YAML::NodeType::Map);
    rshOptionsToYaml(defaults, node);
    settings["rlogin"] = node;
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

RshOptionsEditor::RshOptionsEditor(const RshOptions &value, const RshOptions &defaults, QWidget *parent)
    : QWidget(parent)
{
    const RshOptions fallback = RshOptions::builtIn().overriddenBy(defaults);
    m_localUser = new QLineEdit(value.localUser, this);
    m_localUser->setPlaceholderText(QStringLiteral("Default (%1)").arg(
        fallback.localUser.isEmpty() ? currentUserName() : fallback.localUser));
    m_localUser->setToolTip(QStringLiteral(
        "Sent as your user name on this computer. The server lets you in without a password when "
        "its .rhosts or hosts.equiv trusts this name from this computer; otherwise it asks."));
    m_sourcePort = new QComboBox(this);
    m_sourcePort->addItem(QStringLiteral("Default (%1)").arg(labelFor(rshSourcePortChoices(), fallback.sourcePort)),
                          QString());
    for (const auto &choice : rshSourcePortChoices())
        m_sourcePort->addItem(choice.second, choice.first);
    m_sourcePort->setCurrentIndex(qMax(0, m_sourcePort->findData(value.sourcePort)));
    m_sourcePort->setToolTip(QStringLiteral(
        "Most rlogin and rsh servers accept only connections from a reserved port (below 1024), "
        "which only an administrator may use on Linux and macOS. Windows allows it to everyone."));
    m_command = new QLineEdit(value.command, this);
    m_command->setPlaceholderText(QStringLiteral("uname -a"));
    m_command->setToolTip(QStringLiteral(
        "Runs on the server; its output shows in the tab, and what you type goes to it line by line."));

    // A page of the session window's terminal tabs ("Connection").
    auto *page = this;
    auto *form = new QFormLayout(page);
    m_commandLabel = new QLabel(QStringLiteral("Command:"), page);
    form->addRow(m_commandLabel, m_command);
    form->addRow(QStringLiteral("Local user name:"), m_localUser);
    form->addRow(QStringLiteral("Source port:"), m_sourcePort);
    auto *note = new QLabel(QStringLiteral(
        "Rlogin and RSH send everything, passwords included, unencrypted. Use them only on networks "
        "you trust, for equipment that has nothing better."), page);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(note);
}

void RshOptionsEditor::setCommandShown(bool shown)
{
    m_commandShown = shown;
    m_command->setVisible(shown);
    m_commandLabel->setVisible(shown);
}

RshOptions RshOptionsEditor::value() const
{
    RshOptions options;
    options.localUser = m_localUser->text().trimmed();
    options.sourcePort = m_sourcePort->currentData().toString();
    if (m_commandShown)
        options.command = m_command->text().trimmed();
    return options;
}
