// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "localsessions.h"
#include "filedialogs.h"
#include "formcard.h"
#include "inheritance.h"
#include "jumphost.h"
#include "availabilitynote.h"
#include "preferences.h"
#include "sshsession.h"
#include "statusline.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QListView>
#include <QTemporaryFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <yaml-cpp/yaml.h>

namespace {
const QString Local = QStringLiteral("Local");
const QString LocalAdministrator = QStringLiteral("Local (Administrator)");
const QString Wsl = QStringLiteral("WSL");
const QString Docker = QStringLiteral("Container (Docker)");
const QString Kubernetes = QStringLiteral("Container (Kubernetes)");
// In kubectl's arguments: the kubeconfig written on the SSH server.
const QString RemoteKubeconfig = QStringLiteral("@OPSREMOTE-KUBECONFIG@");

// bash when the container has it, else sh.
const QString AnyShell = QStringLiteral("command -v bash >/dev/null 2>&1 && exec bash || exec sh");

QString shellQuote(const QString &text)
{
    return QLatin1Char('\'') + QString(text).replace(QLatin1Char('\''), QStringLiteral("'\\''"))
           + QLatin1Char('\'');
}

// The shell to start in a container, as arguments.
QStringList shellArguments(const QString &own, const QString &fallback)
{
    const auto shell = own.isEmpty() ? fallback : own;
    if (shell.isEmpty())
        return {QStringLiteral("sh"), QStringLiteral("-c"), AnyShell};
    return QProcess::splitCommand(shell);
}

QString remoteLine(const QString &program, const QStringList &arguments)
{
    QStringList parts{program};
    for (const auto &argument : arguments)
        parts << shellQuote(argument);
    return parts.join(QLatin1Char(' '));
}

QString text(const YAML::Node &node, const char *key)
{
    try {
        if (node[key])
            return QString::fromStdString(node[key].as<std::string>());
    } catch (const YAML::Exception &) {
    }
    return {};
}

// Lines of a program's output; WSL writes UTF-16.
QStringList outputLines(const QByteArray &output)
{
    QString text;
    if (output.contains('\0'))
        text = QString::fromUtf16(reinterpret_cast<const char16_t *>(output.constData()), output.size() / 2);
    else
        text = QString::fromUtf8(output);
    QStringList lines;
    for (auto line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed().remove(QChar(0xfeff));
        // kubectl -o name: "pod/web-1", "namespace/default".
        if (line.contains(QLatin1Char('/')) && !line.contains(QLatin1Char(' ')))
            line = line.section(QLatin1Char('/'), 1);
        if (!line.isEmpty())
            lines << line;
    }
    return lines;
}

// A path field with Browse… (local files only; on an SSH server, typed).
QWidget *pathField(QLineEdit **edit, const QString &value, const QString &placeholder, QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *row = new QHBoxLayout(widget);
    row->setContentsMargins(0, 0, 0, 0);
    *edit = new QLineEdit(value, widget);
    (*edit)->setPlaceholderText(placeholder);
    auto *browse = new QPushButton(QStringLiteral("Browse…"), widget);
    row->addWidget(*edit, 1);
    row->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, widget, [edit = *edit] {
        const auto file = FileDialogs::openFileName(edit->window(), QString(),
                                                       edit->text().isEmpty() ? QDir::homePath() : edit->text());
        if (!file.isEmpty())
            edit->setText(QDir::toNativeSeparators(file));
    });
    return widget;
}

// A kubeconfig for the token and certificate log-ons (or a saved
// credential's, given as token); empty for a kubeconfig file.
QString kubeconfigText(const LocalSessionOptions &options, const QString &token)
{
    if (options.kubeAuth == QStringLiteral("saved"))
        return token;
    if (options.kubeAuth != QStringLiteral("token") && options.kubeAuth != QStringLiteral("certificate"))
        return {};
    YAML::Node cluster(YAML::NodeType::Map);
    cluster["server"] = options.server.toStdString();
    if (options.insecure)
        cluster["insecure-skip-tls-verify"] = true;
    else if (!options.caFile.isEmpty())
        cluster["certificate-authority"] = QDir::fromNativeSeparators(options.caFile).toStdString();
    YAML::Node user(YAML::NodeType::Map);
    if (options.kubeAuth == QStringLiteral("token")) {
        user["token"] = token.toStdString();
    } else {
        user["client-certificate"] = QDir::fromNativeSeparators(options.clientCertificate).toStdString();
        user["client-key"] = QDir::fromNativeSeparators(options.clientKey).toStdString();
    }
    YAML::Node context(YAML::NodeType::Map);
    context["cluster"] = "opsremote";
    context["user"] = "opsremote";
    if (!options.kubeNamespace.isEmpty())
        context["namespace"] = options.kubeNamespace.toStdString();
    YAML::Node config(YAML::NodeType::Map);
    config["apiVersion"] = "v1";
    config["kind"] = "Config";
    YAML::Node clusterEntry, userEntry, contextEntry;
    clusterEntry["name"] = "opsremote";
    clusterEntry["cluster"] = cluster;
    userEntry["name"] = "opsremote";
    userEntry["user"] = user;
    contextEntry["name"] = "opsremote";
    contextEntry["context"] = context;
    config["clusters"].push_back(clusterEntry);
    config["users"].push_back(userEntry);
    config["contexts"].push_back(contextEntry);
    config["current-context"] = "opsremote";
    YAML::Emitter emitter;
    emitter << config;
    return QString::fromUtf8(emitter.c_str());
}

// kubectl's log-on arguments, before its command: --kubeconfig (a file, or
// the written one at kubeconfigPath) and --context.
QStringList kubectlLogOn(const LocalSessionOptions &options, const QString &kubeconfigPath)
{
    QStringList arguments;
    if (!kubeconfigPath.isEmpty()) {
        arguments << QStringLiteral("--kubeconfig") << kubeconfigPath;
        // A saved credential has contexts of its own.
        if (options.kubeAuth == QStringLiteral("saved") && !options.context.isEmpty())
            arguments << QStringLiteral("--context") << options.context;
        return arguments;
    }
    if (options.kubeAuth == QStringLiteral("file") && !options.kubeconfig.isEmpty())
        arguments << QStringLiteral("--kubeconfig") << QDir::fromNativeSeparators(options.kubeconfig);
    if (!options.context.isEmpty())
        arguments << QStringLiteral("--context") << options.context;
    return arguments;
}

// A private file here with the text; empty when it cannot be written.
QString writePrivateFile(const QString &text)
{
    QTemporaryFile file(QDir::temp().filePath(QStringLiteral("opsremote-kube-XXXXXX.yaml")));
    file.setAutoRemove(false);
    if (!file.open())
        return {};
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(text.toUtf8());
    file.close();
    return file.fileName();
}

// On an SSH server: writes the kubeconfig privately, runs the command
// (where RemoteKubeconfig stands for its path), and removes it.
QString withRemoteKubeconfig(const QString &kubeconfig, QString command)
{
    command.replace(QLatin1Char('\'') + RemoteKubeconfig + QLatin1Char('\''), QStringLiteral("\"$k\""));
    return QStringLiteral("umask 077; k=$(mktemp \"${TMPDIR:-/tmp}/opsremote-kube-XXXXXX\") || exit 1; "
                          "printf '%s' '%1' | base64 -d > \"$k\"; %2; s=$?; rm -f \"$k\"; exit $s")
        .arg(QString::fromLatin1(kubeconfig.toUtf8().toBase64()), command);
}

