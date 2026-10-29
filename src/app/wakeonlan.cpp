// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "wakeonlan.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUdpSocket>

QByteArray parseMacAddress(const QString &text)
{
    QString hex = text.trimmed();
    hex.remove(QLatin1Char(':')).remove(QLatin1Char('-')).remove(QLatin1Char('.'));
    if (hex.size() != 12)
        return {};
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    return bytes.size() == 6 && bytes.toHex().compare(hex.toLatin1(), Qt::CaseInsensitive) == 0
               ? bytes
               : QByteArray();
}

bool WakeOnLan::send(QString *error) const
{
    const QByteArray address = parseMacAddress(mac);
    if (address.isEmpty()) {
        *error = QStringLiteral("\"%1\" is not a MAC address.").arg(mac);
        return false;
    }
    const QHostAddress target(broadcast.isEmpty() ? QStringLiteral("255.255.255.255") : broadcast);
    if (target.isNull()) {
        *error = QStringLiteral("\"%1\" is not a broadcast address.").arg(broadcast);
        return false;
    }
    QByteArray packet(6, char(0xff));
    for (int i = 0; i < 16; ++i)
        packet += address;
    QUdpSocket socket;
    // Twice, as a lost UDP packet is not repeated.
    for (int i = 0; i < 2; ++i) {
        if (socket.writeDatagram(packet, target, port > 0 ? port : 9) != packet.size()) {
            *error = socket.errorString();
            return false;
        }
    }
    return true;
}

WakeOnLan wakeOnLanFromYaml(const YAML::Node &node)
{
    WakeOnLan value;
    if (!node || !node.IsMap())
        return value;
    value.mac = QString::fromStdString(node["mac"].as<std::string>(""));
    value.broadcast = QString::fromStdString(node["broadcast"].as<std::string>(""));
    value.port = static_cast<quint16>(qBound(0, node["port"].as<int>(0), 65535));
    value.beforeConnecting = node["before_connecting"].as<bool>(false);
    value.waitSeconds = qBound(0, node["wait_seconds"].as<int>(0), 600);
    return value;
}

YAML::Node wakeOnLanToYaml(const WakeOnLan &value)
{
    YAML::Node node;
    node["mac"] = value.mac.toStdString();
    if (!value.broadcast.isEmpty())
        node["broadcast"] = value.broadcast.toStdString();
    if (value.port > 0)
        node["port"] = static_cast<int>(value.port);
    if (value.beforeConnecting)
        node["before_connecting"] = true;
    if (value.waitSeconds > 0)
        node["wait_seconds"] = value.waitSeconds;
    return node;
}

WakeOnLanEditor::WakeOnLanEditor(const WakeOnLan &value, QWidget *parent) : QWidget(parent)
{
    auto *form = new QFormLayout(this);
    m_mac = new QLineEdit(value.mac, this);
    m_mac->setPlaceholderText(QStringLiteral("aa:bb:cc:dd:ee:ff"));
    m_broadcast = new QLineEdit(value.broadcast, this);
    m_broadcast->setPlaceholderText(QStringLiteral("255.255.255.255, or the network's, e.g. 192.168.1.255"));
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(value.port > 0 ? value.port : 9);
    m_beforeConnecting = new QCheckBox(QStringLiteral("Wake the computer before connecting"), this);
    m_beforeConnecting->setChecked(value.beforeConnecting);
    m_wait = new QSpinBox(this);
    m_wait->setRange(1, 600);
    m_wait->setSuffix(QStringLiteral(" s"));
    m_wait->setValue(value.effectiveWait());
    auto *test = new QPushButton(QStringLiteral("Wake Up Now"), this);
    auto *result = new QLabel(this);
    result->setWordWrap(true);

    form->addRow(QStringLiteral("MAC address:"), m_mac);
    form->addRow(QStringLiteral("Broadcast address:"), m_broadcast);
    form->addRow(QStringLiteral("Port:"), m_port);
    form->addRow(QString(), m_beforeConnecting);
    form->addRow(QStringLiteral("Then wait:"), m_wait);
    form->addRow(test, result);
    auto *help = new QLabel(QStringLiteral(
        "The computer's network card must have Wake-on-LAN turned on, and the packet must reach "
        "its network (it does not cross routers unless they forward it). Right-click the "
        "session → Wake Up sends it at any time."), this);
    help->setWordWrap(true);
    help->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(help);

    connect(m_beforeConnecting, &QCheckBox::toggled, m_wait, &QSpinBox::setEnabled);
    m_wait->setEnabled(value.beforeConnecting);
    connect(test, &QPushButton::clicked, this, [this, result] {
        QString error = problem();
        if (error.isEmpty() && this->value().send(&error))
            result->setText(QStringLiteral("Sent."));
        else
            result->setText(error);
    });
}

WakeOnLan WakeOnLanEditor::value() const
{
    WakeOnLan value;
    value.mac = m_mac->text().trimmed();
    if (value.mac.isEmpty())
        return value;
    value.broadcast = m_broadcast->text().trimmed();
    value.port = m_port->value() == 9 ? 0 : static_cast<quint16>(m_port->value());
    value.beforeConnecting = m_beforeConnecting->isChecked();
    value.waitSeconds = m_wait->value() == 20 ? 0 : m_wait->value();
    return value;
}

QString WakeOnLanEditor::problem() const
{
    const auto value = this->value();
    if (!value.isSet())
        return QString();
    if (parseMacAddress(value.mac).isEmpty())
        return QStringLiteral("\"%1\" is not a MAC address (write it as aa:bb:cc:dd:ee:ff).")
            .arg(value.mac);
    if (!value.broadcast.isEmpty() && QHostAddress(value.broadcast).isNull())
        return QStringLiteral("\"%1\" is not an IP address.").arg(value.broadcast);
    return QString();
}
