// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "xdmcp.h"
#include "appdirs.h"
#include "availabilitynote.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTcpServer>
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

// A display number no X server uses yet, from :10 on (lower ones are
// usually this computer's own).
int freeDisplay()
{
    for (int display = 10; display < 100; ++display) {
#ifdef Q_OS_WIN
        // The Windows X servers listen on TCP 6000 + display.
        QTcpServer probe;
        if (probe.listen(QHostAddress::LocalHost, quint16(6000 + display)))
            return display;
#else
        if (!QFileInfo::exists(QStringLiteral("/tmp/.X11-unix/X%1").arg(display))
            && !QFileInfo::exists(QStringLiteral("/tmp/.X%1-lock").arg(display)))
            return display;
#endif
    }
    return 99;
}

QString windowsXServer(const QString &kind)
{
    const QStringList roots{qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)"),
                            qEnvironmentVariable("ProgramW6432")};
    const QString relative = kind == QStringLiteral("xming") ? QStringLiteral("Xming/Xming.exe")
                                                             : QStringLiteral("VcXsrv/vcxsrv.exe");
    for (const auto &root : roots) {
        if (root.isEmpty())
            continue;
        const QString path = QDir(root).filePath(relative);
        if (QFileInfo(path).isExecutable())
            return path;
    }
    return QStandardPaths::findExecutable(QFileInfo(relative).fileName());
}
}

XdmcpOptions XdmcpOptions::overriddenBy(const XdmcpOptions &session) const
{
    XdmcpOptions result = *this;
    if (!session.mode.isEmpty())
        result.mode = session.mode;
    if (!session.program.isEmpty())
        result.program = session.program;
    if (!session.size.isEmpty())
        result.size = session.size;
    return result;
}

XdmcpOptions XdmcpOptions::builtIn()
{
    XdmcpOptions options;
    options.mode = QStringLiteral("query");
    options.program = QStringLiteral("auto");
    options.size = QStringLiteral("1280x800");
    return options;
}

RdpChoices xdmcpModeChoices()
{
    return {{QStringLiteral("query"), QStringLiteral("The host's own login screen (query)")},
            {QStringLiteral("indirect"), QStringLiteral("The host's chooser of login servers (indirect)")},
            {QStringLiteral("broadcast"), QStringLiteral("The first login server on the network to answer (broadcast)")}};
}

RdpChoices xdmcpProgramChoices()
{
#ifdef Q_OS_WIN
    return {{QStringLiteral("auto"), QStringLiteral("The first one found (VcXsrv, Xming)")},
            {QStringLiteral("vcxsrv"), QStringLiteral("VcXsrv")},
            {QStringLiteral("xming"), QStringLiteral("Xming")},
            {QStringLiteral("custom"), QStringLiteral("Another X server program…")}};
#else
    return {{QStringLiteral("auto"), QStringLiteral("The first one found (Xephyr, Xnest)")},
            {QStringLiteral("xephyr"), QStringLiteral("Xephyr")},
            {QStringLiteral("xnest"), QStringLiteral("Xnest")},
            {QStringLiteral("custom"), QStringLiteral("Another X server program…")}};
#endif
}

RdpChoices xdmcpSizeChoices()
{
    return {{QStringLiteral("1024x768"), QStringLiteral("1024 × 768")},
            {QStringLiteral("1280x800"), QStringLiteral("1280 × 800")},
            {QStringLiteral("1600x900"), QStringLiteral("1600 × 900")},
            {QStringLiteral("1920x1080"), QStringLiteral("1920 × 1080")},
            {QStringLiteral("fullscreen"), QStringLiteral("Full screen")}};
}

XdmcpOptions xdmcpOptionsFromYaml(const YAML::Node &node)
{
    XdmcpOptions options;
    if (!node || !node.IsMap())
        return options;
    try {
        if (node["mode"]) {
            const auto value = QString::fromStdString(node["mode"].as<std::string>());
            for (const auto &choice : xdmcpModeChoices())
                if (choice.first == value)
                    options.mode = value;
        }
        // A program is a known choice or a path (for "custom").
        if (node["program"])
            options.program = QString::fromStdString(node["program"].as<std::string>()).trimmed();
        if (node["size"]) {
            const auto value = QString::fromStdString(node["size"].as<std::string>()).trimmed();
            static const QRegularExpression size(QStringLiteral("^\\d{3,5}x\\d{3,5}$"));
            if (value == QStringLiteral("fullscreen") || size.match(value).hasMatch())
                options.size = value;
        }
    } catch (const YAML::Exception &) {
        // Keep what was read; invalid values use the defaults.
    }
    return options;
}

