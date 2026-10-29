// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "proxy.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QUrl>

ProxySettings ProxySettings::parse(const QString &value)
{
    ProxySettings proxy;
    const QUrl url(value.trimmed());
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http"))
        proxy.type = Type::Http;
    else if (scheme == QStringLiteral("socks5") || scheme == QStringLiteral("socks5h"))
        proxy.type = Type::Socks5;
    else
        return proxy;
    proxy.host = url.host();
    proxy.port = static_cast<quint16>(url.port(proxy.type == Type::Http ? 3128 : 1080));
    proxy.user = url.userName();
    return proxy;
}

QString ProxySettings::toString() const
{
    if (!isSet())
        return ProxySettings::Direct;
    QUrl url;
    url.setScheme(type == Type::Http ? QStringLiteral("http") : QStringLiteral("socks5"));
    url.setHost(host);
    url.setPort(port);
    url.setUserName(user);
    return url.toString();
}

QString ProxySettings::label(const QString &value)
{
    const auto proxy = ProxySettings::parse(value);
    if (!proxy.isSet())
        return QStringLiteral("none");
    return QStringLiteral("%1 %2:%3")
        .arg(proxy.type == ProxySettings::Type::Http ? QStringLiteral("HTTP") : QStringLiteral("SOCKS5"),
             proxy.host)
        .arg(proxy.port);
}

ProxyEditor::ProxyEditor(const QString &value, const QString &password, const QString &inherited,
                         QWidget *parent)
    : QWidget(parent)
{
    const auto proxy = ProxySettings::parse(value);
    auto *form = new QFormLayout(this);
    m_type = new QComboBox(this);
    if (!inherited.isEmpty())
        m_type->addItem(QStringLiteral("Inherit (%1)").arg(inherited), QStringLiteral("inherit"));
    m_type->addItem(QStringLiteral("None (connect directly)"), ProxySettings::Direct);
    m_type->addItem(QStringLiteral("HTTP proxy (CONNECT)"), QStringLiteral("http"));
    m_type->addItem(QStringLiteral("SOCKS5 proxy"), QStringLiteral("socks5"));
    const QString current = value.isEmpty() && !inherited.isEmpty() ? QStringLiteral("inherit")
                            : proxy.type == ProxySettings::Type::Http   ? QStringLiteral("http")
                            : proxy.type == ProxySettings::Type::Socks5 ? QStringLiteral("socks5")
                                                                        : ProxySettings::Direct;
    m_type->setCurrentIndex(qMax(0, m_type->findData(current)));
    m_host = new QLineEdit(proxy.host, this);
    m_host->setPlaceholderText(QStringLiteral("proxy.example.com"));
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(proxy.port > 0 ? proxy.port : 3128);
    m_user = new QLineEdit(proxy.user, this);
    m_user->setPlaceholderText(QStringLiteral("Only if the proxy asks for one"));
    m_password = new QLineEdit(password, this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(QStringLiteral("Kept in the credential vault"));

    form->addRow(QStringLiteral("Proxy:"), m_type);
    form->addRow(QStringLiteral("Host:"), m_host);
    form->addRow(QStringLiteral("Port:"), m_port);
    form->addRow(QStringLiteral("User name:"), m_user);
    form->addRow(QStringLiteral("Password:"), m_password);
    auto *help = new QLabel(QStringLiteral(
        "SSH connections go out through this proxy, for networks that only allow web traffic "
        "out. With jump hosts, only the first one is reached through it. The proxy's own "
        "password is sent unencrypted (as every HTTP and SOCKS5 proxy expects), so the network "
        "up to the proxy can see it; the SSH connection inside stays encrypted."), this);
    help->setWordWrap(true);
    help->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(help);

    connect(m_type, &QComboBox::currentIndexChanged, this, [this] {
        // Typical ports when switching type with the other type's default in place.
        const auto type = m_type->currentData().toString();
        if (type == QStringLiteral("socks5") && m_port->value() == 3128)
            m_port->setValue(1080);
        else if (type == QStringLiteral("http") && m_port->value() == 1080)
            m_port->setValue(3128);
        updateFields();
        emit changed();
    });
    for (auto *edit : {m_host, m_user})
        connect(edit, &QLineEdit::textChanged, this, &ProxyEditor::changed);
    connect(m_password, &QLineEdit::editingFinished, this, [this] {
        if (m_password->isModified())
            emit passwordEntered();
        m_password->setModified(false);
    });
    connect(m_port, &QSpinBox::valueChanged, this, &ProxyEditor::changed);
    updateFields();
}

void ProxyEditor::updateFields()
{
    const auto type = m_type->currentData().toString();
    const bool own = type == QStringLiteral("http") || type == QStringLiteral("socks5");
    for (QWidget *widget : {static_cast<QWidget *>(m_host), static_cast<QWidget *>(m_port),
                            static_cast<QWidget *>(m_user), static_cast<QWidget *>(m_password)})
        widget->setEnabled(own);
}

QString ProxyEditor::value() const
{
    const auto type = m_type->currentData().toString();
    if (type == QStringLiteral("inherit"))
        return QString();
    if (type == ProxySettings::Direct || m_host->text().trimmed().isEmpty())
        return ProxySettings::Direct;
    ProxySettings proxy;
    proxy.type = type == QStringLiteral("http") ? ProxySettings::Type::Http
                                                : ProxySettings::Type::Socks5;
    proxy.host = m_host->text().trimmed();
    proxy.port = static_cast<quint16>(m_port->value());
    proxy.user = m_user->text().trimmed();
    return proxy.toString();
}

QString ProxyEditor::password() const
{
    return m_password->isEnabled() ? m_password->text() : QString();
}
