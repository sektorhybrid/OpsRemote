// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QDialog>
#include <QList>
#include <QString>
#include <QWidget>

#include <yaml-cpp/yaml.h>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTreeWidget;

// One SSH port forward:
//   Local    (ssh -L) a port here reaches targetHost:targetPort from the server
//   Remote   (ssh -R) a port on the server reaches targetHost:targetPort from here
//   Dynamic  (ssh -D) a SOCKS 4/5 proxy here; the server makes the connections
//   ReverseDynamic (ssh -R port) a SOCKS proxy on the server; this computer
//            makes the connections (OpenSSH 7.6 or later on the server)
//   Socket   (ssh -L port:/path) a port here reaches a Unix socket on the
//            server (targetHost is its path), e.g. /var/run/docker.sock
//   HttpProxy an HTTP proxy here (CONNECT and plain http://); the server
//            makes the connections, for programs without SOCKS
struct PortForward
{
    enum class Type { Local, Remote, Dynamic, ReverseDynamic, Socket, HttpProxy };
    Type type = Type::Local;
    quint16 listenPort = 0;
    // Listen on every address instead of only this computer (Local and
    // Dynamic), or ask the server to (Remote; needs GatewayPorts there).
    bool allowLan = false;
    QString targetHost;
    quint16 targetPort = 0;

    bool operator==(const PortForward &) const = default;
    // "Local 8080 → db:5432"
    QString description() const;
    // The port opens on the SSH server (Remote, ReverseDynamic), not here.
    bool listensOnServer() const { return type == Type::Remote || type == Type::ReverseDynamic; }
    // It goes to one destination (host and port, or a socket path).
    bool hasTarget() const { return type == Type::Local || type == Type::Remote || type == Type::Socket; }
    // The ssh option for it ("-L 8080:db:5432"); empty for the HTTP proxy,
    // which ssh does not have.
    QString sshOption() const;
    static QString typeName(Type type);
};

QList<PortForward> portForwardsFromYaml(const YAML::Node &node);
YAML::Node portForwardsToYaml(const QList<PortForward> &forwards);
PortForward portForwardFromYaml(const YAML::Node &node);
YAML::Node portForwardToYaml(const PortForward &forward);

// Edits the fields of one forward (used inside dialogs).
class PortForwardEditor final : public QWidget
{
    Q_OBJECT

public:
    explicit PortForwardEditor(const PortForward &value, QWidget *parent = nullptr);
    PortForward value() const;
    // An explanation of what is missing, or empty when the forward is usable.
    QString problem() const;

private:
    void updateFields();

    QComboBox *m_type = nullptr;
    QSpinBox *m_listenPort = nullptr;
    QCheckBox *m_allowLan = nullptr;
    QLineEdit *m_targetHost = nullptr;
    QSpinBox *m_targetPort = nullptr;
    QLabel *m_help = nullptr;
};

// The SSH session window's "Port forwarding" tab: forwards that open with
// the session, over its connection.
class PortForwardListEditor final : public QWidget
{
    Q_OBJECT

public:
    explicit PortForwardListEditor(const QList<PortForward> &value, QWidget *parent = nullptr);
    QList<PortForward> value() const { return m_forwards; }

private:
    void refresh();
    void edit(int index);

    QList<PortForward> m_forwards;
    QTreeWidget *m_list = nullptr;
};