void xdmcpOptionsToYaml(const XdmcpOptions &options, YAML::Node &node)
{
    const auto text = [&node](const char *key, const QString &value) {
        if (!value.isEmpty())
            node[key] = value.toStdString();
    };
    text("mode", options.mode);
    text("program", options.program);
    text("size", options.size);
}

XdmcpOptions loadXdmcpDefaults()
{
    try {
        return XdmcpOptions::builtIn().overriddenBy(
            xdmcpOptionsFromYaml(YAML::LoadFile(settingsPath().toStdString())["xdmcp"]));
    } catch (const YAML::Exception &) {
        return XdmcpOptions::builtIn();
    }
}

void saveXdmcpDefaults(const XdmcpOptions &options)
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
    xdmcpOptionsToYaml(options, node);
    settings["xdmcp"] = node;
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

XServerProgram findXServer(const QString &choice)
{
    XServerProgram found;
    const auto known = [](const QString &kind) {
#ifdef Q_OS_WIN
        return windowsXServer(kind);
#else
        return QStandardPaths::findExecutable(kind == QStringLiteral("xephyr") ? QStringLiteral("Xephyr")
                                                                              : QStringLiteral("Xnest"));
#endif
    };
#ifdef Q_OS_WIN
    const QStringList automatic{QStringLiteral("vcxsrv"), QStringLiteral("xming")};
#else
    const QStringList automatic{QStringLiteral("xephyr"), QStringLiteral("xnest")};
#endif
    if (choice.isEmpty() || choice == QStringLiteral("auto")) {
        for (const auto &kind : automatic) {
            if (const auto path = known(kind); !path.isEmpty())
                return {path, kind, {}};
        }
    } else if (automatic.contains(choice)) {
        if (const auto path = known(choice); !path.isEmpty())
            return {path, choice, {}};
    } else {
        // A path chosen by the user ("custom" without one has none).
        if (choice != QStringLiteral("custom") && QFileInfo(choice).isExecutable())
            return {choice, QStringLiteral("custom"), {}};
        found.problem = choice == QStringLiteral("custom")
                            ? QStringLiteral("Choose the X server program.")
                            : QStringLiteral("%1 is not a program that can be run.").arg(choice);
        return found;
    }
#ifdef Q_OS_WIN
    found.problem = QStringLiteral(
        "XDMCP shows the login screen in an X server, which Windows does not have. Install VcXsrv "
        "(free, sourceforge.net/projects/vcxsrv) or Xming, or choose another X server program.");
#else
    found.problem = QStringLiteral(
        "XDMCP shows the login screen in a nested X server: install Xephyr (the \"xserver-xephyr\" or "
        "\"xorg-server-xephyr\" package) or Xnest, or choose another X server program.");
#endif
    return found;
}