// docker's (or podman's) arguments for a context or a remote engine.
QStringList dockerLogOn(const LocalSessionOptions &options)
{
    const bool podman = options.engine == QStringLiteral("podman");
    QStringList arguments;
    if (options.via == QStringLiteral("@context") && !options.dockerContext.isEmpty()) {
        arguments << (podman ? QStringLiteral("--connection") : QStringLiteral("--context")) << options.dockerContext;
    } else if (options.via == QStringLiteral("@tcp") && !options.dockerHost.isEmpty()) {
        if (podman)
            return {QStringLiteral("--url"), options.dockerHost};
        arguments << QStringLiteral("-H") << options.dockerHost;
        if (!options.caFile.isEmpty())
            arguments << QStringLiteral("--tlsverify") << QStringLiteral("--tlscacert") << QDir::fromNativeSeparators(options.caFile);
        if (!options.clientCertificate.isEmpty())
            arguments << QStringLiteral("--tlscert") << QDir::fromNativeSeparators(options.clientCertificate)
                      << QStringLiteral("--tlskey") << QDir::fromNativeSeparators(options.clientKey);
    }
    return arguments;
}

// The ↻ beside a list: lists again, showing errors in a message.
QPushButton *listAgainButton(QWidget *parent)
{
    auto *button = new QPushButton(QStringLiteral("↻"), parent);
    button->setToolTip(QStringLiteral("List again"));
    button->setFixedWidth(button->fontMetrics().horizontalAdvance(QStringLiteral("↻")) + 24);
    return button;
}

QWidget *rows(QFormLayout **form, QWidget *parent)
{
    auto *widget = new QWidget(parent);
    *form = new QFormLayout(widget);
    (*form)->setContentsMargins(0, 0, 0, 0);
    return widget;
}

QComboBox *editableList(QWidget *parent, const QString &value, const QString &placeholder)
{
    auto *combo = new QComboBox(parent);
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setEditText(value);
    combo->lineEdit()->setPlaceholderText(placeholder);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return combo;
}
}

QStringList localSessionProtocols()
{
#ifdef Q_OS_WIN
    return {Local, Wsl, Docker, Kubernetes};
#else
    return {Local, Docker, Kubernetes};
#endif
}

QString generatedKubeconfig(const LocalSessionOptions &options, const QString &token)
{
    return kubeconfigText(options, token);
}

const QString &dockerProtocol()
{
    return Docker;
}

const QString &kubernetesProtocol()
{
    return Kubernetes;
}

QString currentProtocolName(const QString &saved)
{
    if (saved == QStringLiteral("Docker"))
        return Docker;
    if (saved == QStringLiteral("Kubernetes"))
        return Kubernetes;
    return saved;
}

bool runsOverSsh(const LocalSessionOptions &options)
{
    return !options.via.isEmpty() && !options.via.startsWith(QLatin1Char('@'));
}

bool isLocalSessionProtocol(const QString &protocol)
{
    return protocol == Local || protocol == LocalAdministrator || protocol == Wsl || protocol == Docker
           || protocol == Kubernetes;
}

bool protocolNeedsHost(const QString &protocol)
{
    return !isLocalSessionProtocol(protocol);
}

LocalSessionOptions localSessionOptionsFromYaml(const YAML::Node &node)
{
    LocalSessionOptions options;
    if (!node || !node.IsMap())
        return options;
    options.localShell = text(node, "local_shell");
    options.program = text(node, "program");
    options.elevation = text(node, "elevation");
    try {
        options.administrator = node["administrator"] && node["administrator"].as<bool>();
    } catch (const YAML::Exception &) {
    }
    options.folder = text(node, "folder");
    options.distribution = text(node, "distribution");
    options.user = text(node, "user");
    options.engine = text(node, "engine");
    options.via = text(node, "via");
    options.container = text(node, "container");
    options.shell = text(node, "shell");
    options.context = text(node, "context");
    options.kubeNamespace = text(node, "namespace");
    options.pod = text(node, "pod");
    options.dockerContext = text(node, "docker_context");
    options.dockerHost = text(node, "docker_host");
    options.kubeAuth = text(node, "kube_auth");
    options.kubeCredential = text(node, "kube_credential");
    options.kubeconfig = text(node, "kubeconfig");
    options.server = text(node, "server");
    options.caFile = text(node, "ca_file");
    try {
        options.insecure = node["insecure"] && node["insecure"].as<bool>();
    } catch (const YAML::Exception &) {
    }
    options.clientCertificate = text(node, "client_certificate");
    options.clientKey = text(node, "client_key");
    return options;
}

void localSessionOptionsToYaml(const LocalSessionOptions &options, YAML::Node &node)
{
    const auto put = [&node](const char *key, const QString &value) {
        if (!value.isEmpty())
            node[key] = value.toStdString();
    };
    put("local_shell", options.localShell);
    put("program", options.program);
    put("elevation", options.elevation);
    if (options.administrator)
        node["administrator"] = true;
    put("folder", options.folder);
    put("distribution", options.distribution);
    put("user", options.user);
    put("engine", options.engine);
    put("via", options.via);
    put("container", options.container);
    put("shell", options.shell);
    put("context", options.context);
    put("namespace", options.kubeNamespace);
    put("pod", options.pod);
    put("docker_context", options.dockerContext);
    put("docker_host", options.dockerHost);
    put("kube_auth", options.kubeAuth);
    put("kube_credential", options.kubeCredential);
    put("kubeconfig", options.kubeconfig);
    put("server", options.server);
    put("ca_file", options.caFile);
    if (options.insecure)
        node["insecure"] = true;
    put("client_certificate", options.clientCertificate);
    put("client_key", options.clientKey);
}

ContainerPreferences loadContainerPreferences()
{
    const auto node = loadPreferencesSection("containers");
    ContainerPreferences value;
    const auto flag = [&node](const char *key, bool &target) {
        try {
            if (node[key])
                target = node[key].as<bool>();
        } catch (const YAML::Exception &) {
        }
    };
    flag("docker_enabled", value.docker.enabled);
    flag("kubernetes_enabled", value.kubernetes.enabled);
    flag("wsl_enabled", value.wsl.enabled);
    value.docker.program = text(node, "docker");
    value.docker.shell = text(node, "docker_shell");
    value.podman = text(node, "podman");
    value.kubernetes.program = text(node, "kubectl");
    value.kubernetes.shell = text(node, "kubernetes_shell");
    value.wsl.distribution = text(node, "wsl_distribution");
    const auto seconds = [&node](const char *key, int &target) {
        try {
            if (node[key])
                target = qBound(3, node[key].as<int>(), 600);
        } catch (const YAML::Exception &) {
        }
    };
    seconds("docker_list_timeout", value.docker.listTimeout);
    seconds("kubernetes_list_timeout", value.kubernetes.listTimeout);
    seconds("wsl_list_timeout", value.wsl.listTimeout);
    return value;
}

