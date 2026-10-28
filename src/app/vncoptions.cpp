// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "vncoptions.h"
#include "appdirs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
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

bool VncOptions::isEmpty() const
{
    return *this == VncOptions();
}

VncOptions VncOptions::overriddenBy(const VncOptions &session) const
{
    VncOptions result = *this;
    const auto text = [](QString &target, const QString &value) {
        if (!value.isEmpty())
            target = value;
    };
    const auto number = [](int &target, int value) {
        if (value >= 0)
            target = value;
    };
    text(result.resolution, session.resolution);
    if (session.colorDepth > 0)
        result.colorDepth = session.colorDepth;
    text(result.quality, session.quality);
    text(result.encryption, session.encryption);
    number(result.remoteCursor, session.remoteCursor);
    number(result.clipboard, session.clipboard);
    number(result.autoReconnect, session.autoReconnect);
    number(result.highDpi, session.highDpi);
    number(result.allMonitors, session.allMonitors);
    text(result.sshTunnel, session.sshTunnel);
    // Session only.
    result.listen = session.listen;
    result.listenPort = session.listenPort;
    result.repeater = session.repeater;
    result.repeaterId = session.repeaterId;
    return result;
}

VncOptions VncOptions::builtIn()
{
    VncOptions options;
    options.resolution = QStringLiteral("follow");
    options.colorDepth = 32;
    options.quality = QStringLiteral("good");
    options.encryption = QStringLiteral("prefer");
    options.remoteCursor = 1;
    options.clipboard = 1;
    options.autoReconnect = 1;
    options.highDpi = 1;
    options.allMonitors = 0;
    options.sshTunnel = QStringLiteral("none");
    return options;
}

RdpChoices vncResolutionChoices()
{
    return {{QStringLiteral("follow"), QStringLiteral("Follow the tab size")},
            {QStringLiteral("fit"), QStringLiteral("Server's size, scaled to fit")}};
}

RdpChoices vncColorDepthChoices()
{
    return {{QStringLiteral("32"), QStringLiteral("True colour (24-bit)")},
            {QStringLiteral("16"), QStringLiteral("High colour (16-bit)")},
            {QStringLiteral("8"), QStringLiteral("256 colours (8-bit)")}};
}

RdpChoices vncQualityChoices()
{
    return {{QStringLiteral("best"), QStringLiteral("Best (lossless, for fast networks)")},
            {QStringLiteral("good"), QStringLiteral("Good")},
            {QStringLiteral("medium"), QStringLiteral("Medium")},
            {QStringLiteral("poor"), QStringLiteral("Poor (for slow links)")}};
}

RdpChoices vncEncryptionChoices()
{
    return {{QStringLiteral("prefer"), QStringLiteral("Use TLS when the server offers it")},
            {QStringLiteral("require"), QStringLiteral("Require TLS (VeNCrypt or TLS)")}};
}

VncOptions vncOptionsFromYaml(const YAML::Node &node)
{
    VncOptions options;
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
        const auto readSwitch = [&node](const char *key, int &target) {
            if (node[key])
                target = node[key].as<bool>() ? 1 : 0;
        };
        const auto readText = [&node](const char *key, QString &target) {
            if (node[key])
                target = QString::fromStdString(node[key].as<std::string>()).trimmed().left(255);
        };
        readChoice("resolution", vncResolutionChoices(), options.resolution);
        if (node["color_depth"]) {
            const int depth = node["color_depth"].as<int>();
            if (depth == 32 || depth == 16 || depth == 8)
                options.colorDepth = depth;
        }
        readChoice("quality", vncQualityChoices(), options.quality);
        readChoice("encryption", vncEncryptionChoices(), options.encryption);
        readSwitch("remote_cursor", options.remoteCursor);
        readSwitch("clipboard", options.clipboard);
        readSwitch("auto_reconnect", options.autoReconnect);
        readSwitch("high_dpi", options.highDpi);
        readSwitch("all_monitors", options.allMonitors);
        readText("ssh_tunnel", options.sshTunnel);
        readSwitch("listen", options.listen);
        if (node["listen_port"])
            options.listenPort = qBound(0, node["listen_port"].as<int>(), 65535);
        readText("repeater", options.repeater);
        readText("repeater_id", options.repeaterId);
    } catch (const YAML::Exception &) {
        // Keep what was read; invalid values use the defaults.
    }
    return options;
}

void vncOptionsToYaml(const VncOptions &options, YAML::Node &node)
{
    const auto text = [&node](const char *key, const QString &value) {
        if (!value.isEmpty())
            node[key] = value.toStdString();
    };
    const auto flag = [&node](const char *key, int value) {
        if (value >= 0)
            node[key] = value == 1;
    };
    text("resolution", options.resolution);
    if (options.colorDepth > 0)
        node["color_depth"] = options.colorDepth;
    text("quality", options.quality);
    text("encryption", options.encryption);
    flag("remote_cursor", options.remoteCursor);
    flag("clipboard", options.clipboard);
    flag("auto_reconnect", options.autoReconnect);
    flag("high_dpi", options.highDpi);
    flag("all_monitors", options.allMonitors);
    text("ssh_tunnel", options.sshTunnel);
    flag("listen", options.listen);
    if (options.listenPort > 0)
        node["listen_port"] = options.listenPort;
    text("repeater", options.repeater);
    text("repeater_id", options.repeaterId);
}