QStringList xServerArguments(const XServerProgram &program, const XdmcpOptions &options, int display,
                             const QString &host, quint16 port, const QString &title)
{
    QStringList arguments{QStringLiteral(":%1").arg(display)};
    if (options.mode == QStringLiteral("broadcast"))
        arguments << QStringLiteral("-broadcast");
    else
        arguments << (options.mode == QStringLiteral("indirect") ? QStringLiteral("-indirect")
                                                                 : QStringLiteral("-query"))
                  << host;
    if (port != 0 && port != 177)
        arguments << QStringLiteral("-port") << QString::number(port);
    // One login session, then the X server ends (and the tab says so).
    arguments << QStringLiteral("-once");
    // The display manager draws its login screen over TCP; current X.Org
    // servers do not listen there unless asked (the Windows ones do).
    if (program.kind == QStringLiteral("xephyr") || program.kind == QStringLiteral("xnest"))
        arguments << QStringLiteral("-listen") << QStringLiteral("tcp");
    const bool full = options.size == QStringLiteral("fullscreen");
    QString size = options.size;
    if (full) {
        const auto *screen = QGuiApplication::primaryScreen();
        const QSize pixels = screen ? screen->size() * screen->devicePixelRatio() : QSize(1920, 1080);
        size = QStringLiteral("%1x%2").arg(pixels.width()).arg(pixels.height());
    }
    if (program.kind == QStringLiteral("xephyr")) {
        arguments << QStringLiteral("-title") << title;
        if (full)
            arguments << QStringLiteral("-fullscreen");
        else
            arguments << QStringLiteral("-screen") << size << QStringLiteral("-resizeable");
    } else if (program.kind == QStringLiteral("xnest")) {
        arguments << QStringLiteral("-name") << title << QStringLiteral("-geometry") << size;
    } else if (program.kind == QStringLiteral("vcxsrv") || program.kind == QStringLiteral("xming")) {
        if (full)
            arguments << QStringLiteral("-fullscreen");
        else
            arguments << QStringLiteral("-screen") << QStringLiteral("0") << size;
    }
    // Another program gets only the standard X server arguments.
    return arguments;
}

XdmcpOptionsEditor::XdmcpOptionsEditor(const XdmcpOptions &value, const XdmcpOptions &defaults, QWidget *parent)
    : QWidget(parent), m_defaults(XdmcpOptions::builtIn().overriddenBy(defaults))
{
    const auto makeBox = [this](const RdpChoices &choices, const QString &current, const QString &defaultValue) {
        auto *box = new QComboBox(this);
        box->addItem(QStringLiteral("Default (%1)").arg(labelFor(choices, defaultValue)), QString());
        for (const auto &choice : choices)
            box->addItem(choice.second, choice.first);
        box->setCurrentIndex(qMax(0, box->findData(current)));
        return box;
    };
    m_mode = makeBox(xdmcpModeChoices(), value.mode, m_defaults.mode);
    // A path is stored as the program itself; shown as "Another…" with it.
    const bool path = !value.program.isEmpty() && value.program != QStringLiteral("custom")
                      && labelFor(xdmcpProgramChoices(), value.program) == value.program;
    // A default that is a path shows as the path.
    m_program = makeBox(xdmcpProgramChoices(), path ? QStringLiteral("custom") : value.program, m_defaults.program);
    m_path = new QLineEdit(path ? value.program : QString(), this);
    m_path->setPlaceholderText(QStringLiteral("Path of the X server program"));
    m_size = makeBox(xdmcpSizeChoices(), value.size, m_defaults.size);
    m_note = new AvailabilityNote(
        [this] {
            QString choice = m_program->currentData().toString();
            if (choice.isEmpty())
                choice = m_defaults.program;
            if (choice == QStringLiteral("custom"))
                choice = m_path->text().trimmed().isEmpty() ? QStringLiteral("custom") : m_path->text().trimmed();
            return findXServer(choice).problem;
        },
        {}, this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("xdmcpTabs"));
    layout->addWidget(tabs);
    auto *page = new QWidget(tabs);
    tabs->addTab(page, QStringLiteral("Display"));
    auto *form = new QFormLayout(page);
    form->addRow(QStringLiteral("Show:"), m_mode);
    form->addRow(QStringLiteral("X server program:"), m_program);
    form->addRow(QString(), m_path);
    form->addRow(QString(), m_note);
    form->addRow(QStringLiteral("Size:"), m_size);
    auto *help = new QLabel(QStringLiteral(
        "The login screen opens in a window of the X server program. XDMCP is not encrypted; the "
        "host's display manager must allow it (for example XDMCP Enable=true in GDM or LightDM), and "
        "this computer's firewall must let the host connect back to TCP port 6000 + the display "
        "number (6010 and up)."),
        page);
    help->setWordWrap(true);
    help->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(help);
    connect(m_program, &QComboBox::currentIndexChanged, this, &XdmcpOptionsEditor::updateCustom);
    connect(m_path, &QLineEdit::editingFinished, m_note, &AvailabilityNote::check);
    updateCustom();
}

void XdmcpOptionsEditor::updateCustom()
{
    m_path->setVisible(m_program->currentData().toString() == QStringLiteral("custom"));
    m_note->check();
}

