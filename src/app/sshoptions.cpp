// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sshoptions.h"
#include "availabilitynote.h"
#include "localresources.h"
#include "sshauth.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include <yaml-cpp/yaml.h>

SshPreferences SshOptions::appliedTo(SshPreferences preferences) const
{
    if (connectTimeout > 0)
        preferences.connectTimeout = connectTimeout;
    if (keepaliveSeconds >= 0)
        preferences.keepaliveSeconds = keepaliveSeconds;
    if (compression >= 0)
        preferences.compression = compression == 1;
    if (tryPublicKeys >= 0)
        preferences.tryPublicKeys = tryPublicKeys == 1;
    if (forwardAgent >= 0)
        preferences.forwardAgent = forwardAgent == 1;
    if (followTerminalFolder >= 0)
        preferences.followTerminalFolder = followTerminalFolder == 1;
    if (forwardX11 >= 0)
        preferences.forwardX11 = forwardX11 == 1;
    if (!fileTransfer.isEmpty())
        preferences.fileTransfer = fileTransfer;
    if (!identityFile.isEmpty())
        preferences.identityFile = identityFile;
    if (kerberos >= 0)
        preferences.kerberos = kerberos == 1;
    if (kerberosDelegate >= 0)
        preferences.kerberosDelegate = kerberosDelegate == 1;
    if (autoReconnect >= 0)
        preferences.autoReconnect = autoReconnect == 1;
    if (reconnectDelay > 0)
        preferences.reconnectDelay = reconnectDelay;
    if (reconnectAttempts >= 0)
        preferences.reconnectAttempts = reconnectAttempts;
    if (!terminalType.isEmpty())
        preferences.terminalType = terminalType;
    return preferences;
}

SshOptions sshOptionsFromYaml(const YAML::Node &node)
{
    SshOptions options;
    if (!node || !node.IsMap())
        return options;
    try {
        const auto number = [&node](const char *key, int &target, int low, int high) {
            if (node[key])
                target = qBound(low, node[key].as<int>(), high);
        };
        const auto flag = [&node](const char *key, int &target) {
            if (node[key])
                target = node[key].as<bool>() ? 1 : 0;
        };
        const auto text = [&node](const char *key, QString &target) {
            if (node[key])
                target = QString::fromStdString(node[key].as<std::string>()).trimmed().left(1024);
        };
        number("connect_timeout", options.connectTimeout, 1, 600);
        number("keepalive_seconds", options.keepaliveSeconds, 0, 3600);
        flag("compression", options.compression);
        flag("try_public_keys", options.tryPublicKeys);
        flag("forward_agent", options.forwardAgent);
        flag("follow_terminal_folder", options.followTerminalFolder);
        flag("forward_x11", options.forwardX11);
        text("identity_file", options.identityFile);
        text("certificate_file", options.certificateFile);
        flag("kerberos", options.kerberos);
        flag("kerberos_delegate", options.kerberosDelegate);
        text("kerberos_server", options.kerberosServer);
        text("file_transfer", options.fileTransfer);
        if (!options.fileTransfer.isEmpty()
            && options.fileTransfer != QStringLiteral("auto") && options.fileTransfer != QStringLiteral("sftp")
            && options.fileTransfer != QStringLiteral("scp"))
            options.fileTransfer.clear();
        flag("auto_reconnect", options.autoReconnect);
        number("reconnect_delay", options.reconnectDelay, 1, 600);
        number("reconnect_attempts", options.reconnectAttempts, 0, 100);
        text("terminal_type", options.terminalType);
        text("backspace", options.backspace);
        if (options.backspace != QStringLiteral("del") && options.backspace != QStringLiteral("ctrl-h"))
            options.backspace.clear();
    } catch (const YAML::Exception &) {
        // Keep what was read; invalid values use the defaults.
    }
    return options;
}

