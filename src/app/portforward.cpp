// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "portforward.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

const char *typeKey(PortForward::Type type)
{
    switch (type) {
    case PortForward::Type::Remote:
        return "remote";
    case PortForward::Type::Dynamic:
        return "dynamic";
    case PortForward::Type::ReverseDynamic:
        return "reverse_dynamic";
    case PortForward::Type::Socket:
        return "socket";
    case PortForward::Type::HttpProxy:
        return "http_proxy";
    case PortForward::Type::Local:
        break;
    }
    return "local";
}

}

QString PortForward::description() const
{
    const QString listen = allowLan ? QStringLiteral("*:%1").arg(listenPort)
                                    : QString::number(listenPort);
    switch (type) {
    case Type::Remote:
        return QStringLiteral("Remote %1 → %2:%3").arg(listen, targetHost).arg(targetPort);
    case Type::Dynamic:
        return QStringLiteral("SOCKS proxy on %1").arg(listen);
    case Type::ReverseDynamic:
        return QStringLiteral("Reverse SOCKS proxy on the server's %1").arg(listen);
    case Type::Socket:
        return QStringLiteral("Socket %1 → %2").arg(listen, targetHost);
    case Type::HttpProxy:
        return QStringLiteral("HTTP proxy on %1").arg(listen);
    case Type::Local:
        break;
    }
    return QStringLiteral("Local %1 → %2:%3").arg(listen, targetHost).arg(targetPort);
}

QString PortForward::typeName(Type type)
{
    switch (type) {
    case Type::Remote:
        return QStringLiteral("Remote");
    case Type::Dynamic:
        return QStringLiteral("SOCKS");
    case Type::ReverseDynamic:
        return QStringLiteral("Reverse SOCKS");
    case Type::Socket:
        return QStringLiteral("Socket");
    case Type::HttpProxy:
        return QStringLiteral("HTTP proxy");
    case Type::Local:
        break;
    }
    return QStringLiteral("Local");
}

QString PortForward::sshOption() const
{
    const QString bind = allowLan ? (listensOnServer() ? QStringLiteral("0.0.0.0:") : QStringLiteral("*:"))
                                  : QString();
    // An IPv6 destination is written in brackets.
    const QString host = targetHost.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(targetHost)
                                                               : targetHost;
    switch (type) {
    case Type::Remote:
        return QStringLiteral("-R %1%2:%3:%4").arg(bind).arg(listenPort).arg(host).arg(targetPort);
    case Type::Dynamic:
        return QStringLiteral("-D %1%2").arg(bind).arg(listenPort);
    case Type::ReverseDynamic:
        return QStringLiteral("-R %1%2").arg(bind).arg(listenPort);
    case Type::Socket:
        return QStringLiteral("-L %1%2:%3").arg(bind).arg(listenPort).arg(targetHost);
    case Type::HttpProxy:
        return {};
    case Type::Local:
        break;
    }
    return QStringLiteral("-L %1%2:%3:%4").arg(bind).arg(listenPort).arg(host).arg(targetPort);
}

PortForward portForwardFromYaml(const YAML::Node &node)
{
    PortForward forward;
    if (!node || !node.IsMap())
        return forward;
    const auto type = node["type"].as<std::string>("local");
    forward.type = type == "remote"            ? PortForward::Type::Remote
                   : type == "dynamic"         ? PortForward::Type::Dynamic
                   : type == "reverse_dynamic" ? PortForward::Type::ReverseDynamic
                   : type == "socket"          ? PortForward::Type::Socket
                   : type == "http_proxy"      ? PortForward::Type::HttpProxy
                                               : PortForward::Type::Local;
    forward.listenPort = static_cast<quint16>(qBound(0, node["listen_port"].as<int>(0), 65535));
    forward.allowLan = node["allow_lan"].as<bool>(false);
    forward.targetHost = QString::fromStdString(node["target_host"].as<std::string>(""));
    forward.targetPort = static_cast<quint16>(qBound(0, node["target_port"].as<int>(0), 65535));
    return forward;
}

YAML::Node portForwardToYaml(const PortForward &forward)
{
    YAML::Node node;
    node["type"] = typeKey(forward.type);
    node["listen_port"] = static_cast<int>(forward.listenPort);
    if (forward.allowLan)
        node["allow_lan"] = true;
    if (forward.hasTarget()) {
        node["target_host"] = forward.targetHost.toStdString();
        if (forward.type != PortForward::Type::Socket)
            node["target_port"] = static_cast<int>(forward.targetPort);
    }
    return node;
}