XdmcpOptions XdmcpOptionsEditor::value() const
{
    XdmcpOptions options;
    options.mode = m_mode->currentData().toString();
    options.program = m_program->currentData().toString();
    if (options.program == QStringLiteral("custom") && !m_path->text().trimmed().isEmpty())
        options.program = m_path->text().trimmed();
    options.size = m_size->currentData().toString();
    return options;
}

// ---- The tab ----

XdmcpPage::XdmcpPage(const QString &name, const QString &host, quint16 port, const XdmcpOptions &options,
                     QWidget *parent)
    : QWidget(parent), m_name(name), m_host(host), m_port(port), m_options(options)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto *title = new QLabel(QStringLiteral("<b>XDMCP: %1</b>").arg(name.toHtmlEscaped()), this);
    m_state = new QLabel(this);
    m_state->setWordWrap(true);
    m_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *buttons = new QHBoxLayout;
    m_restart = new QPushButton(QStringLiteral("Start Again"), this);
    m_stop = new QPushButton(QStringLiteral("Stop"), this);
    buttons->addWidget(m_restart);
    buttons->addWidget(m_stop);
    buttons->addStretch();
    auto *logTitle = new QLabel(QStringLiteral("Messages of the X server:"), this);
    logTitle->setForegroundRole(QPalette::PlaceholderText);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(title);
    layout->addWidget(m_state);
    layout->addLayout(buttons);
    layout->addWidget(logTitle);
    layout->addWidget(m_log, 1);
    connect(m_restart, &QPushButton::clicked, this, &XdmcpPage::start);
    connect(m_stop, &QPushButton::clicked, this, &XdmcpPage::stop);
    start();
}

XdmcpPage::~XdmcpPage()
{
    stop();
}

void XdmcpPage::setState(const QString &text)
{
    m_state->setText(text);
}

void XdmcpPage::start()
{
    stop();
    const auto program = findXServer(m_options.program);
    if (!program.problem.isEmpty()) {
        setState(program.problem);
        m_restart->setText(QStringLiteral("Check Again"));
        m_stop->setEnabled(false);
        return;
    }
    m_restart->setText(QStringLiteral("Start Again"));
    m_display = freeDisplay();
    const QString target = m_options.mode == QStringLiteral("broadcast") ? QStringLiteral("the network")
                                                                         : m_host;
    const auto arguments = xServerArguments(program, m_options, m_display, m_host, m_port,
                                            QStringLiteral("%1 (XDMCP)").arg(m_name));
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, [this] {
        m_log->appendPlainText(QString::fromLocal8Bit(m_process->readAll()).trimmed());
    });
    connect(m_process, &QProcess::errorOccurred, this, [this, program](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            setState(QStringLiteral("%1 could not be started: %2").arg(program.path, m_process->errorString()));
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        setState(status == QProcess::CrashExit
                     ? QStringLiteral("The X server stopped unexpectedly. Its messages are below.")
                 : code == 0 ? QStringLiteral("The session ended, and the X server with it.")
                             : QStringLiteral("The X server ended (code %1). Its messages below may say why; "
                                              "often the host does not answer XDMCP.").arg(code));
        m_stop->setEnabled(false);
    });
    m_log->appendPlainText(QStringLiteral("$ %1 %2").arg(QDir::toNativeSeparators(program.path), arguments.join(QLatin1Char(' '))));
    m_process->start(program.path, arguments);
    m_stop->setEnabled(true);
    setState(QStringLiteral("%1 shows the login of %2 in its own window (display :%3). Log out there, "
                            "close that window or press Stop to end.\n\nIf that window stays black, the host "
                            "answered but cannot connect back: it draws the login screen over TCP port %4 of "
                            "this computer, which this computer's firewall must allow from the host.")
                 .arg(QFileInfo(program.path).completeBaseName(), target)
                 .arg(m_display)
                 .arg(6000 + m_display));
}

void XdmcpPage::stop()
{
    if (!m_process)
        return;
    auto *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(2000)) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    process->deleteLater();
    m_stop->setEnabled(false);
    setState(QStringLiteral("Stopped."));
}