void sshOptionsToYaml(const SshOptions &options, YAML::Node &node)
{
    if (options.connectTimeout > 0)
        node["connect_timeout"] = options.connectTimeout;
    if (options.keepaliveSeconds >= 0)
        node["keepalive_seconds"] = options.keepaliveSeconds;
    if (options.compression >= 0)
        node["compression"] = options.compression == 1;
    if (options.tryPublicKeys >= 0)
        node["try_public_keys"] = options.tryPublicKeys == 1;
    if (options.forwardAgent >= 0)
        node["forward_agent"] = options.forwardAgent == 1;
    if (options.followTerminalFolder >= 0)
        node["follow_terminal_folder"] = options.followTerminalFolder == 1;
    if (options.forwardX11 >= 0)
        node["forward_x11"] = options.forwardX11 == 1;
    if (!options.fileTransfer.isEmpty())
        node["file_transfer"] = options.fileTransfer.toStdString();
    if (!options.identityFile.isEmpty())
        node["identity_file"] = options.identityFile.toStdString();
    if (!options.certificateFile.isEmpty())
        node["certificate_file"] = options.certificateFile.toStdString();
    if (options.kerberos >= 0)
        node["kerberos"] = options.kerberos == 1;
    if (options.kerberosDelegate >= 0)
        node["kerberos_delegate"] = options.kerberosDelegate == 1;
    if (!options.kerberosServer.isEmpty())
        node["kerberos_server"] = options.kerberosServer.toStdString();
    if (options.autoReconnect >= 0)
        node["auto_reconnect"] = options.autoReconnect == 1;
    if (options.reconnectDelay > 0)
        node["reconnect_delay"] = options.reconnectDelay;
    if (options.reconnectAttempts >= 0)
        node["reconnect_attempts"] = options.reconnectAttempts;
    if (!options.terminalType.isEmpty())
        node["terminal_type"] = options.terminalType.toStdString();
    if (!options.backspace.isEmpty())
        node["backspace"] = options.backspace.toStdString();
}

QList<QPair<QString, QString>> sshFileTransferChoices()
{
    return {{QStringLiteral("auto"), QStringLiteral("SFTP, or SCP if the server has no SFTP")},
            {QStringLiteral("sftp"), QStringLiteral("SFTP only")},
            {QStringLiteral("scp"), QStringLiteral("SCP only")}};
}