void saveContainerPreferences(const ContainerPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["docker_enabled"] = value.docker.enabled;
    node["kubernetes_enabled"] = value.kubernetes.enabled;
    node["wsl_enabled"] = value.wsl.enabled;
    node["docker"] = value.docker.program.toStdString();
    node["docker_shell"] = value.docker.shell.toStdString();
    node["podman"] = value.podman.toStdString();
    node["kubectl"] = value.kubernetes.program.toStdString();
    node["kubernetes_shell"] = value.kubernetes.shell.toStdString();
    node["wsl_distribution"] = value.wsl.distribution.toStdString();
    node["docker_list_timeout"] = value.docker.listTimeout;
    node["kubernetes_list_timeout"] = value.kubernetes.listTimeout;
    node["wsl_list_timeout"] = value.wsl.listTimeout;
    savePreferencesSection("containers", node);
}

QString containerProgram(const QString &name)
{
    const auto preferences = loadContainerPreferences();
    const auto own = name == QStringLiteral("podman")    ? preferences.podman
                     : name == QStringLiteral("kubectl") ? preferences.kubernetes.program
                                                         : preferences.docker.program;
    if (!own.isEmpty())
        return QFileInfo(own).isExecutable() ? own : QString();
    return QStandardPaths::findExecutable(name);
}

QString programPathHint(const QString &name)
{
    if (const auto found = QStandardPaths::findExecutable(name); !found.isEmpty())
        return QStringLiteral("Empty: %1 (found on the PATH)").arg(QDir::toNativeSeparators(found));
#ifdef Q_OS_WIN
    static const QHash<QString, QString> usual{
        {QStringLiteral("docker"), QStringLiteral("C:\\Program Files\\Docker\\Docker\\resources\\bin\\docker.exe")},
        {QStringLiteral("podman"), QStringLiteral("C:\\Program Files\\RedHat\\Podman\\podman.exe")},
        {QStringLiteral("kubectl"), QStringLiteral("C:\\Program Files\\Docker\\Docker\\resources\\bin\\kubectl.exe")},
        {QStringLiteral("vcxsrv"), QStringLiteral("C:\\Program Files\\VcXsrv\\vcxsrv.exe")}};
    return QStringLiteral("e.g. %1").arg(usual.value(name, QStringLiteral("C:\\Program Files\\%1\\%1.exe").arg(name)));
#else
    return QStringLiteral("e.g. /usr/bin/%1").arg(name);
#endif
}

QString localSessionProblem(const QString &protocol, const QString &engine)
{
    const auto preferences = loadContainerPreferences();
#ifndef Q_OS_WIN
    if (protocol == Wsl)
        return QStringLiteral("WSL sessions are for Windows.");
#else
    if (protocol == Wsl && !preferences.wsl.enabled)
        return QStringLiteral("WSL sessions are turned off (Preferences → Connections → WSL).");
    if (protocol == Wsl && QStandardPaths::findExecutable(QStringLiteral("wsl")).isEmpty())
        return QStringLiteral("WSL is not installed on this computer (wsl --install, as administrator).");
#endif
    if (protocol == Docker || protocol == Kubernetes) {
        if (!(protocol == Docker ? preferences.docker.enabled : preferences.kubernetes.enabled))
            return QStringLiteral("%1 sessions are turned off (Preferences → Connections → %2).")
                .arg(protocol, protocol == Docker ? QStringLiteral("Container (Docker)") : QStringLiteral("Container (Kubernetes)"));
        const auto program = protocol == Kubernetes ? QStringLiteral("kubectl")
                             : engine.isEmpty()     ? QStringLiteral("docker")
                                                    : engine;
        if (containerProgram(program).isEmpty())
            return QStringLiteral("%1 is not installed on this computer, or not on the PATH (set it in "
                                  "Preferences → Connections → %2). Choose an SSH server under "
                                  "\"Run on\" to use the server's instead.")
                .arg(program, protocol);
    }
    return {};
}

LocalCommand dockerCommand(const LocalSessionOptions &options, const ContainerPreferences &preferences)
{
    const auto engine = options.engine.isEmpty() ? QStringLiteral("docker") : options.engine;
    QStringList arguments = dockerLogOn(options);
    arguments << QStringLiteral("exec") << QStringLiteral("-it");
    if (!options.user.isEmpty())
        arguments << QStringLiteral("-u") << options.user;
    if (!options.folder.isEmpty())
        arguments << QStringLiteral("-w") << options.folder;
    arguments << options.container;
    arguments << shellArguments(options.shell, preferences.docker.shell);
    LocalCommand command;
    if (runsOverSsh(options)) {
        command.remoteCommand = remoteLine(engine, arguments);
        return command;
    }
    command.program = containerProgram(engine);
    command.arguments = arguments;
    return command;
}

LocalCommand kubernetesCommand(const LocalSessionOptions &options, const ContainerPreferences &preferences,
                               const QString &secret)
{
    LocalCommand command;
    const auto kubeconfig = kubeconfigText(options, secret);
    QString written;
    if (!kubeconfig.isEmpty())
        written = runsOverSsh(options) ? RemoteKubeconfig : (command.temporaryFile = writePrivateFile(kubeconfig));
    QStringList arguments = kubectlLogOn(options, written);
    if (!options.kubeNamespace.isEmpty())
        arguments << QStringLiteral("-n") << options.kubeNamespace;
    arguments << QStringLiteral("exec") << QStringLiteral("-it") << options.pod;
    if (!options.container.isEmpty())
        arguments << QStringLiteral("-c") << options.container;
    arguments << QStringLiteral("--") << shellArguments(options.shell, preferences.kubernetes.shell);
    if (runsOverSsh(options)) {
        command.remoteCommand = remoteLine(QStringLiteral("kubectl"), arguments);
        if (!kubeconfig.isEmpty())
            command.remoteCommand = withRemoteKubeconfig(kubeconfig, command.remoteCommand);
        return command;
    }
    command.program = containerProgram(QStringLiteral("kubectl"));
    command.arguments = arguments;
    return command;
}

LocalCommand wslCommand(const LocalSessionOptions &options)
{
    LocalCommand command;
    command.program = QStandardPaths::findExecutable(QStringLiteral("wsl"));
    const auto distribution = options.distribution.isEmpty() ? loadContainerPreferences().wsl.distribution
                                                             : options.distribution;
    if (!distribution.isEmpty())
        command.arguments << QStringLiteral("-d") << distribution;
    if (!options.user.isEmpty())
        command.arguments << QStringLiteral("-u") << options.user;
    command.arguments << QStringLiteral("--cd") << (options.folder.isEmpty() ? QStringLiteral("~") : options.folder);
    return command;
}

QList<QPair<QString, QString>> localShellChoices()
{
    QList<QPair<QString, QString>> choices;
#ifdef Q_OS_WIN
    choices.append({QString(), QStringLiteral("Windows PowerShell")});
    if (!localShellCommand(QStringLiteral("pwsh")).program.isEmpty())
        choices.append({QStringLiteral("pwsh"), QStringLiteral("PowerShell 7")});
    choices.append({QStringLiteral("cmd"), QStringLiteral("Command Prompt")});
    if (!localShellCommand(QStringLiteral("git-bash")).program.isEmpty())
        choices.append({QStringLiteral("git-bash"), QStringLiteral("Git Bash")});
#else
    const auto login = QFileInfo(qEnvironmentVariable("SHELL", QStringLiteral("/bin/sh"))).fileName();
    choices.append({QString(), QStringLiteral("My login shell (%1)").arg(login)});
    for (const auto *shell : {"bash", "zsh", "fish", "sh"}) {
        const auto name = QString::fromLatin1(shell);
        if (name != login && !QStandardPaths::findExecutable(name).isEmpty())
            choices.append({name, name});
    }
#endif
    choices.append({QStringLiteral("custom"), QStringLiteral("Another program")});
    return choices;
}

