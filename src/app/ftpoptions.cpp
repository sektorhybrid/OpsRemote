// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "ftpoptions.h"
#include "appdirs.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QSaveFile>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

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

FtpOptions FtpOptions::overriddenBy(const FtpOptions &session) const
{
    FtpOptions result = *this;
    if (!session.security.isEmpty())
        result.security = session.security;
    if (!session.mode.isEmpty())
        result.mode = session.mode;
    if (!session.listing.isEmpty())
        result.listing = session.listing;
    if (session.connectTimeout > 0)
        result.connectTimeout = session.connectTimeout;
    if (session.keepaliveSeconds >= 0)
        result.keepaliveSeconds = session.keepaliveSeconds;
    return result;
}

FtpOptions FtpOptions::builtIn()
{
    FtpOptions options;
    options.security = QStringLiteral("auto");
    options.mode = QStringLiteral("passive");
    options.listing = QStringLiteral("auto");
    options.connectTimeout = 15;
    options.keepaliveSeconds = 60;
    return options;
}

RdpChoices ftpSecurityChoices()
{
    return {{QStringLiteral("auto"), QStringLiteral("Use TLS when the server offers it")},
            {QStringLiteral("explicit"), QStringLiteral("Require TLS (explicit FTPS)")},
            {QStringLiteral("implicit"), QStringLiteral("Implicit FTPS (TLS from the start, port 990)")},
            {QStringLiteral("none"), QStringLiteral("Plain FTP (not encrypted)")}};
}

RdpChoices ftpModeChoices()
{
    return {{QStringLiteral("passive"), QStringLiteral("Passive (works through most firewalls)")},
            {QStringLiteral("active"), QStringLiteral("Active (the server connects back)")}};
}

RdpChoices ftpListingChoices()
{
    return {{QStringLiteral("auto"), QStringLiteral("Machine-readable (MLSD) when offered")},
            {QStringLiteral("list"), QStringLiteral("Always LIST")}};
}

FtpOptions ftpOptionsFromYaml(const YAML::Node &node)
{
    FtpOptions options;
    if (!node || !node.IsMap())
        return options;
    try {
        const auto readChoice = [&node](const char *key, const RdpChoices &choices, QString &target) {
            if (!node[key])
                return;
            const auto value = QString::fromStdString(node[key].as<std::string>());
            for (const auto &choice : choices) {
                if (choice.first == value)
                    target = value;
            }
        };
        readChoice("security", ftpSecurityChoices(), options.security);
        readChoice("mode", ftpModeChoices(), options.mode);
        readChoice("listing", ftpListingChoices(), options.listing);
        if (node["connect_timeout"])
            options.connectTimeout = qBound(1, node["connect_timeout"].as<int>(), 600);
        if (node["keepalive_seconds"])
            options.keepaliveSeconds = qBound(0, node["keepalive_seconds"].as<int>(), 3600);
    } catch (const YAML::Exception &) {
        // Keep what was read; invalid values use the defaults.
    }
    return options;
}

void ftpOptionsToYaml(const FtpOptions &options, YAML::Node &node)
{
    const auto text = [&node](const char *key, const QString &value) {
        if (!value.isEmpty())
            node[key] = value.toStdString();
    };
    text("security", options.security);
    text("mode", options.mode);
    text("listing", options.listing);
    if (options.connectTimeout > 0)
        node["connect_timeout"] = options.connectTimeout;
    if (options.keepaliveSeconds >= 0)
        node["keepalive_seconds"] = options.keepaliveSeconds;
}

FtpOptions loadFtpDefaults()
{
    try {
        return FtpOptions::builtIn().overriddenBy(
            ftpOptionsFromYaml(YAML::LoadFile(settingsPath().toStdString())["ftp"]));
    } catch (const YAML::Exception &) {
        return FtpOptions::builtIn();
    }
}

void saveFtpDefaults(const FtpOptions &options)
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
    YAML::Node node(YAML::NodeType::Map);
    ftpOptionsToYaml(options, node);
    settings["ftp"] = node;
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

FtpOptionsEditor::FtpOptionsEditor(const FtpOptions &value, const FtpOptions &defaults,
                                   QWidget *parent)
    : QWidget(parent)
{
    const FtpOptions fallback = FtpOptions::builtIn().overriddenBy(defaults);
    const auto makeBox = [this](const RdpChoices &choices, const QString &current,
                                const QString &defaultValue) {
        auto *box = new QComboBox(this);
        box->addItem(QStringLiteral("Default (%1)").arg(labelFor(choices, defaultValue)), QString());
        for (const auto &choice : choices)
            box->addItem(choice.second, choice.first);
        box->setCurrentIndex(qMax(0, box->findData(current)));
        return box;
    };
    const auto makeNumber = [this](int own, int low, int high, const QString &defaultText) {
        auto *box = new QSpinBox(this);
        box->setRange(low - 1, high);
        box->setSpecialValueText(QStringLiteral("Default (%1)").arg(defaultText));
        box->setSuffix(QStringLiteral(" s"));
        box->setValue(own >= low ? own : low - 1);
        return box;
    };
    m_security = makeBox(ftpSecurityChoices(), value.security, fallback.security);
    m_security->setToolTip(QStringLiteral("Plain FTP sends the password and the files unencrypted."));
    m_mode = makeBox(ftpModeChoices(), value.mode, fallback.mode);
    m_mode->setToolTip(QStringLiteral("Through a proxy, passive mode is always used."));
    m_listing = makeBox(ftpListingChoices(), value.listing, fallback.listing);
    m_listing->setToolTip(QStringLiteral("LIST works with every server; MLSD gives exact sizes and "
                                         "times. Choose LIST for servers whose MLSD is broken."));
    m_connectTimeout = makeNumber(value.connectTimeout, 1, 600,
                                  QStringLiteral("%1 s").arg(fallback.connectTimeout));
    m_keepalive = makeNumber(value.keepaliveSeconds, 0, 3600,
                             fallback.keepaliveSeconds > 0
                                 ? QStringLiteral("%1 s").arg(fallback.keepaliveSeconds)
                                 : QStringLiteral("off"));
    m_keepalive->setToolTip(QStringLiteral("Sends NOOP while idle, so the server does not close the "
                                           "connection. 0 turns it off."));

    // Tabs, like the RDP and VNC settings, so the session window's shared
    // tabs (Proxy, Wake-on-LAN) can join them.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("ftpTabs"));
    layout->addWidget(tabs);
    auto *page = new QWidget(tabs);
    tabs->addTab(page, QStringLiteral("Connection"));
    auto *form = new QFormLayout(page);
    form->addRow(QStringLiteral("Security:"), m_security);
    form->addRow(QStringLiteral("Data connections:"), m_mode);
    form->addRow(QStringLiteral("Folder listings:"), m_listing);
    form->addRow(QStringLiteral("Connect timeout:"), m_connectTimeout);
    form->addRow(QStringLiteral("Keepalive every:"), m_keepalive);
}

FtpOptions FtpOptionsEditor::value() const
{
    FtpOptions options;
    options.security = m_security->currentData().toString();
    options.mode = m_mode->currentData().toString();
    options.listing = m_listing->currentData().toString();
    const auto number = [](const QSpinBox *box) {
        return box->value() == box->minimum() ? -1 : box->value();
    };
    options.connectTimeout = number(m_connectTimeout);
    options.keepaliveSeconds = number(m_keepalive);
    return options;
}