SshOptionsEditor::SshOptionsEditor(const SshOptions &value, const SshPreferences &defaults,
                                   QWidget *parent)
    : QWidget(parent), m_defaults(defaults)
{
    // A number whose lowest value means "Default (…)".
    const auto makeNumber = [this](int own, int low, int high, const QString &defaultText,
                                   const QString &suffix) {
        auto *box = new QSpinBox(this);
        box->setRange(low - 1, high);
        box->setSpecialValueText(QStringLiteral("Default (%1)").arg(defaultText));
        box->setSuffix(suffix);
        box->setValue(own >= low ? own : low - 1);
        return box;
    };
    const auto makeSwitch = [this](const QString &text, const QString &tip, int own, bool fallback) {
        auto *box = new QCheckBox(text, this);
        box->setToolTip(tip);
        box->setChecked(own >= 0 ? own == 1 : fallback);
        return box;
    };

    m_connectTimeout = makeNumber(value.connectTimeout, 1, 600,
                                  QStringLiteral("%1 s").arg(defaults.connectTimeout),
                                  QStringLiteral(" s"));
    m_keepalive = makeNumber(value.keepaliveSeconds, 0, 3600,
                             defaults.keepaliveSeconds > 0
                                 ? QStringLiteral("%1 s").arg(defaults.keepaliveSeconds)
                                 : QStringLiteral("off"),
                             QStringLiteral(" s"));
    m_keepalive->setToolTip(QStringLiteral("Keeps routers and firewalls from dropping an idle "
                                           "connection. 0 turns keepalives off."));
    m_compression = makeSwitch(QStringLiteral("Compress the connection"),
                               QStringLiteral("Helps on slow links; costs a little CPU on fast "
                                              "ones."),
                               value.compression, defaults.compression);
    m_tryPublicKeys = makeSwitch(QStringLiteral("Log on with keys from ~/.ssh and the SSH agent"),
                                 QStringLiteral("Off: only a password or the key file below."),
                                 value.tryPublicKeys, defaults.tryPublicKeys);
    m_forwardAgent = makeSwitch(QStringLiteral("Forward the SSH agent (ssh -A)"),
                                QStringLiteral("Lets this server use your local keys to log on "
                                               "further. Only for servers you trust: their "
                                               "administrators can use your keys while you are "
                                               "connected."),
                                value.forwardAgent, defaults.forwardAgent);
    m_followTerminalFolder = new QComboBox(this);
    m_followTerminalFolder->addItem(
        QStringLiteral("Use global setting (%1)").arg(defaults.followTerminalFolder
            ? QStringLiteral("enabled") : QStringLiteral("disabled")), -1);
    m_followTerminalFolder->addItem(QStringLiteral("Enabled"), 1);
    m_followTerminalFolder->addItem(QStringLiteral("Disabled"), 0);
    m_followTerminalFolder->setCurrentIndex(
        qMax(0, m_followTerminalFolder->findData(value.followTerminalFolder)));
    m_followTerminalFolder->setToolTip(QStringLiteral(
        "After each command, show the remote shell's folder in the Files pane. "
        "Disabled stays off even if the global setting changes. Applies when opening a session."));
    m_forwardX11 = makeSwitch(QStringLiteral("Forward X11 (ssh -Y)"),
                              QStringLiteral("Graphical programs started on the server show on "
                                             "this computer. Needs a local X server (on Windows: "
                                             "VcXsrv or Xming). Only for servers you trust."),
                              value.forwardX11, defaults.forwardX11);
    m_fileTransfer = new QComboBox(this);
    for (const auto &[choice, label] : sshFileTransferChoices()) {
        if (choice == defaults.fileTransfer)
            m_fileTransfer->insertItem(0, QStringLiteral("Default (%1)").arg(label), QString());
    }
    for (const auto &[choice, label] : sshFileTransferChoices())
        m_fileTransfer->addItem(label, choice);
    m_fileTransfer->setCurrentIndex(qMax(0, m_fileTransfer->findData(value.fileTransfer)));
    m_fileTransfer->setToolTip(QStringLiteral("How the file browser reaches the server's files. SCP "
                                              "suits devices without SFTP (routers, switches, "
                                              "appliances); it lists folders with shell commands."));
    m_identityFile = new QLineEdit(value.identityFile, this);
    m_identityFile->setPlaceholderText(
        defaults.identityFile.isEmpty()
            ? QStringLiteral("Default (none)")
            : QStringLiteral("Default (%1)").arg(QDir::toNativeSeparators(defaults.identityFile)));
    m_identityFile->setToolTip(QStringLiteral("An unencrypted private key tried before the "
                                              "others."));
    auto *browse = new QPushButton(QStringLiteral("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, QStringLiteral("Private Key"),
                                                       QDir::homePath() + QStringLiteral("/.ssh"));
        if (!file.isEmpty())
            m_identityFile->setText(file);
    });
    auto *identityRow = new QHBoxLayout;
    identityRow->setContentsMargins(0, 0, 0, 0);
    identityRow->addWidget(m_identityFile, 1);
    identityRow->addWidget(browse);

    m_certificate = new QComboBox(this);
    m_certificate->addItem(defaults.useCertificates
                               ? QStringLiteral("Default (the key's own, key-cert.pub)")
                               : QStringLiteral("Default (none: certificates are off)"),
                           QString());
    m_certificate->addItem(QStringLiteral("None"), QStringLiteral("none"));
    if (!value.certificateFile.isEmpty() && value.certificateFile != QStringLiteral("none"))
        m_certificate->addItem(QDir::toNativeSeparators(value.certificateFile), value.certificateFile);
    m_certificate->setCurrentIndex(qMax(0, m_certificate->findData(value.certificateFile)));
    m_certificate->setToolTip(QStringLiteral("An OpenSSH certificate signed by your organisation's "
                                             "certificate authority, sent with the private key "
                                             "above."));
    auto *browseCertificate = new QPushButton(QStringLiteral("Browse…"), this);
    connect(browseCertificate, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(
            this, QStringLiteral("Certificate"), QDir::homePath() + QStringLiteral("/.ssh"),
            QStringLiteral("Certificates (*-cert.pub);;All files (*)"));
        if (file.isEmpty())
            return;
        int index = m_certificate->findData(file);
        if (index < 0) {
            m_certificate->addItem(QDir::toNativeSeparators(file), file);
            index = m_certificate->count() - 1;
        }
        m_certificate->setCurrentIndex(index);
    });
    auto *certificateRow = new QHBoxLayout;
    certificateRow->setContentsMargins(0, 0, 0, 0);
    certificateRow->addWidget(m_certificate, 1);
    certificateRow->addWidget(browseCertificate);
    m_certificateInfo = new QLabel(this);
    m_certificateInfo->setWordWrap(true);
    m_certificateInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connect(m_certificate, &QComboBox::currentIndexChanged, this, &SshOptionsEditor::showCertificate);
    connect(m_identityFile, &QLineEdit::textChanged, this, &SshOptionsEditor::showCertificate);

    m_kerberos = makeSwitch(QStringLiteral("Log on with Kerberos (GSSAPI)"),
                            QStringLiteral("Uses your Kerberos ticket (from logging on to the "
                                           "domain, or kinit) instead of a password. Tried before "
                                           "keys and passwords."),
                            value.kerberos, defaults.kerberos);
    m_kerberosDelegate = makeSwitch(QStringLiteral("Pass the ticket on to the server (ssh -K)"),
                                    QStringLiteral("The server can use your ticket to reach "
                                                   "other servers and file shares. Only for "
                                                   "servers you trust."),
                                    value.kerberosDelegate, defaults.kerberosDelegate);
    m_kerberosServer = new QLineEdit(value.kerberosServer, this);
    m_kerberosServer->setPlaceholderText(QStringLiteral("Default (the host name)"));
    m_kerberosServer->setToolTip(QStringLiteral("The server's name in Kerberos (host/NAME), when "
                                                "it differs from the host you connect to, e.g. "
                                                "behind an alias or an IP address."));
    connect(m_kerberos, &QCheckBox::toggled, this, [this](bool on) {
        m_kerberosDelegate->setEnabled(on);
        m_kerberosServer->setEnabled(on);
    });
    m_kerberosDelegate->setEnabled(m_kerberos->isChecked());
    m_kerberosServer->setEnabled(m_kerberos->isChecked());
    m_autoReconnect = makeSwitch(QStringLiteral("Reconnect automatically if the network drops"),
                                 QString(), value.autoReconnect, defaults.autoReconnect);
    m_reconnectDelay = makeNumber(value.reconnectDelay, 1, 600,
                                  QStringLiteral("%1 s").arg(defaults.reconnectDelay),
                                  QStringLiteral(" s"));
    m_reconnectAttempts = makeNumber(value.reconnectAttempts, 0, 100,
                                     QString::number(defaults.reconnectAttempts), QString());
    m_terminalType = new QComboBox(this);
    m_terminalType->setEditable(true);
    m_terminalType->addItem(QStringLiteral("Default (%1)").arg(defaults.terminalType), QString());
    for (const auto *type : {"xterm-256color", "xterm", "vt100", "linux", "screen-256color"})
        m_terminalType->addItem(QString::fromLatin1(type), QString::fromLatin1(type));
    const int known = m_terminalType->findData(value.terminalType);
    if (known >= 0)
        m_terminalType->setCurrentIndex(known);
    else
        m_terminalType->setEditText(value.terminalType);
    m_terminalType->setToolTip(QStringLiteral("The TERM name sent to the server."));
    m_backspace = new QComboBox(this);
    m_backspace->addItem(QStringLiteral("DEL (default, ^?)"), QString());
    m_backspace->addItem(QStringLiteral("Ctrl+H (^H)"), QStringLiteral("ctrl-h"));
    m_backspace->setCurrentIndex(value.backspace == QStringLiteral("ctrl-h") ? 1 : 0);
    m_backspace->setToolTip(QStringLiteral("Match the remote system's erase character (stty -a)."));

    auto *form = new QFormLayout(this);
    form->addRow(QStringLiteral("Connect timeout:"), m_connectTimeout);
    form->addRow(QStringLiteral("Keepalive every:"), m_keepalive);
    form->addRow(QString(), m_compression);
    form->addRow(QString(), m_tryPublicKeys);
    form->addRow(QString(), m_forwardAgent);
    form->addRow(QStringLiteral("File transfer:"), m_fileTransfer);
    form->addRow(QStringLiteral("Files follow terminal folder:"), m_followTerminalFolder);
    form->addRow(QString(), m_forwardX11);
    // Off stays possible when there is no X server.
    form->addRow(QString(), new AvailabilityNote(&LocalResources::x11Problem, [this](bool available) {
        m_forwardX11->setEnabled(available || m_forwardX11->isChecked());
    }, this));
    form->addRow(QStringLiteral("Private key:"), identityRow);
    form->addRow(QStringLiteral("Certificate:"), certificateRow);
    form->addRow(QString(), m_certificateInfo);
    form->addRow(QString(), m_kerberos);
    form->addRow(QString(), new AvailabilityNote(&kerberosProblem, [this](bool available) {
        m_kerberos->setEnabled(available || m_kerberos->isChecked());
    }, this));
    form->addRow(QString(), m_kerberosDelegate);
    form->addRow(QStringLiteral("Kerberos server name:"), m_kerberosServer);
    form->addRow(QString(), m_autoReconnect);
    form->addRow(QStringLiteral("Reconnect after:"), m_reconnectDelay);
    form->addRow(QStringLiteral("Reconnect attempts:"), m_reconnectAttempts);
    form->addRow(QStringLiteral("Terminal type:"), m_terminalType);
    form->addRow(QStringLiteral("Backspace sends:"), m_backspace);
    showCertificate();
}