VncOptions loadVncDefaults()
{
    try {
        auto defaults = VncOptions::builtIn().overriddenBy(
            vncOptionsFromYaml(YAML::LoadFile(settingsPath().toStdString())["vnc"]));
        return defaults;
    } catch (const YAML::Exception &) {
        return VncOptions::builtIn();
    }
}

void saveVncDefaults(const VncOptions &options)
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
    // Defaults never include the session-only fields.
    VncOptions defaults = options;
    defaults.listen = -1;
    defaults.listenPort = 0;
    defaults.repeater.clear();
    defaults.repeaterId.clear();
    YAML::Node node(YAML::NodeType::Map);
    vncOptionsToYaml(defaults, node);
    settings["vnc"] = node;
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

VncOptionsEditor::VncOptionsEditor(const VncOptions &value, const VncOptions &defaults,
                                   const QList<QPair<QString, QString>> &sshSessions,
                                   QWidget *parent)
    : QWidget(parent), m_defaults(VncOptions::builtIn().overriddenBy(defaults))
{
    const auto makeBox = [this](const RdpChoices &choices, const QString &current,
                                const QString &defaultValue) {
        auto *box = new QComboBox(this);
        box->addItem(QStringLiteral("Default (%1)").arg(labelFor(choices, defaultValue)), QString());
        for (const auto &choice : choices)
            box->addItem(choice.second, choice.first);
        box->setCurrentIndex(qMax(0, box->findData(current)));
        connect(box, &QComboBox::currentIndexChanged, this, &VncOptionsEditor::changed);
        return box;
    };
    const auto makeSwitch = [this](const QString &text, const QString &tip, int own, int fallback) {
        auto *box = new QCheckBox(text, this);
        box->setToolTip(tip);
        box->setChecked((own >= 0 ? own : fallback) == 1);
        connect(box, &QCheckBox::toggled, this, &VncOptionsEditor::changed);
        return box;
    };
    const auto makeText = [this](const QString &current, const QString &placeholder,
                                 const QString &tip) {
        auto *edit = new QLineEdit(current, this);
        edit->setPlaceholderText(placeholder);
        edit->setToolTip(tip);
        edit->setMaxLength(255);
        connect(edit, &QLineEdit::textChanged, this, &VncOptionsEditor::changed);
        return edit;
    };

    m_resolution = makeBox(vncResolutionChoices(), value.resolution, m_defaults.resolution);
    m_colorDepth = makeBox(vncColorDepthChoices(),
                           value.colorDepth > 0 ? QString::number(value.colorDepth) : QString(),
                           QString::number(m_defaults.colorDepth));
    m_quality = makeBox(vncQualityChoices(), value.quality, m_defaults.quality);
    m_quality->setToolTip(QStringLiteral("Picks the encodings and the JPEG and compression levels."));
    m_highDpi = makeSwitch(QStringLiteral("Sharp on high-DPI screens"),
                           QStringLiteral("One remote pixel per physical screen pixel, instead of "
                                          "an enlarged, blurry desktop."),
                           value.highDpi, m_defaults.highDpi);
    m_allMonitors = makeSwitch(QStringLiteral("Full screen uses all monitors"),
                               QStringLiteral("In full screen (Ctrl+Alt+Enter), the window spans "
                                              "every monitor (not on Wayland)."),
                               value.allMonitors, m_defaults.allMonitors);
    m_remoteCursor = makeSwitch(QStringLiteral("Show the remote pointer shape"),
                                QStringLiteral("Draws the server's pointer locally, so it moves "
                                               "without delay. Turn off for servers that also "
                                               "draw it into the picture (two pointers)."),
                                value.remoteCursor, m_defaults.remoteCursor);
    m_clipboard = makeSwitch(QStringLiteral("Share clipboard text"),
                             QStringLiteral("Copy text here and paste it on the remote computer, "
                                            "and the other way round."),
                             value.clipboard, m_defaults.clipboard);
    m_encryption = makeBox(vncEncryptionChoices(), value.encryption, m_defaults.encryption);
    m_encryption->setToolTip(QStringLiteral("Plain VNC is not encrypted; an SSH tunnel encrypts "
                                            "any server."));
    m_autoReconnect = makeSwitch(QStringLiteral("Reconnect automatically if the network drops"),
                                 QStringLiteral("Tries again for about half a minute."),
                                 value.autoReconnect, m_defaults.autoReconnect);

    m_sshTunnel = new QComboBox(this);
    m_sshTunnel->setEditable(true);
    m_sshTunnel->setToolTip(QStringLiteral("Connect through an SSH server: one of your saved SSH "
                                           "sessions, or user@host[:port]. The VNC host is then "
                                           "as seen from that server (often localhost)."));
    const QString defaultTunnel = m_defaults.sshTunnel == QStringLiteral("none")
                                      ? QStringLiteral("none")
                                      : m_defaults.sshTunnel;
    m_sshTunnel->addItem(QStringLiteral("Default (%1)").arg(defaultTunnel), QString());
    m_sshTunnel->addItem(QStringLiteral("None (connect directly)"), QStringLiteral("none"));
    for (const auto &[id, label] : sshSessions)
        m_sshTunnel->addItem(label, QStringLiteral("session:") + id);
    const int known = m_sshTunnel->findData(value.sshTunnel);
    if (known >= 0)
        m_sshTunnel->setCurrentIndex(known);
    else
        m_sshTunnel->setEditText(value.sshTunnel);
    connect(m_sshTunnel, &QComboBox::currentTextChanged, this, &VncOptionsEditor::changed);

    m_listen = makeSwitch(QStringLiteral("Wait for the server to connect (listen mode)"),
                          QStringLiteral("Reverse VNC: the remote computer connects to this one, "
                                         "for example from behind a firewall. The host above is "
                                         "not used."),
                          value.listen, 0);
    m_listenPort = new QSpinBox(this);
    m_listenPort->setRange(1, 65535);
    m_listenPort->setValue(value.listenPort > 0 ? value.listenPort : 5500);
    m_listenPort->setEnabled(m_listen->isChecked());
    connect(m_listen, &QCheckBox::toggled, m_listenPort, &QWidget::setEnabled);
    m_repeater = makeText(value.repeater, QStringLiteral("None"),
                          QStringLiteral("An UltraVNC repeater, as host or host:port (5901 if not "
                                         "given)."));
    m_repeaterId = makeText(value.repeaterId, QStringLiteral("None (use the host and port above)"),
                            QStringLiteral("The server's ID number at the repeater."));

    // One tab per section keeps the session window short.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs);
    const auto section = [this, tabs](const QString &title) {
        auto *page = new QWidget(this);
        auto *form = new QFormLayout(page);
        tabs->addTab(page, title);
        return form;
    };
    auto *display = section(QStringLiteral("Display"));
    display->addRow(QStringLiteral("Resolution:"), m_resolution);
    display->addRow(QStringLiteral("Colours:"), m_colorDepth);
    display->addRow(QStringLiteral("Quality:"), m_quality);
    display->addRow(QString(), m_highDpi);
    display->addRow(QString(), m_allMonitors);
    auto *resources = section(QStringLiteral("Local resources"));
    resources->addRow(QString(), m_remoteCursor);
    resources->addRow(QString(), m_clipboard);
    auto *connection = section(QStringLiteral("Connection"));
    connection->addRow(QStringLiteral("Encryption:"), m_encryption);
    connection->addRow(QString(), m_autoReconnect);
    connection->addRow(QStringLiteral("SSH tunnel:"), m_sshTunnel);
    connection->addRow(QString(), m_listen);
    connection->addRow(QStringLiteral("Listen on port:"), m_listenPort);
    connection->addRow(QStringLiteral("Repeater:"), m_repeater);
    connection->addRow(QStringLiteral("Repeater ID:"), m_repeaterId);
}

