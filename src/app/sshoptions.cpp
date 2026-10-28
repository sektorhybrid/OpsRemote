// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sshoptions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
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
    if (!identityFile.isEmpty())
        preferences.identityFile = identityFile;
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
        text("identity_file", options.identityFile);
        flag("auto_reconnect", options.autoReconnect);
        number("reconnect_delay", options.reconnectDelay, 1, 600);
        number("reconnect_attempts", options.reconnectAttempts, 0, 100);
        text("terminal_type", options.terminalType);
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
    if (!options.identityFile.isEmpty())
        node["identity_file"] = options.identityFile.toStdString();
    if (options.autoReconnect >= 0)
        node["auto_reconnect"] = options.autoReconnect == 1;
    if (options.reconnectDelay > 0)
        node["reconnect_delay"] = options.reconnectDelay;
    if (options.reconnectAttempts >= 0)
        node["reconnect_attempts"] = options.reconnectAttempts;
    if (!options.terminalType.isEmpty())
        node["terminal_type"] = options.terminalType.toStdString();
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

    auto *form = new QFormLayout(this);
    form->addRow(QStringLiteral("Connect timeout:"), m_connectTimeout);
    form->addRow(QStringLiteral("Keepalive every:"), m_keepalive);
    form->addRow(QString(), m_compression);
    form->addRow(QString(), m_tryPublicKeys);
    form->addRow(QStringLiteral("Private key:"), identityRow);
    form->addRow(QString(), m_autoReconnect);
    form->addRow(QStringLiteral("Reconnect after:"), m_reconnectDelay);
    form->addRow(QStringLiteral("Reconnect attempts:"), m_reconnectAttempts);
    form->addRow(QStringLiteral("Terminal type:"), m_terminalType);
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
    options.autoReconnect = flag(m_autoReconnect, m_defaults.autoReconnect);
    options.identityFile = m_identityFile->text().trimmed();
    const int index = m_terminalType->currentIndex();
    options.terminalType = index >= 0 && m_terminalType->itemText(index) == m_terminalType->currentText()
                               ? m_terminalType->itemData(index).toString()
                               : m_terminalType->currentText().trimmed();
    return options;
}
