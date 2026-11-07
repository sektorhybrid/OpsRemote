// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sshoptions.h"
#include "filedialogs.h"
#include "inheritance.h"
#include "formcard.h"
#include "reconnect.h"
#include "availabilitynote.h"
#include "localresources.h"
#include "sshauth.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QVBoxLayout>
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
    if (!kerberosOrder.isEmpty())
        preferences.kerberosOrder = kerberosOrder;
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
        text("kerberos_order", options.kerberosOrder);
        if (!QStringList{QString(), QStringLiteral("first"), QStringLiteral("after-keys"),
                         QStringLiteral("last")}.contains(options.kerberosOrder))
            options.kerberosOrder.clear();
        text("file_transfer", options.fileTransfer);
        if (!options.fileTransfer.isEmpty()
            && options.fileTransfer != QStringLiteral("auto") && options.fileTransfer != QStringLiteral("sftp")
            && options.fileTransfer != QStringLiteral("scp"))
            options.fileTransfer.clear();
        flag("auto_reconnect", options.autoReconnect);
        readReconnectTiming(node, options.reconnectDelay, options.reconnectAttempts);
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
    if (!options.kerberosOrder.isEmpty())
        node["kerberos_order"] = options.kerberosOrder.toStdString();
    if (options.autoReconnect >= 0)
        node["auto_reconnect"] = options.autoReconnect == 1;
    writeReconnectTiming(node, options.reconnectDelay, options.reconnectAttempts);
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
    // A number whose lowest value means the inherited one (shown as it).
    const auto makeNumber = [this](int own, int low, int high, const QString &defaultText,
                                   const QString &suffix) {
        auto *box = new QSpinBox(this);
        box->setRange(low - 1, high);
        box->setSpecialValueText(defaultText);
        box->setProperty("inherits", true); // applyInheritance()
        box->setSuffix(suffix);
        box->setValue(own >= low ? own : low - 1);
        return box;
    };
    const auto makeSwitch = [this](const QString &text, const QString &tip, int own, bool fallback) {
        auto *box = new QCheckBox(text, this);
        box->setToolTip(tip);
        box->setChecked(own >= 0 ? own == 1 : fallback);
        box->setProperty("inheritedOn", fallback); // applyInheritance()
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
    // The inherited choice first, then only the other one (inheritance.h).
    addInheritedItem(m_followTerminalFolder,
                     defaults.followTerminalFolder ? QStringLiteral("On") : QStringLiteral("Off"), -1);
    if (defaults.followTerminalFolder)
        m_followTerminalFolder->addItem(QStringLiteral("Off"), 0);
    else
        m_followTerminalFolder->addItem(QStringLiteral("On"), 1);
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
            insertInheritedItem(m_fileTransfer, QString(label), QString());
    }
    for (const auto &[choice, label] : sshFileTransferChoices())
        if (choice != defaults.fileTransfer) // listed once, as the inherited one
            m_fileTransfer->addItem(label, choice);
    m_fileTransfer->setCurrentIndex(qMax(0, m_fileTransfer->findData(value.fileTransfer)));
    m_fileTransfer->setToolTip(QStringLiteral("How the file browser reaches the server's files. SCP "
                                              "suits devices without SFTP (routers, switches, "
                                              "appliances); it lists folders with shell commands."));
    m_identityFile = new QLineEdit(value.identityFile, this);
    m_identityFile->setPlaceholderText(
        defaults.identityFile.isEmpty()
            ? QStringLiteral("None")
            : QDir::toNativeSeparators(defaults.identityFile));
    m_identityFile->setToolTip(QStringLiteral("An unencrypted private key tried before the "
                                              "others."));
    auto *browse = new QPushButton(QStringLiteral("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto file = FileDialogs::openFileName(this, QStringLiteral("Private Key"),
                                                       QDir::homePath() + QStringLiteral("/.ssh"));
        if (!file.isEmpty())
            m_identityFile->setText(file);
    });
    auto *identityRow = new QHBoxLayout;
    identityRow->setContentsMargins(0, 0, 0, 0);
    identityRow->addWidget(m_identityFile, 1);
    identityRow->addWidget(browse);

    m_certificate = new QComboBox(this);
    addInheritedItem(m_certificate, defaults.useCertificates
                                        ? QStringLiteral("The key's own (key-cert.pub)")
                                        : QStringLiteral("None (certificates are off)"),
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
        const auto file = FileDialogs::openFileName(
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
                                           "domain, or kinit) instead of a password. When it is "
                                           "tried: the sequence of authentication below."),
                            value.kerberos, defaults.kerberos);
    m_kerberosDelegate = makeSwitch(QStringLiteral("Pass the ticket on to the server (ssh -K)"),
                                    QStringLiteral("The server can use your ticket to reach "
                                                   "other servers and file shares. Only for "
                                                   "servers you trust."),
                                    value.kerberosDelegate, defaults.kerberosDelegate);
    m_kerberosServer = new QLineEdit(value.kerberosServer, this);
    m_kerberosServer->setPlaceholderText(QStringLiteral("The host name"));
    m_kerberosServer->setToolTip(QStringLiteral("The server's name in Kerberos (host/NAME), when "
                                                "it differs from the host you connect to, e.g. "
                                                "behind an alias or an IP address."));
    m_kerberosOrder = new QComboBox(this);
    {
        static const QList<QPair<QString, QString>> orders = {{QStringLiteral("first"), QStringLiteral("First, before keys and passwords")},
     {QStringLiteral("after-keys"), QStringLiteral("After keys")},
     {QStringLiteral("last"), QStringLiteral("Last, after keys and the saved password")}};
        QString inherited;
        for (const auto &[key, label] : orders)
            if (key == defaults.kerberosOrder)
                inherited = label;
        addInheritedItem(m_kerberosOrder, QString(inherited), QString());
        for (const auto &[key, label] : orders)
            if (key != defaults.kerberosOrder) // listed once, as the inherited one
                m_kerberosOrder->addItem(label, key);
        m_kerberosOrder->setCurrentIndex(qMax(0, m_kerberosOrder->findData(value.kerberosOrder)));
        m_kerberosOrder->setToolTip(QStringLiteral("When Kerberos is tried among the ways to log on."));
    }
    connect(m_kerberos, &QCheckBox::toggled, this, [this](bool on) {
        m_kerberosDelegate->setEnabled(on);
        m_kerberosServer->setEnabled(on);
        m_kerberosOrder->setEnabled(on);
    });
    m_kerberosDelegate->setEnabled(m_kerberos->isChecked());
    m_kerberosServer->setEnabled(m_kerberos->isChecked());
    m_kerberosOrder->setEnabled(m_kerberos->isChecked());
    m_autoReconnect = makeSwitch(QStringLiteral("Reconnect automatically if the network drops"),
                                 QString(), value.autoReconnect, defaults.autoReconnect);
    m_terminalType = new QComboBox(this);
    m_terminalType->setEditable(true);
    addInheritedItem(m_terminalType, QString(defaults.terminalType), QString());
    for (const auto *type : {"xterm-256color", "xterm", "vt100", "linux", "screen-256color"})
        m_terminalType->addItem(QString::fromLatin1(type), QString::fromLatin1(type));
    const int known = m_terminalType->findData(value.terminalType);
    if (known >= 0)
        m_terminalType->setCurrentIndex(known);
    else
        m_terminalType->setEditText(value.terminalType);
    m_terminalType->setToolTip(QStringLiteral("The TERM name sent to the server."));
    m_backspace = new QComboBox(this);
    m_backspace->addItem(QStringLiteral("DEL (^?)"), QString());
    m_backspace->addItem(QStringLiteral("Ctrl+H (^H)"), QStringLiteral("ctrl-h"));
    m_backspace->setCurrentIndex(value.backspace == QStringLiteral("ctrl-h") ? 1 : 0);
    m_backspace->setToolTip(QStringLiteral("Match the remote system's erase character (stty -a)."));

    // Related settings in light cards (formcard.h).
    auto *page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(6);
    // The same cards in every session type: General, Logon, then the
    // type's own. In each: fields, the switches that stand alone, then the
    // groups led by a switch (formcard.h).
    auto *form = addFormCard(page, QStringLiteral("General"), this);
    m_generalForm = form;
    form->addRow(QStringLiteral("Timeout:"), m_connectTimeout);
    form->addRow(QStringLiteral("Keepalive:"), m_keepalive);
    form->addRow(QStringLiteral("File transfer:"), m_fileTransfer);
    form->addRow(QStringLiteral("Files follow folder:"), m_followTerminalFolder);
    form->addRow(QString(), m_compression);
    form->addRow(QString(), m_forwardX11);
    // Off stays possible when there is no X server.
    form->addRow(QString(), new AvailabilityNote(&LocalResources::x11Problem, [this](bool available) {
        m_forwardX11->setEnabled(available || m_forwardX11->isChecked());
    }, this));
    // Rows only a terminal uses (setFileOnly()); rows keep their numbers
    // when applyInheritance() wraps their fields.
    for (QWidget *field : {static_cast<QWidget *>(m_followTerminalFolder), static_cast<QWidget *>(m_forwardX11)}) {
        int row = -1;
        form->getWidgetPosition(field, &row, nullptr);
        m_terminalRows << row;
    }
    m_terminalRows << m_terminalRows.last() + 1; // the X server note
    form->addRow(QString(), m_autoReconnect);
    // The session type's wait and tries, inherited until set here.
    m_reconnectDelay = reconnectDelayBox(value.reconnectDelay, m_defaults.reconnectDelay);
    m_reconnectAttempts = reconnectAttemptsBox(value.reconnectAttempts, m_defaults.reconnectAttempts);
    form->addRow(QStringLiteral("Wait:"), m_reconnectDelay);
    form->addRow(QStringLiteral("Tries:"), m_reconnectAttempts);
    for (QWidget *box : {static_cast<QWidget *>(m_reconnectDelay), static_cast<QWidget *>(m_reconnectAttempts)}) {
        box->setEnabled(m_autoReconnect->isChecked());
        connect(m_autoReconnect, &QCheckBox::toggled, box, &QWidget::setEnabled);
    }
    form = addFormCard(page, QStringLiteral("Logon"), this);
    form->addRow(QStringLiteral("Private key:"), identityRow);
    form->addRow(QStringLiteral("Certificate:"), certificateRow);
    form->addRow(QString(), m_certificateInfo);
    form->addRow(QString(), m_tryPublicKeys);
    form->addRow(QString(), m_forwardAgent);
    form->addRow(QString(), m_kerberos);
    form->addRow(QString(), new AvailabilityNote(&kerberosProblem, [this](bool available) {
        m_kerberos->setEnabled(available || m_kerberos->isChecked());
    }, this));
    form->addRow(QString(), m_kerberosDelegate);
    form->addRow(QStringLiteral("Kerberos server:"), m_kerberosServer);
    form->addRow(QStringLiteral("Kerberos order:"), m_kerberosOrder);
    form = addFormCard(page, QStringLiteral("Terminal"), this);
    m_terminalCard = form->parentWidget();
    form->addRow(QStringLiteral("Type:"), m_terminalType);
    form->addRow(QStringLiteral("Backspace sends:"), m_backspace);
    page->addStretch();
    alignFormLabels(this);
    showCertificate();
}

void SshOptionsEditor::setFileOnly(bool fileOnly)
{
    for (int row : std::as_const(m_terminalRows))
        m_generalForm->setRowVisible(row, !fileOnly);
    m_terminalCard->setVisible(!fileOnly);
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
    options.reconnectDelay = m_reconnectDelay->value();
    options.reconnectAttempts = m_reconnectAttempts->value();
    options.identityFile = m_identityFile->text().trimmed();
    options.certificateFile = m_certificate->currentData().toString();
    options.kerberos = flag(m_kerberos, m_defaults.kerberos);
    options.kerberosDelegate = flag(m_kerberosDelegate, m_defaults.kerberosDelegate);
    options.kerberosServer = m_kerberosServer->text().trimmed();
    options.kerberosOrder = m_kerberosOrder->currentData().toString();
    options.fileTransfer = m_fileTransfer->currentData().toString();
    options.backspace = m_backspace->currentData().toString();
    const int index = m_terminalType->currentIndex();
    options.terminalType = index >= 0 && m_terminalType->itemText(index) == m_terminalType->currentText()
                               ? m_terminalType->itemData(index).toString()
                               : m_terminalType->currentText().trimmed();
    return options;
}