VncOptions VncOptionsEditor::value() const
{
    VncOptions options;
    options.resolution = m_resolution->currentData().toString();
    options.colorDepth = m_colorDepth->currentData().toString().toInt();
    options.quality = m_quality->currentData().toString();
    options.encryption = m_encryption->currentData().toString();
    // A switch is stored only when it differs from the default, so changing
    // the default later still applies to this session.
    const auto switchValue = [](const QCheckBox *box, int fallback) {
        const int value = box->isChecked() ? 1 : 0;
        return value == fallback ? -1 : value;
    };
    options.remoteCursor = switchValue(m_remoteCursor, m_defaults.remoteCursor);
    options.clipboard = switchValue(m_clipboard, m_defaults.clipboard);
    options.autoReconnect = switchValue(m_autoReconnect, m_defaults.autoReconnect);
    options.highDpi = switchValue(m_highDpi, m_defaults.highDpi);
    options.allMonitors = switchValue(m_allMonitors, m_defaults.allMonitors);
    // A listed choice stores its value; typed text is stored as written.
    const int index = m_sshTunnel->currentIndex();
    options.sshTunnel = index >= 0 && m_sshTunnel->itemText(index) == m_sshTunnel->currentText()
                            ? m_sshTunnel->itemData(index).toString()
                            : m_sshTunnel->currentText().trimmed();
    options.listen = m_listen->isChecked() ? 1 : -1;
    options.listenPort = m_listen->isChecked() && m_listenPort->value() != 5500 ? m_listenPort->value() : 0;
    options.repeater = m_repeater->text().trimmed();
    options.repeaterId = m_repeaterId->text().trimmed();
    return options;
}
