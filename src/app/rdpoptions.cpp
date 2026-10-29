// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "rdpoptions.h"
#include "availabilitynote.h"
#include "localresources.h"
#include "appdirs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QStandardItemModel>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QInputMethod>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTabWidget>

#include <yaml-cpp/yaml.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QString settingsPath()
{
    return portableFilePath(QStringLiteral("settings/settings.yaml"));
}

bool validResolution(const QString &value)
{
    static const QRegularExpression size(QStringLiteral("^\\d{3,4}x\\d{3,4}$"));
    return value == QStringLiteral("follow") || value == QStringLiteral("connect")
           || size.match(value).hasMatch();
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

bool RdpOptions::isEmpty() const
{
    return resolution.isEmpty() && colorDepth == 0 && quality.isEmpty() && security.isEmpty()
           && adminSession < 0 && clipboardText < 0 && clipboardFiles < 0
           && graphicsPipeline < 0 && highDpi < 0 && allMonitors < 0 && gateway.isEmpty()
           && gatewayUser.isEmpty() && keyboardLayout.isEmpty() && autoReconnect < 0
           && askCredentials < 0 && startProgram.isEmpty() && startFolder.isEmpty()
           && audio.isEmpty() && microphone < 0 && drives.isEmpty() && driveFolders.isEmpty()
           && printers < 0 && smartCards < 0;
}

RdpOptions RdpOptions::overriddenBy(const RdpOptions &session) const
{
    RdpOptions result = *this;
    if (!session.resolution.isEmpty())
        result.resolution = session.resolution;
    if (session.colorDepth > 0)
        result.colorDepth = session.colorDepth;
    if (!session.quality.isEmpty())
        result.quality = session.quality;
    if (!session.security.isEmpty())
        result.security = session.security;
    if (session.adminSession >= 0)
        result.adminSession = session.adminSession;
    if (session.clipboardText >= 0)
        result.clipboardText = session.clipboardText;
    if (session.clipboardFiles >= 0)
        result.clipboardFiles = session.clipboardFiles;
    if (session.graphicsPipeline >= 0)
        result.graphicsPipeline = session.graphicsPipeline;
    if (session.highDpi >= 0)
        result.highDpi = session.highDpi;
    if (session.allMonitors >= 0)
        result.allMonitors = session.allMonitors;
    if (!session.keyboardLayout.isEmpty())
        result.keyboardLayout = session.keyboardLayout;
    if (session.autoReconnect >= 0)
        result.autoReconnect = session.autoReconnect;
    if (session.askCredentials >= 0)
        result.askCredentials = session.askCredentials;
    if (!session.startProgram.isEmpty()) {
        result.startProgram = session.startProgram;
        result.startFolder = session.startFolder;
    }
    if (!session.audio.isEmpty())
        result.audio = session.audio;
    if (session.microphone >= 0)
        result.microphone = session.microphone;
    // The folders belong to the "chosen folders" they are set with.
    if (!session.drives.isEmpty()) {
        result.drives = session.drives;
        result.driveFolders = session.driveFolders;
    }
    if (session.printers >= 0)
        result.printers = session.printers;
    if (session.smartCards >= 0)
        result.smartCards = session.smartCards;
    // The gateway user belongs to the gateway it is set with.
    if (!session.gateway.isEmpty()) {
        result.gateway = session.gateway;
        result.gatewayUser = session.gatewayUser;
    } else if (!session.gatewayUser.isEmpty()) {
        result.gatewayUser = session.gatewayUser;
    }
    return result;
}

RdpOptions RdpOptions::builtIn()
{
    RdpOptions options;
    options.resolution = QStringLiteral("follow");
    options.colorDepth = 32;
    options.quality = QStringLiteral("lan");
    options.security = QStringLiteral("auto");
    options.adminSession = 0;
    options.clipboardText = 1;
    options.clipboardFiles = 1;
    options.graphicsPipeline = 1;
    options.highDpi = 1;
    options.allMonitors = 0;
    options.keyboardLayout = QStringLiteral("auto");
    options.autoReconnect = 1;
    options.askCredentials = 1;
    // Sound and printers are off unless chosen (globally or per session).
    options.audio = QStringLiteral("off");
    options.microphone = 0;
    options.drives = QStringLiteral("none");
    options.printers = 0;
    options.smartCards = 0;
    return options;
}

RdpChoices rdpAudioChoices()
{
    return {{QStringLiteral("local"), QStringLiteral("Play on this computer")},
            {QStringLiteral("remote"), QStringLiteral("Leave on the remote computer")},
            {QStringLiteral("off"), QStringLiteral("Don't play")}};
}

RdpChoices rdpDriveChoices()
{
    return {{QStringLiteral("none"), QStringLiteral("None")},
            {QStringLiteral("shared"), QStringLiteral("The package's shared folder")},
            {QStringLiteral("home"), QStringLiteral("My home folder")},
            {QStringLiteral("all"), QStringLiteral("All local drives")},
            {QStringLiteral("custom"), QStringLiteral("Chosen folders")}};
}

RdpChoices rdpResolutionChoices()
{
    return {{QStringLiteral("follow"), QStringLiteral("Follow the tab size")},
            {QStringLiteral("connect"), QStringLiteral("Tab size when connecting")},
            {QStringLiteral("1280x720"), QStringLiteral("1280 × 720")},
            {QStringLiteral("1366x768"), QStringLiteral("1366 × 768")},
            {QStringLiteral("1600x900"), QStringLiteral("1600 × 900")},
            {QStringLiteral("1920x1080"), QStringLiteral("1920 × 1080")},
            {QStringLiteral("2560x1440"), QStringLiteral("2560 × 1440")}};
}

RdpChoices rdpColorDepthChoices()
{
    return {{QStringLiteral("32"), QStringLiteral("32-bit")},
            {QStringLiteral("24"), QStringLiteral("24-bit")},
            {QStringLiteral("16"), QStringLiteral("16-bit")},
            {QStringLiteral("15"), QStringLiteral("15-bit")}};
}

RdpChoices rdpQualityChoices()
{
    return {{QStringLiteral("lan"), QStringLiteral("LAN (all visual effects)")},
            {QStringLiteral("broadband"), QStringLiteral("Broadband")},
            {QStringLiteral("low"), QStringLiteral("Low bandwidth (no wallpaper or animations)")}};
}

RdpChoices rdpSecurityChoices()
{
    return {{QStringLiteral("auto"), QStringLiteral("Automatic")},
            {QStringLiteral("nla"), QStringLiteral("NLA (Network Level Authentication)")},
            {QStringLiteral("tls"), QStringLiteral("TLS")},
            {QStringLiteral("rdp"), QStringLiteral("Legacy RDP encryption")}};
}

RdpChoices rdpKeyboardChoices()
{
    return {{QStringLiteral("auto"), QStringLiteral("This computer's layout")},
            {QStringLiteral("00000409"), QStringLiteral("English (United States)")},
            {QStringLiteral("00000809"), QStringLiteral("English (United Kingdom)")},
            {QStringLiteral("00020409"), QStringLiteral("English (US International)")},
            {QStringLiteral("00000408"), QStringLiteral("Greek")},
            {QStringLiteral("00000407"), QStringLiteral("German")},
            {QStringLiteral("0000040c"), QStringLiteral("French")},
            {QStringLiteral("0000080c"), QStringLiteral("French (Belgium)")},
            {QStringLiteral("0000100c"), QStringLiteral("French (Switzerland)")},
            {QStringLiteral("00000807"), QStringLiteral("German (Switzerland)")},
            {QStringLiteral("0000040a"), QStringLiteral("Spanish")},
            {QStringLiteral("00000410"), QStringLiteral("Italian")},
            {QStringLiteral("00000816"), QStringLiteral("Portuguese")},
            {QStringLiteral("00000416"), QStringLiteral("Portuguese (Brazil)")},
            {QStringLiteral("00000413"), QStringLiteral("Dutch")},
            {QStringLiteral("00000406"), QStringLiteral("Danish")},
            {QStringLiteral("0000041d"), QStringLiteral("Swedish")},
            {QStringLiteral("00000414"), QStringLiteral("Norwegian")},
            {QStringLiteral("0000040b"), QStringLiteral("Finnish")},
            {QStringLiteral("00000415"), QStringLiteral("Polish")},
            {QStringLiteral("00000405"), QStringLiteral("Czech")},
            {QStringLiteral("0000040e"), QStringLiteral("Hungarian")},
            {QStringLiteral("00000418"), QStringLiteral("Romanian")},
            {QStringLiteral("00000402"), QStringLiteral("Bulgarian")},
            {QStringLiteral("00000419"), QStringLiteral("Russian")},
            {QStringLiteral("00000422"), QStringLiteral("Ukrainian")},
            {QStringLiteral("0000041f"), QStringLiteral("Turkish")},
            {QStringLiteral("0000040d"), QStringLiteral("Hebrew")},
            {QStringLiteral("00000401"), QStringLiteral("Arabic")},
            {QStringLiteral("00000411"), QStringLiteral("Japanese")},
            {QStringLiteral("00000412"), QStringLiteral("Korean")}};
}

quint32 rdpKeyboardLayoutId(const QString &value)
{
    if (value != QStringLiteral("auto") && !value.isEmpty()) {
        bool ok = false;
        const quint32 id = value.toUInt(&ok, 16);
        return ok && id ? id : 0x409;
    }
#ifdef Q_OS_WIN
    // The language part of the active layout (0x0408 for Greek).
    const auto layout = reinterpret_cast<quintptr>(GetKeyboardLayout(0));
    if (const quint32 language = quint32(layout & 0xFFFF))
        return language;
#endif
    // Elsewhere, the input language: the layouts above whose language matches.
    const QLocale locale = QGuiApplication::inputMethod() ? QGuiApplication::inputMethod()->locale()
                                                          : QLocale::system();
    static const QList<QPair<QLocale::Language, quint32>> languages = {
        {QLocale::Greek, 0x408}, {QLocale::German, 0x407}, {QLocale::French, 0x40C},
        {QLocale::Spanish, 0x40A}, {QLocale::Italian, 0x410}, {QLocale::Dutch, 0x413},
        {QLocale::Danish, 0x406}, {QLocale::Swedish, 0x41D}, {QLocale::NorwegianBokmal, 0x414},
        {QLocale::Finnish, 0x40B}, {QLocale::Polish, 0x415}, {QLocale::Czech, 0x405},
        {QLocale::Hungarian, 0x40E}, {QLocale::Romanian, 0x418}, {QLocale::Bulgarian, 0x402},
        {QLocale::Russian, 0x419}, {QLocale::Ukrainian, 0x422}, {QLocale::Turkish, 0x41F},
        {QLocale::Hebrew, 0x40D}, {QLocale::Arabic, 0x401}, {QLocale::Japanese, 0x411},
        {QLocale::Korean, 0x412}};
    if (locale.language() == QLocale::English)
        return locale.territory() == QLocale::UnitedKingdom ? 0x809 : 0x409;
    if (locale.language() == QLocale::Portuguese)
        return locale.territory() == QLocale::Brazil ? 0x416 : 0x816;
    for (const auto &[language, id] : languages) {
        if (locale.language() == language)
            return id;
    }
    return 0x409;
}

RdpOptions rdpOptionsFromYaml(const YAML::Node &node)
{
    RdpOptions options;
    if (!node || !node.IsMap())
        return options;
    try {
        if (node["resolution"]) {
            const auto value = QString::fromStdString(node["resolution"].as<std::string>());
            if (validResolution(value))
                options.resolution = value;
        }
        if (node["color_depth"]) {
            const int depth = node["color_depth"].as<int>();
            if (depth == 32 || depth == 24 || depth == 16 || depth == 15)
                options.colorDepth = depth;
        }
        const auto readChoice = [&node](const char *key, const RdpChoices &choices, QString &target) {
            if (!node[key])
                return;
            const auto value = QString::fromStdString(node[key].as<std::string>());
            for (const auto &choice : choices) {
                if (choice.first == value)
                    target = value;
            }
        };
        readChoice("quality", rdpQualityChoices(), options.quality);
        readChoice("security", rdpSecurityChoices(), options.security);
        const auto readSwitch = [&node](const char *key, int &target) {
            if (node[key])
                target = node[key].as<bool>() ? 1 : 0;
        };
        readSwitch("admin_session", options.adminSession);
        readSwitch("clipboard_text", options.clipboardText);
        readSwitch("clipboard_files", options.clipboardFiles);
        readSwitch("graphics_pipeline", options.graphicsPipeline);
        readSwitch("high_dpi", options.highDpi);
        readSwitch("all_monitors", options.allMonitors);
        const auto readText = [&node](const char *key, QString &target) {
            if (node[key])
                target = QString::fromStdString(node[key].as<std::string>()).trimmed().left(255);
        };
        readText("gateway", options.gateway);
        readText("gateway_user", options.gatewayUser);
        readChoice("keyboard_layout", rdpKeyboardChoices(), options.keyboardLayout);
        readSwitch("auto_reconnect", options.autoReconnect);
        readSwitch("ask_credentials", options.askCredentials);
        readText("start_program", options.startProgram);
        readText("start_folder", options.startFolder);
        readChoice("audio", rdpAudioChoices(), options.audio);
        readSwitch("microphone", options.microphone);
        readChoice("drives", rdpDriveChoices(), options.drives);
        if (node["drive_folders"])
            options.driveFolders = QString::fromStdString(node["drive_folders"].as<std::string>()).trimmed().left(4000);
        readSwitch("printers", options.printers);
        readSwitch("smart_cards", options.smartCards);
    } catch (const YAML::Exception &) {
        // Keep what was read; invalid values use the defaults.
    }
    return options;
}

void rdpOptionsToYaml(const RdpOptions &options, YAML::Node &node)
{
    if (!options.resolution.isEmpty())
        node["resolution"] = options.resolution.toStdString();
    if (options.colorDepth > 0)
        node["color_depth"] = options.colorDepth;
    if (!options.quality.isEmpty())
        node["quality"] = options.quality.toStdString();
    if (!options.security.isEmpty())
        node["security"] = options.security.toStdString();
    if (options.adminSession >= 0)
        node["admin_session"] = options.adminSession == 1;
    if (options.clipboardText >= 0)
        node["clipboard_text"] = options.clipboardText == 1;
    if (options.clipboardFiles >= 0)
        node["clipboard_files"] = options.clipboardFiles == 1;
    if (options.graphicsPipeline >= 0)
        node["graphics_pipeline"] = options.graphicsPipeline == 1;
    if (options.highDpi >= 0)
        node["high_dpi"] = options.highDpi == 1;
    if (options.allMonitors >= 0)
        node["all_monitors"] = options.allMonitors == 1;
    if (!options.gateway.isEmpty())
        node["gateway"] = options.gateway.toStdString();
    if (!options.gatewayUser.isEmpty())
        node["gateway_user"] = options.gatewayUser.toStdString();
    if (!options.keyboardLayout.isEmpty())
        node["keyboard_layout"] = options.keyboardLayout.toStdString();
    if (options.autoReconnect >= 0)
        node["auto_reconnect"] = options.autoReconnect == 1;
    if (options.askCredentials >= 0)
        node["ask_credentials"] = options.askCredentials == 1;
    if (!options.startProgram.isEmpty())
        node["start_program"] = options.startProgram.toStdString();
    if (!options.startFolder.isEmpty())
        node["start_folder"] = options.startFolder.toStdString();
    if (!options.audio.isEmpty())
        node["audio"] = options.audio.toStdString();
    if (options.microphone >= 0)
        node["microphone"] = options.microphone == 1;
    if (!options.drives.isEmpty())
        node["drives"] = options.drives.toStdString();
    if (!options.driveFolders.isEmpty())
        node["drive_folders"] = options.driveFolders.toStdString();
    if (options.printers >= 0)
        node["printers"] = options.printers == 1;
    if (options.smartCards >= 0)
        node["smart_cards"] = options.smartCards == 1;
}

RdpOptions loadRdpDefaults()
{
    try {
        return RdpOptions::builtIn().overriddenBy(
            rdpOptionsFromYaml(YAML::LoadFile(settingsPath().toStdString())["rdp"]));
    } catch (const YAML::Exception &) {
        return RdpOptions::builtIn();
    }
}

void saveRdpDefaults(const RdpOptions &options)
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
    rdpOptionsToYaml(options, node);
    settings["rdp"] = node;
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

RdpOptionsEditor::RdpOptionsEditor(const RdpOptions &value, bool withDefault,
                                   const RdpOptions &defaults, QWidget *parent)
    : QWidget(parent), m_defaults(RdpOptions::builtIn().overriddenBy(defaults)),
      m_withDefault(withDefault)
{
    const auto makeBox = [this, withDefault](const RdpChoices &choices, const QString &current,
                                             const QString &defaultValue) {
        auto *box = new QComboBox(this);
        if (withDefault)
            box->addItem(QStringLiteral("Default (%1)").arg(labelFor(choices, defaultValue)),
                         QString());
        for (const auto &choice : choices)
            box->addItem(choice.second, choice.first);
        box->setCurrentIndex(qMax(0, box->findData(current)));
        connect(box, &QComboBox::currentIndexChanged, this, &RdpOptionsEditor::changed);
        return box;
    };
    m_resolution = makeBox(rdpResolutionChoices(), value.resolution, defaults.resolution);
    m_colorDepth = makeBox(rdpColorDepthChoices(),
                           value.colorDepth > 0 ? QString::number(value.colorDepth) : QString(),
                           QString::number(defaults.colorDepth));
    m_quality = makeBox(rdpQualityChoices(), value.quality, defaults.quality);
    m_security = makeBox(rdpSecurityChoices(), value.security, defaults.security);
    // A checkbox shows the session's own value, or the default if it has none.
    const auto makeSwitch = [this](const QString &text, const QString &tip, int own, int fallback) {
        auto *box = new QCheckBox(text, this);
        box->setToolTip(tip);
        box->setChecked((own >= 0 ? own : fallback) == 1);
        connect(box, &QCheckBox::toggled, this, &RdpOptionsEditor::changed);
        return box;
    };
    m_clipboardText = makeSwitch(QStringLiteral("Share clipboard text"),
                                 QStringLiteral("Copy text here and paste it on the remote computer, "
                                                "and the other way round."),
                                 value.clipboardText, m_defaults.clipboardText);
    m_clipboardFiles = makeSwitch(QStringLiteral("Share files through the clipboard"),
                                  QStringLiteral("Copy or drop files onto the remote screen and paste "
                                                 "them there; save files copied on the remote "
                                                 "computer."),
                                  value.clipboardFiles, m_defaults.clipboardFiles);
    m_admin = makeSwitch(QStringLiteral("Administrator (console) session"),
                         QStringLiteral("Connects to the server's console session, like mstsc /admin."),
                         value.adminSession, m_defaults.adminSession);
    m_graphics = makeSwitch(QStringLiteral("Modern graphics (faster, smoother)"),
                            QStringLiteral("The RDP 8 graphics pipeline with RemoteFX, and H.264 "
                                           "where this build can decode it. Turn off for old "
                                           "servers that draw incorrectly. Needs 32-bit colour."),
                            value.graphicsPipeline, m_defaults.graphicsPipeline);
    m_highDpi = makeSwitch(QStringLiteral("Sharp on high-DPI screens"),
                           QStringLiteral("Uses the screen's full resolution and asks Windows to "
                                          "scale text and icons to match, instead of enlarging a "
                                          "smaller desktop."),
                           value.highDpi, m_defaults.highDpi);
    m_allMonitors = makeSwitch(QStringLiteral("Full screen uses all monitors"),
                               QStringLiteral("In full screen (Ctrl+Alt+Enter), the remote desktop "
                                              "gets one monitor per local monitor. Needs a server "
                                              "that allows several monitors."),
                               value.allMonitors, m_defaults.allMonitors);
    const auto makeText = [this, withDefault](const QString &current, const QString &defaultValue,
                                              const QString &emptyText, const QString &tip) {
        auto *edit = new QLineEdit(current, this);
        edit->setPlaceholderText(withDefault && !defaultValue.isEmpty()
                                     ? QStringLiteral("Default (%1)").arg(defaultValue)
                                     : emptyText);
        edit->setToolTip(tip);
        edit->setMaxLength(255);
        connect(edit, &QLineEdit::textChanged, this, &RdpOptionsEditor::changed);
        return edit;
    };
    m_gateway = makeText(value.gateway, defaults.gateway, QStringLiteral("None (connect directly)"),
                         QStringLiteral("An RD Gateway, as host or host:port (port 443 if not "
                                        "given). Type \"none\" to connect directly when a "
                                        "default gateway is set."));
    m_gatewayUser = makeText(value.gatewayUser, defaults.gatewayUser,
                             QStringLiteral("Same as the session"),
                             QStringLiteral("The gateway's user name, as DOMAIN\\user or "
                                            "user@domain. Leave empty to log on to the gateway "
                                            "with the session's own credentials."));
    m_keyboard = makeBox(rdpKeyboardChoices(), value.keyboardLayout, defaults.keyboardLayout);
    m_keyboard->setToolTip(QStringLiteral("The remote session's keyboard layout. Keys are sent "
                                          "by position, so this decides which characters they "
                                          "type there."));
    m_autoReconnect = makeSwitch(QStringLiteral("Reconnect automatically if the network drops"),
                                 QStringLiteral("Tries again for about half a minute, resuming the "
                                                "same remote session."),
                                 value.autoReconnect, m_defaults.autoReconnect);
    m_askCredentials = makeSwitch(QStringLiteral("Ask for the password before connecting"),
                                  QStringLiteral("Off: when no password is saved, connect first "
                                                 "and log on at the remote computer's own screen "
                                                 "(not possible with NLA, which Windows usually "
                                                 "requires)."),
                                  value.askCredentials, m_defaults.askCredentials);
    m_startProgram = makeText(value.startProgram, defaults.startProgram,
                              QStringLiteral("None (the desktop)"),
                              QStringLiteral("A program to start instead of the desktop, such as "
                                             "C:\\Tools\\app.exe. Windows Server and xrdp honour "
                                             "it; Windows 10 and 11 ignore it."));
    m_startFolder = makeText(value.startFolder, defaults.startFolder, QStringLiteral("Default"),
                             QStringLiteral("The folder the program starts in."));

    m_audio = makeBox(rdpAudioChoices(), value.audio, defaults.audio.isEmpty() ? QStringLiteral("off") : defaults.audio);
    m_audio->setToolTip(QStringLiteral("Where the remote computer's sound plays."));
    m_microphone = makeSwitch(QStringLiteral("Use this computer's microphone"),
                              QStringLiteral("Programs on the remote computer can record from it "
                                             "(calls, dictation)."),
                              value.microphone, m_defaults.microphone);
    m_drives = makeBox(rdpDriveChoices(), value.drives, defaults.drives.isEmpty() ? QStringLiteral("none") : defaults.drives);
    m_drives->setToolTip(QStringLiteral("Local folders that appear as drives in the remote "
                                        "session's File Explorer (\\tsclient)."));
    m_driveFolders = makeText(value.driveFolders, defaults.driveFolders,
                              QStringLiteral("Folders, separated by ;"),
                              QStringLiteral("For \"Chosen folders\": e.g. C:\\Data;D:\\ISO or "
                                             "/home/me/work;/mnt/usb"));
    m_driveFolders->setMaxLength(4000);
    const auto updateFolders = [this] {
        const QString choice = m_drives->currentData().toString();
        m_driveFolders->setEnabled(choice == QStringLiteral("custom")
                                   || (choice.isEmpty() && m_defaults.drives == QStringLiteral("custom")));
    };
    connect(m_drives, &QComboBox::currentIndexChanged, this, updateFolders);
    updateFolders();
    m_printers = makeSwitch(QStringLiteral("Use this computer's printers"),
                            QStringLiteral("Local printers appear in the remote session."),
                            value.printers, m_defaults.printers);
    m_smartCards = makeSwitch(QStringLiteral("Use this computer's smart cards"),
                              QStringLiteral("For smart-card log-on and signing on the remote "
                                             "computer. On Linux, needs the pcscd service."),
                              value.smartCards, m_defaults.smartCards);

    // Three sections, so the many options stay easy to scan.
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
    display->addRow(QStringLiteral("Colour depth:"), m_colorDepth);
    display->addRow(QStringLiteral("Connection quality:"), m_quality);
    display->addRow(QString(), m_graphics);
    display->addRow(QString(), m_highDpi);
    display->addRow(QString(), m_allMonitors);
    auto *resources = section(QStringLiteral("Local resources"));
    resources->addRow(QStringLiteral("Keyboard layout:"), m_keyboard);
    resources->addRow(QString(), m_clipboardText);
    resources->addRow(QString(), m_clipboardFiles);
    resources->addRow(QStringLiteral("Remote audio:"), m_audio);
    resources->addRow(QString(), m_microphone);
    addAvailabilityRow(resources, &LocalResources::soundProblem, [this](bool available) {
        // Playing here and the microphone need a sound server; leaving the
        // sound on the remote computer does not.
        if (auto *model = qobject_cast<QStandardItemModel *>(m_audio->model())) {
            if (auto *item = model->item(m_audio->findData(QStringLiteral("local"))))
                item->setEnabled(available);
        }
        m_microphone->setEnabled(available);
    });
    resources->addRow(QStringLiteral("Drives:"), m_drives);
    resources->addRow(QStringLiteral("Folders:"), m_driveFolders);
    resources->addRow(QString(), m_printers);
    addAvailabilityRow(resources, &LocalResources::printerProblem,
                       [this](bool available) { m_printers->setEnabled(available); });
    resources->addRow(QString(), m_smartCards);
    addAvailabilityRow(resources, &LocalResources::smartCardProblem,
                       [this](bool available) { m_smartCards->setEnabled(available); });
    auto *connection = section(QStringLiteral("Connection"));
    connection->addRow(QStringLiteral("Security:"), m_security);
    connection->addRow(QString(), m_askCredentials);
    connection->addRow(QString(), m_autoReconnect);
    connection->addRow(QString(), m_admin);
    connection->addRow(QStringLiteral("Start program:"), m_startProgram);
    connection->addRow(QStringLiteral("Start in folder:"), m_startFolder);
    connection->addRow(QStringLiteral("RD Gateway:"), m_gateway);
    connection->addRow(QStringLiteral("Gateway user:"), m_gatewayUser);
}

// Under an option that needs something outside OpsRemote: why it is
// unavailable, with Check again (the option is disabled meanwhile).
void RdpOptionsEditor::addAvailabilityRow(QFormLayout *form, QString (*problem)(),
                                          const std::function<void(bool)> &apply)
{
    form->addRow(QString(), new AvailabilityNote(problem, apply, this));
}

RdpOptions RdpOptionsEditor::value() const
{
    RdpOptions options;
    options.resolution = m_resolution->currentData().toString();
    options.colorDepth = m_colorDepth->currentData().toString().toInt();
    options.quality = m_quality->currentData().toString();
    options.security = m_security->currentData().toString();
    // For a session, store a switch only when it differs from the default, so
    // changing the default later still applies to this session.
    const auto switchValue = [this](const QCheckBox *box, int fallback) {
        const int value = box->isChecked() ? 1 : 0;
        return m_withDefault && value == fallback ? -1 : value;
    };
    options.adminSession = switchValue(m_admin, m_defaults.adminSession);
    options.clipboardText = switchValue(m_clipboardText, m_defaults.clipboardText);
    options.clipboardFiles = switchValue(m_clipboardFiles, m_defaults.clipboardFiles);
    options.graphicsPipeline = switchValue(m_graphics, m_defaults.graphicsPipeline);
    options.highDpi = switchValue(m_highDpi, m_defaults.highDpi);
    options.allMonitors = switchValue(m_allMonitors, m_defaults.allMonitors);
    options.gateway = m_gateway->text().trimmed();
    options.gatewayUser = m_gatewayUser->text().trimmed();
    options.keyboardLayout = m_keyboard->currentData().toString();
    options.autoReconnect = switchValue(m_autoReconnect, m_defaults.autoReconnect);
    options.askCredentials = switchValue(m_askCredentials, m_defaults.askCredentials);
    options.startProgram = m_startProgram->text().trimmed();
    options.startFolder = m_startFolder->text().trimmed();
    options.audio = m_audio->currentData().toString();
    options.microphone = switchValue(m_microphone, m_defaults.microphone);
    options.drives = m_drives->currentData().toString();
    options.driveFolders = m_driveFolders->text().trimmed();
    options.printers = switchValue(m_printers, m_defaults.printers);
    options.smartCards = switchValue(m_smartCards, m_defaults.smartCards);
    return options;
}
