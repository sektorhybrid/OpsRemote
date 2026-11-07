// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "vncoptions.h"
#include "formcard.h"
#include "inheritance.h"
#include "reconnect.h"
#include "preferences.h"
#include "appdirs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QTabWidget>
#include <QTabBar>
#include <QVBoxLayout>

#include <yaml-cpp/yaml.h>

namespace {

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
    if (session.reconnectDelay > 0)
        result.reconnectDelay = session.reconnectDelay;
    if (session.reconnectAttempts >= 0)
        result.reconnectAttempts = session.reconnectAttempts;
    number(result.highDpi, session.highDpi);
    number(result.allMonitors, session.allMonitors);
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
    options.reconnectDelay = DefaultReconnectDelay;
    options.reconnectAttempts = DefaultReconnectAttempts;
    options.highDpi = 1;
    options.allMonitors = 0;
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
        readReconnectTiming(node, options.reconnectDelay, options.reconnectAttempts);
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
    writeReconnectTiming(node, options.reconnectDelay, options.reconnectAttempts);
    flag("high_dpi", options.highDpi);
    flag("all_monitors", options.allMonitors);
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
            vncOptionsFromYaml(loadPreferencesSection("vnc")));
        return defaults;
    } catch (const YAML::Exception &) {
        return VncOptions::builtIn();
    }
}

void saveVncDefaults(const VncOptions &options)
{
    // Defaults never include the session-only fields.
    VncOptions defaults = options;
    defaults.listen = -1;
    defaults.listenPort = 0;
    defaults.repeater.clear();
    defaults.repeaterId.clear();
    YAML::Node node(YAML::NodeType::Map);
    vncOptionsToYaml(defaults, node);
    savePreferencesSection("vnc", node);
}

VncOptionsEditor::VncOptionsEditor(const VncOptions &value, const VncOptions &defaults,
                                   QWidget *parent)
    : QWidget(parent), m_defaults(VncOptions::builtIn().overriddenBy(defaults))
{
    const auto makeBox = [this](const RdpChoices &choices, const QString &current,
                                const QString &defaultValue) {
        auto *box = new QComboBox(this);
        addInheritedItem(box, QString(labelFor(choices, defaultValue)), QString());
        for (const auto &choice : choices)
            if (choice.first != defaultValue) // listed once, as the inherited one
                box->addItem(choice.second, choice.first);
        box->setCurrentIndex(qMax(0, box->findData(current)));
        connect(box, &QComboBox::currentIndexChanged, this, &VncOptionsEditor::changed);
        return box;
    };
    const auto makeSwitch = [this](const QString &text, const QString &tip, int own, int fallback) {
        auto *box = new QCheckBox(text, this);
        box->setToolTip(tip);
        box->setChecked((own >= 0 ? own : fallback) == 1);
        box->setProperty("inheritedOn", fallback == 1); // applyInheritance()
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
    m_encryption->setToolTip(QStringLiteral("Plain VNC is not encrypted; a jump host encrypts "
                                            "any server."));
    m_autoReconnect = makeSwitch(QStringLiteral("Reconnect automatically if the network drops"),
                                 QStringLiteral("Tries again for about half a minute."),
                                 value.autoReconnect, m_defaults.autoReconnect);

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
    // A tab of light cards (formcard.h).
    const auto section = [this, tabs](const QString &title) {
        auto *page = new QWidget(this);
        auto *layout = new QVBoxLayout(page);
        layout->setSpacing(6);
        tabs->addTab(page, title);
        return layout;
    };
    const auto finish = [this](QVBoxLayout *page) {
        page->addStretch();
        alignFormLabels(this); // all tabs alike
    };
    auto *displayPage = section(QStringLiteral("Display"));
    auto *display = addFormCard(displayPage, QStringLiteral("General"), this);
    display->addRow(QStringLiteral("Resolution:"), m_resolution);
    display->addRow(QStringLiteral("Colours:"), m_colorDepth);
    display->addRow(QStringLiteral("Quality:"), m_quality);
    display->addRow(QString(), m_highDpi);
    display->addRow(QString(), m_allMonitors);
    finish(displayPage);
    auto *resourcesPage = section(QStringLiteral("Local resources"));
    auto *resources = addFormCard(resourcesPage, QStringLiteral("General"), this);
    resources->addRow(QString(), m_remoteCursor);
    resources->addRow(QString(), m_clipboard);
    finish(resourcesPage);
    auto *connectionPage = section(QStringLiteral("Connection"));
    // The same cards as every session type: General, Logon, then VNC's own.
    auto *connection = addFormCard(connectionPage, QStringLiteral("General"), this);
    connection->addRow(QString(), m_autoReconnect);
    // The session type's wait and tries, inherited until set here.
    m_reconnectDelay = reconnectDelayBox(value.reconnectDelay, m_defaults.reconnectDelay);
    m_reconnectAttempts = reconnectAttemptsBox(value.reconnectAttempts, m_defaults.reconnectAttempts);
    connection->addRow(QStringLiteral("Wait:"), m_reconnectDelay);
    connection->addRow(QStringLiteral("Tries:"), m_reconnectAttempts);
    for (QWidget *box : {static_cast<QWidget *>(m_reconnectDelay), static_cast<QWidget *>(m_reconnectAttempts)}) {
        box->setEnabled(m_autoReconnect->isChecked());
        connect(m_autoReconnect, &QCheckBox::toggled, box, &QWidget::setEnabled);
    }
    connection = addFormCard(connectionPage, QStringLiteral("Logon"), this);
    connection->addRow(QStringLiteral("Encryption:"), m_encryption);
    connection = addFormCard(connectionPage, QStringLiteral("Listening"), this);
    connection->addRow(QString(), m_listen);
    connection->addRow(QStringLiteral("Port:"), m_listenPort);
    connection = addFormCard(connectionPage, QStringLiteral("Repeater"), this);
    connection->addRow(QStringLiteral("Address:"), m_repeater);
    connection->addRow(QStringLiteral("ID:"), m_repeaterId);
    finish(connectionPage);
    // Connection first, as in every session type's window.
    tabs->tabBar()->moveTab(tabs->indexOf(connectionPage->parentWidget()), 0);
    tabs->setCurrentIndex(0);
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
    options.reconnectDelay = m_reconnectDelay->value();
    options.reconnectAttempts = m_reconnectAttempts->value();
    options.highDpi = switchValue(m_highDpi, m_defaults.highDpi);
    options.allMonitors = switchValue(m_allMonitors, m_defaults.allMonitors);
    options.listen = m_listen->isChecked() ? 1 : -1;
    options.listenPort = m_listen->isChecked() && m_listenPort->value() != 5500 ? m_listenPort->value() : 0;
    options.repeater = m_repeater->text().trimmed();
    options.repeaterId = m_repeaterId->text().trimmed();
    return options;
}