QList<PortForward> portForwardsFromYaml(const YAML::Node &node)
{
    QList<PortForward> forwards;
    if (node && node.IsSequence()) {
        for (const auto &item : node)
            forwards.append(portForwardFromYaml(item));
    }
    return forwards;
}

YAML::Node portForwardsToYaml(const QList<PortForward> &forwards)
{
    YAML::Node node(YAML::NodeType::Sequence);
    for (const auto &forward : forwards)
        node.push_back(portForwardToYaml(forward));
    return node;
}

PortForwardEditor::PortForwardEditor(const PortForward &value, QWidget *parent) : QWidget(parent)
{
    auto *form = new QFormLayout(this);
    m_type = new QComboBox(this);
    m_type->addItem(QStringLiteral("Local (a port here reaches a host behind the server)"),
                    int(PortForward::Type::Local));
    m_type->addItem(QStringLiteral("Remote (a port on the server reaches a host here)"),
                    int(PortForward::Type::Remote));
    m_type->addItem(QStringLiteral("Dynamic (SOCKS proxy through the server)"),
                    int(PortForward::Type::Dynamic));
    m_type->addItem(QStringLiteral("Reverse SOCKS (a proxy on the server through this computer)"),
                    int(PortForward::Type::ReverseDynamic));
    m_type->addItem(QStringLiteral("Socket (a port here reaches a Unix socket on the server)"),
                    int(PortForward::Type::Socket));
    m_type->addItem(QStringLiteral("HTTP proxy (through the server, for programs without SOCKS)"),
                    int(PortForward::Type::HttpProxy));
    m_type->setCurrentIndex(m_type->findData(int(value.type)));
    m_listenPort = new QSpinBox(this);
    m_listenPort->setRange(1, 65535);
    m_listenPort->setValue(value.listenPort > 0 ? value.listenPort : 8080);
    m_allowLan = new QCheckBox(this);
    m_allowLan->setChecked(value.allowLan);
    m_targetHost = new QLineEdit(value.targetHost, this);
    m_targetPort = new QSpinBox(this);
    m_targetPort->setRange(1, 65535);
    m_targetPort->setValue(value.targetPort > 0 ? value.targetPort : 80);
    m_help = new QLabel(this);
    m_help->setWordWrap(true);
    m_help->setForegroundRole(QPalette::PlaceholderText);

    form->addRow(QStringLiteral("Type:"), m_type);
    form->addRow(QStringLiteral("Listen on port:"), m_listenPort);
    form->addRow(QString(), m_allowLan);
    form->addRow(QStringLiteral("Destination host:"), m_targetHost);
    form->addRow(QStringLiteral("Destination port:"), m_targetPort);
    form->addRow(m_help);
    connect(m_type, &QComboBox::currentIndexChanged, this, &PortForwardEditor::updateFields);
    updateFields();
}

void PortForwardEditor::updateFields()
{
    const auto type = PortForward::Type(m_type->currentData().toInt());
    PortForward probe;
    probe.type = type;
    m_targetHost->setEnabled(probe.hasTarget());
    m_targetPort->setEnabled(probe.hasTarget() && type != PortForward::Type::Socket);
    switch (type) {
    case PortForward::Type::Local:
        m_allowLan->setText(QStringLiteral("Allow other computers to connect to this port"));
        m_targetHost->setPlaceholderText(QStringLiteral("as seen from the SSH server, e.g. localhost"));
        m_help->setText(QStringLiteral("Connections to the port on this computer are carried to "
                                       "the destination by the SSH server (ssh -L)."));
        break;
    case PortForward::Type::Remote:
        m_allowLan->setText(QStringLiteral("Ask the server to accept connections from other "
                                           "computers (needs GatewayPorts on the server)"));
        m_targetHost->setPlaceholderText(QStringLiteral("as seen from this computer, e.g. localhost"));
        m_help->setText(QStringLiteral("Connections to the port on the SSH server are carried "
                                       "back to the destination from this computer (ssh -R)."));
        break;
    case PortForward::Type::Dynamic:
        m_allowLan->setText(QStringLiteral("Allow other computers to use the proxy"));
        m_help->setText(QStringLiteral("A SOCKS 4/5 proxy on this computer. Programs set to use "
                                       "it connect through the SSH server (ssh -D)."));
        break;
    case PortForward::Type::ReverseDynamic:
        m_allowLan->setText(QStringLiteral("Ask the server to let other computers use the proxy "
                                           "(needs GatewayPorts on the server)"));
        m_help->setText(QStringLiteral("A SOCKS proxy on the SSH server: programs there reach "
                                       "the networks of this computer (ssh -R port; OpenSSH 7.6 "
                                       "or later on the server)."));
        break;
    case PortForward::Type::Socket:
        m_allowLan->setText(QStringLiteral("Allow other computers to connect to this port"));
        m_targetHost->setPlaceholderText(QStringLiteral("socket path on the server, e.g. /var/run/docker.sock"));
        m_help->setText(QStringLiteral("Connections to the port on this computer reach a Unix "
                                       "socket on the SSH server (ssh -L port:/path)."));
        break;
    case PortForward::Type::HttpProxy:
        m_allowLan->setText(QStringLiteral("Allow other computers to use the proxy"));
        m_help->setText(QStringLiteral("An HTTP proxy on this computer (CONNECT and plain http://) "
                                       "for programs that cannot use SOCKS; the SSH server makes "
                                       "the connections."));
        break;
    }
}