// Who the chosen certificate is for and until when; hidden without one.
void SshOptionsEditor::showCertificate()
{
    auto path = m_certificate->currentData().toString();
    if (path.isEmpty() && m_defaults.useCertificates) {
        auto identity = m_identityFile->text().trimmed();
        if (identity.isEmpty())
            identity = m_defaults.identityFile;
        path = defaultCertificateFor(identity);
        if (!QFileInfo::exists(path))
            path.clear();
    }
    if (path.isEmpty() || path == QStringLiteral("none")) {
        m_certificateInfo->hide();
        return;
    }
    const auto info = readSshCertificate(path);
    const bool bad = !info.valid || info.expired() || info.notYetValid();
    m_certificateInfo->setText(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), info.summary()));
    m_certificateInfo->setStyleSheet(bad ? QStringLiteral("color: #c0392b;") : QString());
    m_certificateInfo->show();
}

SshOptions SshOptionsEditor::value() const
{
    SshOptions options;
    // The lowest spin box value is "Default".
    const auto number = [](const QSpinBox *box) {
        return box->value() == box->minimum() ? (box->minimum() < 0 ? -1 : 0) : box->value();
    };
    options.connectTimeout = number(m_connectTimeout);
    options.keepaliveSeconds = number(m_keepalive);
    options.reconnectDelay = number(m_reconnectDelay);
    options.reconnectAttempts = number(m_reconnectAttempts);
    // A switch is stored only when it differs from the default.
    const auto flag = [](const QCheckBox *box, bool fallback) {
        return box->isChecked() == fallback ? -1 : (box->isChecked() ? 1 : 0);
    };
    options.compression = flag(m_compression, m_defaults.compression);
    options.tryPublicKeys = flag(m_tryPublicKeys, m_defaults.tryPublicKeys);
    options.forwardAgent = flag(m_forwardAgent, m_defaults.forwardAgent);
    options.followTerminalFolder = m_followTerminalFolder->currentData().toInt();
    options.forwardX11 = flag(m_forwardX11, m_defaults.forwardX11);
    options.autoReconnect = flag(m_autoReconnect, m_defaults.autoReconnect);
    options.identityFile = m_identityFile->text().trimmed();
    options.certificateFile = m_certificate->currentData().toString();
    options.kerberos = flag(m_kerberos, m_defaults.kerberos);
    options.kerberosDelegate = flag(m_kerberosDelegate, m_defaults.kerberosDelegate);
    options.kerberosServer = m_kerberosServer->text().trimmed();
    options.fileTransfer = m_fileTransfer->currentData().toString();
    options.backspace = m_backspace->currentData().toString();
    const int index = m_terminalType->currentIndex();
    options.terminalType = index >= 0 && m_terminalType->itemText(index) == m_terminalType->currentText()
                               ? m_terminalType->itemData(index).toString()
                               : m_terminalType->currentText().trimmed();
    return options;
}