LocalShellCommand localShellCommand(const QString &choice, const QString &customProgram)
{
    if (choice == QStringLiteral("custom") && !customProgram.trimmed().isEmpty()) {
        auto parts = QProcess::splitCommand(customProgram);
        const auto program = parts.takeFirst();
        return {program, parts, QFileInfo(program).completeBaseName()};
    }
#ifdef Q_OS_WIN
    const QString system = QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")));
    if (choice == QStringLiteral("pwsh")) {
        QString pwsh = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
        if (pwsh.isEmpty() && QFile::exists(QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe")))
            pwsh = QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe");
        return {pwsh, {QStringLiteral("-NoLogo")}, QStringLiteral("PowerShell 7")};
    }
    if (choice == QStringLiteral("cmd"))
        return {qEnvironmentVariable("ComSpec", system + QStringLiteral("/System32/cmd.exe")), {},
                QStringLiteral("Command Prompt")};
    if (choice == QStringLiteral("git-bash")) {
        for (const auto &path : {QStringLiteral("C:/Program Files/Git/bin/bash.exe"),
                                 QStringLiteral("C:/Program Files (x86)/Git/bin/bash.exe")}) {
            if (QFile::exists(path))
                return {path, {QStringLiteral("--login"), QStringLiteral("-i")}, QStringLiteral("Git Bash")};
        }
        return {QString(), {}, QStringLiteral("Git Bash")};
    }
    return {system + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe"),
            {QStringLiteral("-NoLogo")}, QStringLiteral("PowerShell")};
#else
    if (!choice.isEmpty() && choice != QStringLiteral("custom")) {
        const auto program = QStandardPaths::findExecutable(choice);
        if (!program.isEmpty())
            return {program, {QStringLiteral("-l")}, choice};
    }
    return {QString(), {}, QStringLiteral("Local Terminal")};
#endif
}

QList<QPair<QString, QString>> elevationChoices()
{
#ifdef Q_OS_WIN
    return {{QStringLiteral("sudo"), QStringLiteral("Windows sudo when it is turned on, else OpsRemote's helper")},
            {QStringLiteral("helper"), QStringLiteral("OpsRemote's elevated helper (UAC)")}};
#else
    return {{QStringLiteral("sudo"), QStringLiteral("sudo (asks for the password in the terminal)")},
            {QStringLiteral("pkexec"), QStringLiteral("pkexec (asks in a system window)")}};
#endif
}

LocalSessionEditor::LocalSessionEditor(const LocalSessionOptions &value, SessionChoices sessions,
                                       Resolver resolver, QWidget *parent)
    : QWidget(parent), m_resolver(std::move(resolver))
{
    auto *page = new QVBoxLayout(this);
    page->setSpacing(6);
    // The rows of each card, in groups shown by type (setProtocol()); the
    // cards as in every session type: General, Logon, then Start.
    auto *general = new QWidget(this);
    auto *logOn = new QWidget(this);
    auto *start = new QWidget(this);
    for (auto *group : {general, logOn, start}) {
        auto *inside = new QVBoxLayout(group);
        inside->setContentsMargins(0, 0, 0, 0);
        inside->setSpacing(4);
    }
    auto *layout = qobject_cast<QVBoxLayout *>(general->layout());
    QFormLayout *form = nullptr;
    // The line under a list, kept in its form (see StatusLine).
    const auto addStatus = [this](QFormLayout *rowsForm, QComboBox *combo) {
        auto *line = new StatusLine(combo->parentWidget());
        rowsForm->addRow(QString(), line);
        m_statusLines.insert(combo, line);
    };

    m_localRows = rows(&form, this);
    m_localShell = new QComboBox(m_localRows);
    for (const auto &[id, label] : localShellChoices())
        m_localShell->addItem(label, id);
    if (m_localShell->findData(value.localShell) < 0)
        m_localShell->addItem(QStringLiteral("%1 (not found here)").arg(value.localShell), value.localShell);
    m_localShell->setCurrentIndex(qMax(0, m_localShell->findData(value.localShell)));
    form->addRow(QStringLiteral("Shell:"), m_localShell);
    m_program = new QLineEdit(value.program, m_localRows);
    m_program->setPlaceholderText(QStringLiteral("A program with its arguments"));
    m_program->setToolTip(QStringLiteral("e.g. htop, /usr/bin/fish -l, or "
                                         "\"C:\\msys64\\usr\\bin\\bash.exe\" --login."));
    form->addRow(QStringLiteral("Program:"), m_program);
    m_administrator = new QCheckBox(QStringLiteral("Run as administrator"), m_localRows);
#ifndef Q_OS_WIN
    m_administrator->setText(QStringLiteral("Run as root"));
#endif
    m_administrator->setChecked(value.administrator);
    form->addRow(QString(), m_administrator);
    m_elevation = new QComboBox(m_localRows);
    addInheritedItem(m_elevation, QStringLiteral("As in Preferences → Terminals"), QString());
    for (const auto &[id, label] : elevationChoices())
        m_elevation->addItem(label, id);
    m_elevation->setCurrentIndex(qMax(0, m_elevation->findData(value.elevation)));
    form->addRow(QStringLiteral("Run as administrator with:"), m_elevation);
    connect(m_administrator, &QCheckBox::toggled, this, [this, form](bool on) { form->setRowVisible(m_elevation, on); });
    const auto updateProgram = [this, form] {
        form->setRowVisible(m_program, m_localShell->currentData().toString() == QStringLiteral("custom"));
    };
    connect(m_localShell, &QComboBox::currentIndexChanged, this, updateProgram);
    updateProgram();
    layout->addWidget(m_localRows);

    m_wslRows = rows(&form, this);
    m_distribution = editableList(m_wslRows, value.distribution, QStringLiteral("Default distribution"));
    auto *listDistributions = listAgainButton(m_wslRows);
    auto *distributionRow = new QHBoxLayout;
    distributionRow->addWidget(m_distribution, 1);
    distributionRow->addWidget(listDistributions);
    form->addRow(QStringLiteral("Distribution:"), distributionRow);
    addStatus(form, m_distribution);
    connect(listDistributions, &QPushButton::clicked, this, [this] { list(m_distribution, true); });
    layout->addWidget(m_wslRows);

    m_dockerRows = rows(&form, this);
    m_engine = new QComboBox(m_dockerRows);
    m_engine->addItem(QStringLiteral("Docker"), QString());
    m_engine->addItem(QStringLiteral("Podman"), QStringLiteral("podman"));
    m_engine->setCurrentIndex(qMax(0, m_engine->findData(value.engine)));
    form->addRow(QStringLiteral("Engine:"), m_engine);
    layout->addWidget(m_dockerRows);

    // Shared by Docker and Kubernetes.
    m_containerRows = rows(&form, this);
    m_via = new QComboBox(m_containerRows);
    m_via->addItem(QStringLiteral("This computer"), QString());
    m_via->addItem(QStringLiteral("A Docker context"), QStringLiteral("@context"));
    m_via->addItem(QStringLiteral("A remote engine (TCP, TLS)"), QStringLiteral("@tcp"));
    for (const auto &[id, label] : sessions())
        m_via->addItem(QStringLiteral("SSH: %1").arg(label), id);
    if (!value.via.isEmpty() && m_via->findData(value.via) < 0)
        m_via->addItem(QStringLiteral("(a deleted session)"), value.via);
    m_via->setCurrentIndex(qMax(0, m_via->findData(value.via)));
    m_via->setToolTip(QStringLiteral("Where docker or kubectl runs: here, or on a saved SSH server "
                                     "(with its jump hosts and proxy). Docker can also reach an engine "
                                     "through a Docker context, or over TCP."));
    form->addRow(QStringLiteral("Run on:"), m_via);
    layout->addWidget(m_containerRows);

    // How docker and kubectl log on.
    layout = qobject_cast<QVBoxLayout *>(logOn->layout());
    m_logOnRows = rows(&form, this);
    m_dockerContext = editableList(m_logOnRows, value.dockerContext, QStringLiteral("Context name"));
    auto *listContexts = listAgainButton(m_logOnRows);
    connect(listContexts, &QPushButton::clicked, this, [this] { list(m_dockerContext, true); });
    auto *contextRow = new QHBoxLayout;
    contextRow->addWidget(m_dockerContext, 1);
    contextRow->addWidget(listContexts);
    form->addRow(QStringLiteral("Docker context:"), contextRow);
    addStatus(form, m_dockerContext);
    m_dockerHost = new QLineEdit(value.dockerHost, m_logOnRows);
    m_dockerHost->setPlaceholderText(QStringLiteral("tcp://host:2376"));
    form->addRow(QStringLiteral("Engine address:"), m_dockerHost);
    m_kubeAuth = new QComboBox(m_logOnRows);
    m_kubeAuth->addItem(QStringLiteral("The default kubeconfig (~/.kube/config, $KUBECONFIG)"), QString());
    m_kubeAuth->addItem(QStringLiteral("A saved credential (Tools → Kubernetes Credentials)"), QStringLiteral("saved"));
    m_kubeAuth->addItem(QStringLiteral("A kubeconfig file"), QStringLiteral("file"));
    m_kubeAuth->addItem(QStringLiteral("API server and token"), QStringLiteral("token"));
    m_kubeAuth->addItem(QStringLiteral("API server and client certificate"), QStringLiteral("certificate"));
    m_kubeAuth->setCurrentIndex(qMax(0, m_kubeAuth->findData(value.kubeAuth)));
    m_kubeAuth->setToolTip(QStringLiteral("Cloud clusters (EKS, GKE, AKS) and OIDC log on through "
                                          "their kubeconfig, which runs their login helper."));
    form->addRow(QStringLiteral("Log on with:"), m_kubeAuth);
    m_kubeCredential = new QComboBox(m_logOnRows);
    m_kubeCredential->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (!value.kubeCredential.isEmpty())
        m_kubeCredential->addItem(value.kubeCredential);
    auto *manage = new QPushButton(QStringLiteral("Manage…"), m_logOnRows);
    connect(manage, &QPushButton::clicked, this, &LocalSessionEditor::manageCredentialsRequested);
    auto *credentialRow = new QHBoxLayout;
    credentialRow->addWidget(m_kubeCredential, 1);
    credentialRow->addWidget(manage);
    form->addRow(QStringLiteral("Credential:"), credentialRow);
    form->addRow(QStringLiteral("Kubeconfig:"),
                 pathField(&m_kubeconfig, value.kubeconfig,
                           QStringLiteral("e.g. %1").arg(QDir::toNativeSeparators(QDir::home().filePath(QStringLiteral(".kube/config")))),
                           m_logOnRows));
    m_server = new QLineEdit(value.server, m_logOnRows);
    m_server->setPlaceholderText(QStringLiteral("https://api.example.com:6443"));
    form->addRow(QStringLiteral("API server:"), m_server);
    m_tokenNote = new QLabel(QStringLiteral("The token is the session's password above (kept in the "
                                            "vault or the system's store)."),
                             m_logOnRows);
    m_tokenNote->setWordWrap(true);
    m_tokenNote->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(QString(), m_tokenNote);
    form->addRow(QStringLiteral("CA certificate:"),
                 pathField(&m_caFile, value.caFile, QStringLiteral("The server's CA (PEM); empty: the system's"), m_logOnRows));
    m_insecure = new QCheckBox(QStringLiteral("Do not check the server's certificate (test clusters only)"), m_logOnRows);
    m_insecure->setChecked(value.insecure);
    form->addRow(QString(), m_insecure);
    form->addRow(QStringLiteral("Client certificate:"),
                 pathField(&m_clientCertificate, value.clientCertificate, QStringLiteral("PEM file"), m_logOnRows));
    form->addRow(QStringLiteral("Client key:"),
                 pathField(&m_clientKey, value.clientKey, QStringLiteral("PEM file"), m_logOnRows));
    layout->addWidget(m_logOnRows);
    connect(m_via, &QComboBox::currentIndexChanged, this, &LocalSessionEditor::updateRows);
    connect(m_kubeAuth, &QComboBox::currentIndexChanged, this, &LocalSessionEditor::updateRows);
    connect(m_insecure, &QCheckBox::toggled, this, &LocalSessionEditor::updateRows);

    layout = qobject_cast<QVBoxLayout *>(start->layout());
    m_kubernetesRows = rows(&form, this);
    m_context = editableList(m_kubernetesRows, value.context, QStringLiteral("The kubeconfig's current context"));
    m_namespace = editableList(m_kubernetesRows, value.kubeNamespace, QStringLiteral("The context's namespace"));
    m_pod = editableList(m_kubernetesRows, value.pod, QStringLiteral("Pod name"));
    const auto withList = [this](QComboBox *combo) {
        auto *button = listAgainButton(m_kubernetesRows);
        connect(button, &QPushButton::clicked, this, [this, combo] { list(combo, true); });
        auto *row = new QHBoxLayout;
        row->addWidget(combo, 1);
        row->addWidget(button);
        return row;
    };
    form->addRow(QStringLiteral("Context:"), withList(m_context));
    addStatus(form, m_context);
    form->addRow(QStringLiteral("Namespace:"), withList(m_namespace));
    addStatus(form, m_namespace);
    form->addRow(QStringLiteral("Pod:"), withList(m_pod));
    addStatus(form, m_pod);
    layout->addWidget(m_kubernetesRows);

    // The rest: container (Docker; Kubernetes: in the pod), user, folder, shell.
    auto *common = rows(&form, this);
    m_container = editableList(common, value.container, QStringLiteral("Container name or ID"));
    auto *listContainers = listAgainButton(common);
    connect(listContainers, &QPushButton::clicked, this, [this] { list(m_container, true); });
    auto *containerRow = new QHBoxLayout;
    containerRow->addWidget(m_container, 1);
    containerRow->addWidget(listContainers);
    form->addRow(QStringLiteral("Container:"), containerRow);
    addStatus(form, m_container);
    m_user = new QLineEdit(value.user, common);
    m_user->setPlaceholderText(QStringLiteral("As the distribution or image sets it"));
    form->addRow(QStringLiteral("User:"), m_user);
    m_folder = new QLineEdit(value.folder, common);
    m_folder->setPlaceholderText(QStringLiteral("As the distribution or image sets it"));
    form->addRow(QStringLiteral("Start folder:"), m_folder);
    m_shell = new QLineEdit(value.shell, common);
    m_shell->setPlaceholderText(QStringLiteral("bash if there is one, else sh"));
    form->addRow(QStringLiteral("Shell:"), m_shell);
    // A list's state shows in its own box (Listing…, none found, an error).
    m_listStatus = new QLabel(common);
    m_listStatus->hide();
    layout->addWidget(common);
    common->setObjectName(QStringLiteral("commonRows"));

    // A container program on an SSH server is the server's business.
    m_note = new AvailabilityNote([this] {
        return overSsh() ? QString() : localSessionProblem(m_protocol, m_engine->currentData().toString());
    }, {}, this);
    addWidgetCard(page, QStringLiteral("General"), general);
    m_logOnCard = addWidgetCard(page, QStringLiteral("Logon"), logOn);
    addWidgetCard(page, QStringLiteral("Start"), start);
    page->addWidget(m_note);
    page->addStretch();
    alignFormLabels(this);
    connect(m_via, &QComboBox::currentIndexChanged, m_note, &AvailabilityNote::check);
    connect(m_engine, &QComboBox::currentIndexChanged, m_note, &AvailabilityNote::check);

    // The lists fill themselves: when shown, when where or how they log on
    // changes, and down the chain context → namespace → pod → container.
    for (auto *combo : {m_via, m_engine, m_kubeAuth, m_kubeCredential})
        connect(combo, &QComboBox::currentIndexChanged, this, &LocalSessionEditor::scheduleLists);
    for (auto *edit : {m_kubeconfig, m_server, m_caFile, m_clientCertificate, m_clientKey, m_dockerHost})
        connect(edit, &QLineEdit::editingFinished, this, &LocalSessionEditor::scheduleLists);
    connect(m_insecure, &QCheckBox::toggled, this, &LocalSessionEditor::scheduleLists);
    connect(m_dockerContext, &QComboBox::currentTextChanged, this, [this] { scheduleList(m_container); });
    for (auto *combo : {m_context, m_namespace, m_pod})
        connect(combo, &QComboBox::currentTextChanged, this, [this, combo] { listDependents(combo); });
}

int LocalSessionEditor::listTimeout() const
{
    const auto preferences = loadContainerPreferences();
    if (m_protocol == Wsl)
        return preferences.wsl.listTimeout;
    return m_protocol == Kubernetes ? preferences.kubernetes.listTimeout : preferences.docker.listTimeout;
}

void LocalSessionEditor::scheduleList(QComboBox *target)
{
    auto *timer = m_listTimers.value(target);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(400);
        connect(timer, &QTimer::timeout, this, [this, target] { list(target, false); });
        m_listTimers.insert(target, timer);
    }
    timer->start();
}

// What the session type lists by itself.
void LocalSessionEditor::scheduleLists()
{
    if (m_protocol == Wsl) {
        scheduleList(m_distribution);
    } else if (m_protocol == Docker) {
        if (via() == QStringLiteral("@context"))
            scheduleList(m_dockerContext);
        scheduleList(m_container);
    } else if (m_protocol == Kubernetes) {
        const auto auth = m_kubeAuth->currentData().toString();
        if (auth != QStringLiteral("token") && auth != QStringLiteral("certificate"))
            scheduleList(m_context);
        scheduleList(m_namespace);
        scheduleList(m_pod);
        scheduleList(m_container);
    }
}

void LocalSessionEditor::listDependents(QComboBox *changed)
{
    if (m_protocol != Kubernetes)
        return;
    if (changed == m_context) {
        scheduleList(m_namespace);
        scheduleList(m_pod);
    }
    if (changed == m_context || changed == m_namespace)
        scheduleList(m_pod);
    if (changed != m_container)
        scheduleList(m_container);
}

void LocalSessionEditor::list(QComboBox *target, bool asked)
{
    if (target == m_distribution) {
        fillFrom(m_distribution, QStandardPaths::findExecutable(QStringLiteral("wsl")),
                 {QStringLiteral("-l"), QStringLiteral("-q")}, QString(), QStringLiteral("distributions"), asked);
    } else if (target == m_dockerContext) {
        const bool podman = m_engine->currentData().toString() == QStringLiteral("podman");
        fillFrom(m_dockerContext, containerProgram(podman ? QStringLiteral("podman") : QStringLiteral("docker")),
                 podman ? QStringList{QStringLiteral("system"), QStringLiteral("connection"), QStringLiteral("list"),
                                      QStringLiteral("--format"), QStringLiteral("{{.Name}}")}
                        : QStringList{QStringLiteral("context"), QStringLiteral("ls"), QStringLiteral("--format"),
                                      QStringLiteral("{{.Name}}")},
                 QString(), QStringLiteral("contexts"), asked);
    } else if (target == m_container) {
        refreshContainers(asked);
    } else if (target == m_context) {
        refreshKubernetes(m_context, {QStringLiteral("config"), QStringLiteral("get-contexts"),
                                      QStringLiteral("-o"), QStringLiteral("name")}, QStringLiteral("contexts"), asked);
    } else if (target == m_namespace) {
        refreshKubernetes(m_namespace, {QStringLiteral("get"), QStringLiteral("namespaces"),
                                        QStringLiteral("-o"), QStringLiteral("name")}, QStringLiteral("namespaces"), asked);
    } else if (target == m_pod) {
        QStringList arguments;
        if (!m_namespace->currentText().trimmed().isEmpty())
            arguments << QStringLiteral("-n") << m_namespace->currentText().trimmed();
        refreshKubernetes(m_pod, arguments + QStringList{QStringLiteral("get"), QStringLiteral("pods"),
                                                         QStringLiteral("-o"), QStringLiteral("name")},
                          QStringLiteral("pods"), asked);
    }
}

void LocalSessionEditor::setProtocol(const QString &protocol)
{
    m_protocol = protocol;
    const bool docker = protocol == Docker;
    const bool kubernetes = protocol == Kubernetes;
    m_localRows->setVisible(protocol == Local);
    if (auto *localForm = qobject_cast<QFormLayout *>(m_localRows->layout()))
        localForm->setRowVisible(m_elevation, m_administrator->isChecked());
    m_wslRows->setVisible(protocol == Wsl);
    m_dockerRows->setVisible(docker);
    m_containerRows->setVisible(docker || kubernetes);
    m_kubernetesRows->setVisible(kubernetes);
    auto *common = findChild<QWidget *>(QStringLiteral("commonRows"));
    common->setVisible(true);
    auto *form = qobject_cast<QFormLayout *>(common->layout());
    form->setRowVisible(0, docker || kubernetes); // Container and its status line
    form->setRowVisible(1, docker || kubernetes);
    form->setRowVisible(m_user, protocol == Wsl || docker);
    form->setRowVisible(m_folder, protocol != Kubernetes);
    form->setRowVisible(m_shell, docker || kubernetes);
    m_container->lineEdit()->setPlaceholderText(kubernetes ? QStringLiteral("The pod's first")
                                                           : QStringLiteral("Container name or ID"));
    updateRows();
    scheduleLists();
}

void LocalSessionEditor::setCredentialNames(std::function<QStringList()> names)
{
    m_credentialNames = std::move(names);
    const auto current = m_kubeCredential->currentText();
    m_kubeCredential->clear();
    m_kubeCredential->addItems(m_credentialNames ? m_credentialNames() : QStringList());
    if (!current.isEmpty() && m_kubeCredential->findText(current) < 0)
        m_kubeCredential->addItem(current);
    if (!current.isEmpty())
        m_kubeCredential->setCurrentText(current);
}

// The log-on rows of the chosen type and way.
void LocalSessionEditor::updateRows()
{
    const bool docker = m_protocol == Docker;
    const bool kubernetes = m_protocol == Kubernetes;
    // Contexts and TCP engines are Docker's.
    if (auto *view = qobject_cast<QListView *>(m_via->view())) {
        view->setRowHidden(m_via->findData(QStringLiteral("@context")), !docker);
        view->setRowHidden(m_via->findData(QStringLiteral("@tcp")), !docker);
    }
    if (!docker && via().startsWith(QLatin1Char('@')))
        m_via->setCurrentIndex(0);
    const auto where = via();
    const auto auth = m_kubeAuth->currentData().toString();
    const bool server = kubernetes && (auth == QStringLiteral("token") || auth == QStringLiteral("certificate"));
    const bool tcp = docker && where == QStringLiteral("@tcp");
    auto *form = qobject_cast<QFormLayout *>(m_logOnRows->layout());
    // Rows 0 (with its status line, 1) and 4 hold a list with a button beside it.
    form->setRowVisible(0, docker && where == QStringLiteral("@context")); // Docker context
    form->setRowVisible(1, docker && where == QStringLiteral("@context"));
    form->setRowVisible(m_dockerHost, tcp);
    form->setRowVisible(m_kubeAuth, kubernetes);
    form->setRowVisible(4, kubernetes && auth == QStringLiteral("saved")); // Credential
    form->setRowVisible(m_kubeconfig->parentWidget(), kubernetes && auth == QStringLiteral("file"));
    form->setRowVisible(m_server, server);
    form->setRowVisible(m_tokenNote, kubernetes && auth == QStringLiteral("token"));
    form->setRowVisible(m_caFile->parentWidget(), (server && !m_insecure->isChecked()) || tcp);
    form->setRowVisible(m_insecure, server);
    const bool client = (kubernetes && auth == QStringLiteral("certificate")) || tcp;
    form->setRowVisible(m_clientCertificate->parentWidget(), client);
    form->setRowVisible(m_clientKey->parentWidget(), client);
    m_logOnRows->setVisible(docker || kubernetes);
    // Docker on this computer or over SSH logs on by itself: no card then.
    m_logOnCard->setVisible(kubernetes || (docker && (tcp || where == QStringLiteral("@context"))));
    // A written kubeconfig (token, certificate) has a single context.
    if (auto *kubernetesForm = qobject_cast<QFormLayout *>(m_kubernetesRows->layout())) {
        kubernetesForm->setRowVisible(0, !server); // Context and its status line
        kubernetesForm->setRowVisible(1, !server);
    }
    m_note->check();
}

QString LocalSessionEditor::via() const
{
    return m_via->currentData().toString();
}

LocalSessionOptions LocalSessionEditor::value() const
{
    LocalSessionOptions options;
    const bool docker = m_protocol == Docker;
    const bool kubernetes = m_protocol == Kubernetes;
    if (m_protocol == Local || m_protocol == LocalAdministrator) {
        options.localShell = m_localShell->currentData().toString();
        if (options.localShell == QStringLiteral("custom"))
            options.program = m_program->text().trimmed();
        options.administrator = m_administrator->isChecked();
        if (options.administrator)
            options.elevation = m_elevation->currentData().toString();
    }
    if (m_protocol == Wsl)
        options.distribution = m_distribution->currentText().trimmed();
    if (m_protocol == Wsl || docker)
        options.user = m_user->text().trimmed();
    if (m_protocol != Kubernetes)
        options.folder = m_folder->text().trimmed();
    if (docker)
        options.engine = m_engine->currentData().toString();
    if (docker && via() == QStringLiteral("@context"))
        options.dockerContext = m_dockerContext->currentText().trimmed();
    if (docker && via() == QStringLiteral("@tcp")) {
        options.dockerHost = m_dockerHost->text().trimmed();
        options.caFile = m_caFile->text().trimmed();
        options.clientCertificate = m_clientCertificate->text().trimmed();
        options.clientKey = m_clientKey->text().trimmed();
    }
    if (kubernetes) {
        options.kubeAuth = m_kubeAuth->currentData().toString();
        if (options.kubeAuth == QStringLiteral("saved"))
            options.kubeCredential = m_kubeCredential->currentText().trimmed();
        if (options.kubeAuth == QStringLiteral("file"))
            options.kubeconfig = m_kubeconfig->text().trimmed();
        if (options.kubeAuth == QStringLiteral("token") || options.kubeAuth == QStringLiteral("certificate")) {
            options.server = m_server->text().trimmed();
            options.insecure = m_insecure->isChecked();
            if (!options.insecure)
                options.caFile = m_caFile->text().trimmed();
        }
        if (options.kubeAuth == QStringLiteral("certificate")) {
            options.clientCertificate = m_clientCertificate->text().trimmed();
            options.clientKey = m_clientKey->text().trimmed();
        }
    }
    if (docker || kubernetes) {
        options.via = via();
        options.container = m_container->currentText().trimmed();
        options.shell = m_shell->text().trimmed();
    }
    if (kubernetes) {
        options.context = m_context->currentText().trimmed();
        options.kubeNamespace = m_namespace->currentText().trimmed();
        options.pod = m_pod->currentText().trimmed();
    }
    return options;
}

void LocalSessionEditor::refreshContainers(bool asked)
{
    if (m_protocol == Kubernetes) {
        // The containers of the chosen pod.
        QStringList arguments;
        if (!m_context->currentText().trimmed().isEmpty())
            arguments << QStringLiteral("--context") << m_context->currentText().trimmed();
        if (!m_namespace->currentText().trimmed().isEmpty())
            arguments << QStringLiteral("-n") << m_namespace->currentText().trimmed();
        if (m_pod->currentText().trimmed().isEmpty())
            return;
        refreshKubernetes(m_container, arguments + QStringList{
                                           QStringLiteral("get"), QStringLiteral("pod"), m_pod->currentText().trimmed(),
                                           QStringLiteral("-o"), QStringLiteral("jsonpath={range .spec.containers[*]}{.name}{\"\\n\"}{end}")},
                          QStringLiteral("containers"), asked);
        return;
    }
    const auto engine = m_engine->currentData().toString().isEmpty() ? QStringLiteral("docker")
                                                                     : m_engine->currentData().toString();
    fillFrom(m_container, overSsh() ? engine : containerProgram(engine),
             dockerLogOn(value()) + QStringList{QStringLiteral("ps"), QStringLiteral("--format"), QStringLiteral("{{.Names}}")},
             QString(), QStringLiteral("running containers"), asked);
}

// kubectl with the session's log-on: its kubeconfig (or one written for a
// token, a certificate, or a saved credential) and context.
void LocalSessionEditor::refreshKubernetes(QComboBox *target, const QStringList &arguments, const QString &noun,
                                           bool asked)
{
    const auto options = value();
    const auto kubeconfig = kubeconfigText(options, m_secret ? m_secret(options) : QString());
    if (options.kubeAuth == QStringLiteral("saved") && kubeconfig.isEmpty()) {
        if (asked)
            QMessageBox::warning(this, QStringLiteral("List"),
                                 QStringLiteral("Choose a saved credential first (Manage… adds one)."));
        if (auto *line = m_statusLines.value(target))
            line->setStatus(QStringLiteral("Choose a saved credential first (Manage… adds one)."));
        return;
    }
    // Listing contexts needs no context.
    auto logOn = kubectlLogOn(options, kubeconfig.isEmpty() ? QString() : (overSsh() ? RemoteKubeconfig : QStringLiteral("@LOCAL@")));
    if (target == m_context) {
        const int at = logOn.indexOf(QStringLiteral("--context"));
        if (at >= 0)
            logOn.remove(at, 2);
    }
    fillFrom(target, overSsh() ? QStringLiteral("kubectl") : containerProgram(QStringLiteral("kubectl")),
             logOn + arguments, kubeconfig, noun, asked);
}

bool LocalSessionEditor::overSsh() const
{
    return !via().isEmpty() && !via().startsWith(QLatin1Char('@'));
}

void LocalSessionEditor::fillFrom(QComboBox *target, const QString &program, const QStringList &givenArguments,
                                  const QString &kubeconfig, const QString &noun, bool asked)
{
    auto arguments = givenArguments;
    const QPointer<QComboBox> combo(target);
    // The box's own hint, kept to show again after "Listing…".
    if (!combo->property("hint").isValid())
        combo->setProperty("hint", combo->lineEdit()->placeholderText());
    // A newer list for the same box wins.
    const int run = combo->property("listRun").toInt() + 1;
    combo->setProperty("listRun", run);
    const auto show = [this, combo, noun, asked, run](const QStringList &lines, const QString &problem) {
        if (!combo || combo->property("listRun").toInt() != run)
            return;
        const auto current = combo->currentText();
        {
            const QSignalBlocker blocker(combo);
            combo->clear();
            combo->addItems(lines);
            combo->setEditText(current.isEmpty() && !lines.isEmpty() ? lines.first() : current);
        }
        // Nothing to choose, or an error: greyed, the reason on the line
        // under it. typeable: listing is not possible from here, so the
        // name is typed.
        const bool typeable = problem.startsWith(QStringLiteral("Cannot list"));
        combo->setEnabled(!lines.isEmpty() || typeable);
        combo->lineEdit()->setPlaceholderText(combo->property("hint").toString());
        if (auto *line = m_statusLines.value(combo)) {
            if (!problem.isEmpty())
                line->setStatus(typeable ? problem : QStringLiteral("Could not list the %1: %2").arg(noun, problem), !typeable);
            else if (lines.isEmpty())
                line->setStatus(QStringLiteral("Not found."), true);
            else
                line->clearStatus();
        }
        if (combo->currentText() != current)
            listDependents(combo);
        if (!problem.isEmpty() && asked)
            QMessageBox::warning(this, QStringLiteral("List"), QStringLiteral("Could not list the %1:\n\n%2").arg(noun, problem));
    };
    // While listing: greyed, saying so inside.
    combo->setEnabled(false);
    combo->lineEdit()->setPlaceholderText(QStringLiteral("Retrieving info…"));
    if (auto *line = m_statusLines.value(combo))
        line->clearStatus();
    const int timeout = listTimeout();
    if (!overSsh()) {
        if (program.isEmpty()) {
            show({}, QStringLiteral("The program is not installed on this computer."));
            return;
        }
        // The written kubeconfig, private, removed when the program ends.
        QString written;
        if (!kubeconfig.isEmpty()) {
            written = writePrivateFile(kubeconfig);
            arguments.replaceInStrings(QStringLiteral("@LOCAL@"), written);
        }
        auto *process = new QProcess(this);
        if (!written.isEmpty())
            connect(process, &QObject::destroyed, [written] { QFile::remove(written); });
        connect(process, &QProcess::finished, this,
                [process, show, timeout, name = QFileInfo(program).fileName()](int exitCode, QProcess::ExitStatus status) {
            const auto output = process->readAllStandardOutput();
            auto error = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            if (status == QProcess::CrashExit)
                error = QStringLiteral("%1 did not answer within %2 seconds (Preferences → Connections: List "
                                       "timeout).").arg(name).arg(timeout);
            else if (exitCode != 0 && error.isEmpty())
                error = QStringLiteral("%1 ended with exit status %2.").arg(name).arg(exitCode);
            show(outputLines(output), exitCode == 0 && status == QProcess::NormalExit ? QString() : error.left(2000));
            process->deleteLater();
        });
        connect(process, &QProcess::errorOccurred, this, [process, show](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                show({}, process->errorString());
                process->deleteLater();
            }
        });
        QTimer::singleShot(timeout * 1000, process, [process] { process->kill(); });
        process->start(program, arguments);
        return;
    }
    const auto settings = m_resolver ? m_resolver(via()) : std::nullopt;
    if (!settings) {
        show({}, QStringLiteral("Cannot list on this server from here (it has jump hosts); type the name."));
        return;
    }
    auto connection = *settings;
    connection.openShell = false;
    auto *ssh = new SshSession(this);
    QString command = remoteLine(program, arguments);
    if (!kubeconfig.isEmpty())
        command = withRemoteKubeconfig(kubeconfig, command);
    connect(ssh, &SshSession::hostKeyVerificationRequired, ssh,
            [ssh] { ssh->answerHostKey(SshHostKeyDecision::Reject); });
    // A key's passphrase or a password is asked as everywhere else.
    const auto server = connection.username.isEmpty()
                            ? connection.host
                            : QStringLiteral("%1@%2").arg(connection.username, connection.host);
    connect(ssh, &SshSession::inputRequired, this,
            [this, ssh, server](const QString &instruction, const QStringList &prompts,
                                const QList<bool> &echo) {
                askSshLogOn(window(), ssh, server, instruction, prompts, echo);
            });
    connect(ssh, &SshSession::connected, ssh, [ssh, command, timeout] { ssh->runCommand(command, false, timeout, 256 * 1024); });
    connect(ssh, &SshSession::commandFinished, this,
            [ssh, show](int, int exitStatus, const QByteArray &output, bool, const QString &error, int) {
                QString problem;
                if (exitStatus != 0)
                    problem = !error.isEmpty() ? error
                              : !output.trimmed().isEmpty() ? QString::fromUtf8(output).trimmed().left(2000)
                                                            : QStringLiteral("The command ended with exit status %1 on the server.").arg(exitStatus);
                show(exitStatus == 0 ? outputLines(output) : QStringList(), problem);
                ssh->deleteLater();
            });
    connect(ssh, &SshSession::disconnected, this, [ssh, show](const QString &reason, bool error) {
        if (error)
            show({}, QStringLiteral("Could not connect: %1 (a new server's key is confirmed by opening it once).").arg(reason));
        ssh->deleteLater();
    });
    ssh->connectToHost(connection);
}