PortForward PortForwardEditor::value() const
{
    PortForward forward;
    forward.type = PortForward::Type(m_type->currentData().toInt());
    forward.listenPort = static_cast<quint16>(m_listenPort->value());
    forward.allowLan = m_allowLan->isChecked();
    if (forward.hasTarget()) {
        forward.targetHost = m_targetHost->text().trimmed();
        forward.targetPort = forward.type == PortForward::Type::Socket
                                 ? 0
                                 : static_cast<quint16>(m_targetPort->value());
    }
    return forward;
}

QString PortForwardEditor::problem() const
{
    const auto forward = value();
    if (forward.hasTarget() && forward.targetHost.isEmpty())
        return forward.type == PortForward::Type::Socket ? QStringLiteral("Enter the socket's path.")
                                                         : QStringLiteral("Enter the destination host.");
    if (forward.type == PortForward::Type::Socket && !forward.targetHost.startsWith(QLatin1Char('/')))
        return QStringLiteral("The socket's path starts with /.");
    return QString();
}

PortForwardListEditor::PortForwardListEditor(const QList<PortForward> &value, QWidget *parent)
    : QWidget(parent), m_forwards(value)
{
    auto *layout = new QVBoxLayout(this);
    auto *note = new QLabel(QStringLiteral("Opened with this session, over its connection, and "
                                           "closed with its tab."), this);
    note->setWordWrap(true);
    layout->addWidget(note);
    m_list = new QTreeWidget(this);
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(false);
    m_list->setMinimumHeight(110);
    layout->addWidget(m_list);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add…"), this);
    auto *editButton = new QPushButton(QStringLiteral("Edit…"), this);
    auto *remove = new QPushButton(QStringLiteral("Remove"), this);
    buttons->addWidget(add);
    buttons->addWidget(editButton);
    buttons->addWidget(remove);
    buttons->addStretch();
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, [this] { edit(-1); });
    connect(editButton, &QPushButton::clicked, this, [this] {
        if (m_list->currentItem())
            edit(m_list->indexOfTopLevelItem(m_list->currentItem()));
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item) { edit(m_list->indexOfTopLevelItem(item)); });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (auto *item = m_list->currentItem()) {
            m_forwards.removeAt(m_list->indexOfTopLevelItem(item));
            refresh();
        }
    });
    refresh();
}

void PortForwardListEditor::refresh()
{
    m_list->clear();
    for (const auto &forward : std::as_const(m_forwards))
        m_list->addTopLevelItem(new QTreeWidgetItem({forward.description()}));
}

// index -1 adds a forward.
void PortForwardListEditor::edit(int index)
{
    QDialog dialog(this);
    dialog.setWindowTitle(index < 0 ? QStringLiteral("Add Port Forward")
                                    : QStringLiteral("Edit Port Forward"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *editor = new PortForwardEditor(index < 0 ? PortForward() : m_forwards[index], &dialog);
    layout->addWidget(editor);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, editor] {
        if (const auto problem = editor->problem(); !problem.isEmpty())
            QMessageBox::warning(&dialog, dialog.windowTitle(), problem);
        else
            dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (index < 0)
        m_forwards.append(editor->value());
    else
        m_forwards[index] = editor->value();
    refresh();
}
