// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "tunnels.h"
#include "appdirs.h"
#include "jumphost.h"
#include "tunneleditor.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSplitter>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace {

constexpr int RetrySeconds = 10;
// Forwards a tunnel adds to an open tab's connection are numbered from here
// (the tab's own count from 0), 64 per tunnel.
constexpr int TabForwardBase = 100000;

QString tunnelsFilePath()
{
    return QDir(portableFilePath(QStringLiteral("settings"))).filePath(QStringLiteral("tunnels.yaml"));
}

QString amount(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(double(bytes) / (1024.0 * 1024 * 1024), 0, 'f', 2);
}

QString duration(qint64 seconds)
{
    if (seconds < 0)
        return {};
    if (seconds < 60)
        return QStringLiteral("%1 s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1 m").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1 h %2 m").arg(seconds / 3600).arg((seconds % 3600) / 60);
    return QStringLiteral("%1 d %2 h").arg(seconds / 86400).arg((seconds % 86400) / 3600);
}

// The state as a shape and a colour: filled dot running, triangle working
// or retrying, cross failed, ring stopped.
QIcon stateIcon(TunnelRunner::State state)
{
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    switch (state) {
    case TunnelRunner::State::Running:
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x2e, 0x7d, 0x32));
        painter.drawEllipse(QRectF(7, 7, 18, 18));
        break;
    case TunnelRunner::State::Waking:
    case TunnelRunner::State::Connecting:
    case TunnelRunner::State::Retrying: {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xb2, 0x6a, 0x00));
        const QPointF points[] = {{16, 5}, {28, 26}, {4, 26}};
        painter.drawPolygon(points, 3);
        break;
    }
    case TunnelRunner::State::Failed:
        painter.setPen(QPen(QColor(0xc6, 0x28, 0x28), 5, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(8, 8, 24, 24);
        painter.drawLine(24, 8, 8, 24);
        break;
    case TunnelRunner::State::Stopped:
        painter.setPen(QPen(QColor(0x8a, 0x8a, 0x85), 3));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(8, 8, 16, 16));
        break;
    }
    return QIcon(pixmap);
}

QString stateText(const TunnelRunner *runner)
{
    switch (runner->state()) {
    case TunnelRunner::State::Waking:
        return QStringLiteral("Waking");
    case TunnelRunner::State::Connecting:
        return QStringLiteral("Connecting");
    case TunnelRunner::State::Running:
        return QStringLiteral("Running");
    case TunnelRunner::State::Retrying:
        return QStringLiteral("Retrying in %1 s").arg(runner->secondsToRetry());
    case TunnelRunner::State::Failed:
        return QStringLiteral("Failed");
    case TunnelRunner::State::Stopped:
        break;
    }
    return QStringLiteral("Stopped");
}

// What a new tunnel of a kind starts with.
PortForward defaultForward(PortForward::Type type)
{
    PortForward forward;
    forward.type = type;
    switch (type) {
    case PortForward::Type::Local:
        forward.listenPort = 8080;
        forward.targetHost = QStringLiteral("localhost");
        forward.targetPort = 80;
        break;
    case PortForward::Type::Remote:
        forward.listenPort = 8080;
        forward.targetHost = QStringLiteral("localhost");
        forward.targetPort = 3000;
        break;
    case PortForward::Type::Dynamic:
    case PortForward::Type::ReverseDynamic:
        forward.listenPort = 1080;
        break;
    case PortForward::Type::Socket:
        forward.listenPort = 2375;
        forward.targetHost = QStringLiteral("/var/run/docker.sock");
        break;
    case PortForward::Type::HttpProxy:
        forward.listenPort = 3128;
        break;
    }
    return forward;
}

// What is missing for a forward to work, or empty.
QString forwardProblem(const PortForward &forward)
{
    if (forward.listenPort == 0)
        return QStringLiteral("Choose a port.");
    if (forward.hasTarget() && forward.targetHost.isEmpty())
        return forward.type == PortForward::Type::Socket ? QStringLiteral("Enter the socket's path.")
                                                         : QStringLiteral("Enter the destination.");
    if (forward.type == PortForward::Type::Socket && !forward.targetHost.startsWith(QLatin1Char('/')))
        return QStringLiteral("The socket's path starts with /.");
    if (forward.hasTarget() && forward.type != PortForward::Type::Socket && forward.targetPort == 0)
        return QStringLiteral("Choose the destination's port.");
    return {};
}

}

QList<SavedTunnel> loadTunnels()
{
    QList<SavedTunnel> tunnels;
    try {
        const auto document = YAML::LoadFile(tunnelsFilePath().toStdString());
        for (const auto &node : document["tunnels"]) {
            SavedTunnel tunnel;
            tunnel.id = QString::fromStdString(node["id"].as<std::string>(""));
            if (tunnel.id.isEmpty())
                tunnel.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            tunnel.name = QString::fromStdString(node["name"].as<std::string>(""));
            tunnel.server = QString::fromStdString(node["server"].as<std::string>(""));
            if (node["via"] && node["via"].IsSequence()) {
                for (const auto &hop : node["via"])
                    tunnel.via << QString::fromStdString(hop.as<std::string>(""));
            }
            // Older files have one "forward".
            tunnel.forwards = portForwardsFromYaml(node["forwards"]);
            if (tunnel.forwards.isEmpty() && node["forward"])
                tunnel.forwards << portForwardFromYaml(node["forward"]);
            tunnel.autoStart = node["auto_start"].as<bool>(false);
            tunnel.reconnect = node["reconnect"].as<bool>(true);
            tunnel.reuseTab = node["reuse_tab"].as<bool>(true);
            tunnel.wake = wakeOnLanFromYaml(node["wake_on_lan"]);
            tunnel.waitSeconds = qBound(0, node["wait_seconds"].as<int>(0), 600);
            tunnels.append(tunnel);
        }
    } catch (const YAML::Exception &) {
        // No tunnels yet, or an unreadable file: start with none.
    }
    return tunnels;
}

void saveTunnels(const QList<SavedTunnel> &tunnels)
{
    const auto path = tunnelsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;
    YAML::Node list(YAML::NodeType::Sequence);
    for (const auto &tunnel : tunnels) {
        YAML::Node node;
        node["id"] = tunnel.id.toStdString();
        node["name"] = tunnel.name.toStdString();
        node["server"] = tunnel.server.toStdString();
        if (!tunnel.via.isEmpty()) {
            YAML::Node via(YAML::NodeType::Sequence);
            for (const auto &hop : tunnel.via)
                via.push_back(hop.toStdString());
            node["via"] = via;
        }
        node["forwards"] = portForwardsToYaml(tunnel.forwards);
        if (tunnel.autoStart)
            node["auto_start"] = true;
        if (!tunnel.reconnect)
            node["reconnect"] = false;
        if (!tunnel.reuseTab)
            node["reuse_tab"] = false;
        if (!tunnel.wake.isEmpty())
            node["wake_on_lan"] = wakeOnLanToYaml(tunnel.wake);
        if (tunnel.waitSeconds > 0)
            node["wait_seconds"] = tunnel.waitSeconds;
        list.push_back(node);
    }
    YAML::Node document;
    document["tunnels"] = list;
    YAML::Emitter emitter;
    emitter << document;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

QString tunnelCommand(const SavedTunnel &tunnel, const TunnelServerInfo &server)
{
    QStringList parts{QStringLiteral("ssh"), QStringLiteral("-N")};
    if (!server.jumps.isEmpty())
        parts << QStringLiteral("-J") << server.jumps.join(QLatin1Char(','));
    if (server.port != 22 && server.port != 0)
        parts << QStringLiteral("-p") << QString::number(server.port);
    QStringList notes;
    for (const auto &forward : tunnel.forwards) {
        const QString option = forward.sshOption();
        if (option.isEmpty())
            notes << QStringLiteral("the HTTP proxy on %1 is OpsRemote's own (ssh has -D for SOCKS)")
                         .arg(forward.listenPort);
        else
            parts << option;
    }
    const QString host = server.valid ? server.host : QStringLiteral("server");
    parts << (server.user.isEmpty() ? host : QStringLiteral("%1@%2").arg(server.user, host));
    QString command = parts.join(QLatin1Char(' '));
    if (!notes.isEmpty())
        command += QStringLiteral("   # ") + notes.join(QStringLiteral("; "));
    return command;
}

QList<SavedTunnel> tunnelsFromSshConfig(const QString &path, QString *problem)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *problem = QStringLiteral("Cannot read %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return {};
    }
    struct Block
    {
        QString alias;
        QString hostName;
        QString user;
        quint16 port = 0;
        QStringList jumps;
        QList<PortForward> forwards;
    };
    QList<Block> blocks;
    Block *current = nullptr;
    bool includes = false;
    // "[bind:]port" → port and whether other computers may connect.
    const auto listen = [](QString text, quint16 *port, bool *lan) {
        *lan = false;
        if (text.startsWith(QLatin1Char('['))) {
            const int close = text.indexOf(QLatin1Char(']'));
            const QString bind = text.mid(1, close - 1);
            *lan = bind != QStringLiteral("::1") && !bind.isEmpty();
            text = text.mid(close + 2);
        } else if (text.contains(QLatin1Char(':'))) {
            const QString bind = text.section(QLatin1Char(':'), 0, -2);
            *lan = bind == QStringLiteral("*") || bind == QStringLiteral("0.0.0.0") || bind.isEmpty();
            text = text.section(QLatin1Char(':'), -1);
        }
        bool ok = false;
        const uint value = text.toUInt(&ok);
        *port = ok && value > 0 && value < 65536 ? quint16(value) : 0;
        return *port != 0;
    };
    // "host:port", "[v6]:port" or "/socket".
    const auto target = [](const QString &text, QString *host, quint16 *port) {
        if (text.startsWith(QLatin1Char('/'))) {
            *host = text;
            *port = 0;
            return true;
        }
        QString rest = text;
        if (rest.startsWith(QLatin1Char('['))) {
            const int close = rest.indexOf(QLatin1Char(']'));
            *host = rest.mid(1, close - 1);
            rest = rest.mid(close + 1);
        } else {
            *host = rest.section(QLatin1Char(':'), 0, -2);
            rest = QLatin1Char(':') + rest.section(QLatin1Char(':'), -1);
        }
        bool ok = false;
        const uint value = rest.mid(1).toUInt(&ok);
        *port = ok && value > 0 && value < 65536 ? quint16(value) : 0;
        return !host->isEmpty() && *port != 0;
    };
    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        // "Key value", "Key=value", or "Key = value".
        static const QRegularExpression split(QStringLiteral("^(\\S+?)\\s*(=|\\s)\\s*(.*)$"));
        const auto match = split.match(line);
        if (!match.hasMatch())
            continue;
        const QString key = match.captured(1).toLower();
        const QString value = match.captured(3).trimmed().remove(QLatin1Char('"'));
        const QStringList words = value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (key == QStringLiteral("host")) {
            current = nullptr;
            // Patterns (*, ?, !) describe many hosts, not one tunnel.
            const QString alias = words.value(0);
            if (words.size() == 1 && !alias.contains(QRegularExpression(QStringLiteral("[*?!]")))) {
                blocks.append({alias, {}, {}, 0, {}, {}});
                current = &blocks.last();
            }
            continue;
        }
        if (key == QStringLiteral("match")) {
            current = nullptr;
            continue;
        }
        if (key == QStringLiteral("include"))
            includes = true;
        if (!current)
            continue;
        if (key == QStringLiteral("hostname"))
            current->hostName = value;
        else if (key == QStringLiteral("user"))
            current->user = value;
        else if (key == QStringLiteral("port"))
            current->port = quint16(value.toUInt());
        else if (key == QStringLiteral("proxyjump") && value.compare(QStringLiteral("none"), Qt::CaseInsensitive) != 0)
            current->jumps = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
        else if (key == QStringLiteral("localforward") || key == QStringLiteral("remoteforward")
                 || key == QStringLiteral("dynamicforward")) {
            PortForward forward;
            if (!listen(words.value(0), &forward.listenPort, &forward.allowLan))
                continue;
            if (key == QStringLiteral("dynamicforward")) {
                forward.type = PortForward::Type::Dynamic;
            } else if (key == QStringLiteral("remoteforward") && words.size() < 2) {
                forward.type = PortForward::Type::ReverseDynamic;
            } else {
                if (!target(words.value(1), &forward.targetHost, &forward.targetPort))
                    continue;
                forward.type = key == QStringLiteral("remoteforward") ? PortForward::Type::Remote
                               : forward.targetPort == 0              ? PortForward::Type::Socket
                                                                      : PortForward::Type::Local;
                // A socket on this side of a remote forward is not supported.
                if (forward.type == PortForward::Type::Remote && forward.targetPort == 0)
                    continue;
            }
            current->forwards << forward;
        }
    }
    QList<SavedTunnel> tunnels;
    for (const auto &block : std::as_const(blocks)) {
        if (block.forwards.isEmpty())
            continue;
        SavedTunnel tunnel;
        tunnel.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        tunnel.name = block.alias;
        const QString host = block.hostName.isEmpty() ? block.alias : block.hostName;
        tunnel.server = (block.user.isEmpty() ? host : QStringLiteral("%1@%2").arg(block.user, host))
                        + (block.port && block.port != 22 ? QStringLiteral(":%1").arg(block.port) : QString());
        // Hops named by an alias of this file use its address.
        for (const auto &jump : block.jumps) {
            QString hop = jump.trimmed();
            for (const auto &other : std::as_const(blocks)) {
                if (other.alias == hop && !other.hostName.isEmpty())
                    hop = (other.user.isEmpty() ? other.hostName : QStringLiteral("%1@%2").arg(other.user, other.hostName))
                          + (other.port && other.port != 22 ? QStringLiteral(":%1").arg(other.port) : QString());
            }
            tunnel.via << hop;
        }
        tunnel.forwards = block.forwards;
        tunnels << tunnel;
    }
    if (includes)
        *problem = QStringLiteral("Files named by Include are not read.");
    return tunnels;
}

// ---- Runner ----

TunnelRunner::TunnelRunner(QWidget *dialogParent)
    : QObject(dialogParent), m_dialogParent(dialogParent)
{
    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, [this] {
        if (m_wanted)
            connectNow();
    });
}

TunnelRunner::~TunnelRunner()
{
    m_server.password.fill(QChar(0));
    for (auto &jump : m_server.jumps)
        jump.password.fill(QChar(0));
    closeConnections();
}

void TunnelRunner::start(SshConnectionSettings server, const SavedTunnel &tunnel, SshSession *tab)
{
    closeConnections();
    m_server = std::move(server);
    m_server.openShell = false;
    m_server.forwards = tunnel.forwards;
    m_tunnel = tunnel;
    m_forwardStates = QList<ForwardState>(tunnel.forwards.size());
    m_wanted = true;
    m_wasRunning = false;
    m_running.invalidate();
    if (tab) {
        // Over an open tab's connection: no log-on of our own.
        static int nextBase = 0;
        m_firstIndex = TabForwardBase + 64 * (nextBase++ % 1000);
        setState(State::Connecting, QStringLiteral("Using the open tab's connection…"));
        attach(tab, m_firstIndex);
        m_tab = tab;
        // A closed tab takes its connection along without a word: carry on
        // over a connection of our own at once.
        connect(tab, &QObject::destroyed, this, [this] {
            if (!m_wanted)
                return;
            m_running.invalidate();
            m_forwardStates = QList<ForwardState>(m_tunnel.forwards.size());
            setState(State::Retrying, QStringLiteral("The tab whose connection the tunnel used closed."));
            m_retry->start(0);
        });
        tab->addForwards(m_tunnel.forwards, m_firstIndex);
        return;
    }
    if (m_tunnel.wake.isSet()) {
        QString error;
        if (!m_tunnel.wake.send(&error)) {
            setState(State::Failed, QStringLiteral("Could not wake the server: %1").arg(error));
            m_wanted = false;
            return;
        }
        setState(State::Waking, QStringLiteral("Woke the server; waiting for it to answer…"));
        waitForServer(m_tunnel.waitSeconds > 0 ? m_tunnel.waitSeconds : m_tunnel.wake.effectiveWait());
        return;
    }
    if (m_tunnel.waitSeconds > 0 && m_server.jumps.empty()) {
        setState(State::Waking, QStringLiteral("Waiting for the server to answer…"));
        waitForServer(m_tunnel.waitSeconds);
        return;
    }
    connectNow();
}

// Tries the server's SSH port every two seconds (directly reachable servers
// only), then connects either way.
void TunnelRunner::waitForServer(int secondsLeft)
{
    if (!m_wanted)
        return;
    if (secondsLeft <= 0 || !m_server.jumps.empty() || m_server.proxy.isSet()) {
        connectNow();
        return;
    }
    auto *socket = new QTcpSocket(this);
    auto *timeout = new QTimer(socket);
    timeout->setSingleShot(true);
    const auto next = [this, socket, secondsLeft](bool answered) {
        socket->disconnect(this);
        socket->deleteLater();
        if (!m_wanted)
            return;
        if (answered)
            connectNow();
        else
            QTimer::singleShot(2000, this, [this, secondsLeft] { waitForServer(secondsLeft - 4); });
    };
    connect(socket, &QTcpSocket::connected, this, [next] { next(true); });
    connect(socket, &QTcpSocket::errorOccurred, this, [next] { next(false); });
    connect(timeout, &QTimer::timeout, this, [next] { next(false); });
    timeout->start(2000);
    socket->connectToHost(m_server.host, m_server.port);
}

void TunnelRunner::connectNow()
{
    closeConnections();
    m_forwardStates = QList<ForwardState>(m_tunnel.forwards.size());
    setState(State::Connecting, QStringLiteral("Connecting to %1…").arg(m_server.host));
    if (m_server.jumps.empty()) {
        connectServer(0);
        return;
    }
    m_chain = new JumpChain(m_dialogParent);
    connect(m_chain, &JumpChain::ready, this, &TunnelRunner::connectServer);
    connect(m_chain, &JumpChain::failed, this, &TunnelRunner::ended);
    connect(m_chain, &JumpChain::statusMessage, this, [this](const QString &message) {
        setState(m_state, message);
    });
    m_chain->open(m_server.jumps, m_server.host, m_server.port);
}

void TunnelRunner::stop()
{
    m_wanted = false;
    m_retry->stop();
    closeConnections();
    setState(State::Stopped, QString());
}

void TunnelRunner::closeConnections()
{
    if (m_tab) {
        m_tab->removeForwards(m_firstIndex, int(m_tunnel.forwards.size()));
        m_tab->disconnect(this);
        m_tab = nullptr;
    }
    // The ports close at once (the connection itself ends on its thread
    // later), so the tunnel can start again on them straight away.
    if (m_ssh)
        m_ssh->removeForwards(0, int(m_tunnel.forwards.size()));
    for (QObject *object : {static_cast<QObject *>(m_chain.data()),
                            static_cast<QObject *>(m_ssh.data())}) {
        if (object) {
            object->disconnect(this);
            object->deleteLater();
        }
    }
    m_chain = nullptr;
    m_ssh = nullptr;
    m_running.invalidate();
}

void TunnelRunner::connectServer(quint16 viaLocalPort)
{
    auto settings = m_server;
    settings.jumps.clear();
    settings.viaLocalPort = viaLocalPort;
    const QString server = QStringLiteral("tunnel server %1").arg(m_server.host);
    m_ssh = new SshSession(this);
    connect(m_ssh, &SshSession::hostKeyVerificationRequired, this,
            [this, server](SshHostKeyStatus status, const QString &keyType,
                           const QString &fingerprint) {
                askSshHostKey(m_dialogParent, m_ssh, server, status, keyType, fingerprint);
            });
    connect(m_ssh, &SshSession::inputRequired, this,
            [this, server](const QString &instruction, const QStringList &prompts,
                           const QList<bool> &echo) {
                askSshLogOn(m_dialogParent, m_ssh, server, instruction, prompts, echo);
            });
    attach(m_ssh, 0);
    m_ssh->connectToHost(settings);
    settings.password.fill(QChar(0));
}

// Follows a connection's forwards numbered firstIndex onwards.
void TunnelRunner::attach(SshSession *ssh, int firstIndex)
{
    const int count = int(m_tunnel.forwards.size());
    const auto mine = [firstIndex, count](int index) { return index >= firstIndex && index < firstIndex + count; };
    connect(ssh, &SshSession::forwardStatus, this,
            [this, mine, firstIndex](int index, bool listening, const QString &message) {
                if (!mine(index))
                    return;
                auto &state = m_forwardStates[index - firstIndex];
                state.listening = listening;
                state.message = message;
                int open = 0;
                int answered = 0;
                for (const auto &each : std::as_const(m_forwardStates)) {
                    open += each.listening ? 1 : 0;
                    answered += each.listening || !each.message.isEmpty() ? 1 : 0;
                }
                if (answered < m_forwardStates.size()) {
                    emit changed();
                    return;
                }
                if (open == 0) {
                    // A port in use or refused by the server will not fix itself.
                    m_wanted = false;
                    closeConnections();
                    setState(State::Failed, message);
                    return;
                }
                m_wasRunning = true;
                if (!m_running.isValid())
                    m_running.start();
                setState(State::Running, open == m_forwardStates.size()
                                             ? QString()
                                             : QStringLiteral("%1 of %2 forwards could not start.")
                                                   .arg(m_forwardStates.size() - open).arg(m_forwardStates.size()));
            });
    connect(ssh, &SshSession::forwardStats, this,
            [this, mine, firstIndex](int index, int connections, qint64 received, qint64 sent) {
                if (!mine(index))
                    return;
                auto &state = m_forwardStates[index - firstIndex];
                state.connections = connections;
                state.received = received;
                state.sent = sent;
                emit changed();
            });
    connect(ssh, &SshSession::forwardConnectionFailed, this, [this, mine](int index, const QString &message) {
        if (mine(index))
            setState(m_state, message);
    });
    connect(ssh, &SshSession::probeFinished, this,
            [this](int id, bool reachable, const QString &message, int milliseconds) {
                if (!m_probes.contains(id))
                    return;
                emit probeFinished(m_probes.take(id), reachable, message, milliseconds);
            });
    connect(ssh, &SshSession::disconnected, this, [this](const QString &reason, bool error) {
        // An open tab that closes takes its forwards along; our own
        // connection takes over (when the tunnel reconnects).
        ended(m_tab ? QStringLiteral("The tab whose connection the tunnel used closed.") : reason,
              error || m_tab);
    });
}

void TunnelRunner::probe(int forward)
{
    SshSession *ssh = m_tab ? m_tab.data() : m_ssh.data();
    if (!ssh || forward < 0 || forward >= m_tunnel.forwards.size() || m_state != State::Running) {
        emit probeFinished(forward, false, QStringLiteral("the tunnel is not running"), 0);
        return;
    }
    const auto &rule = m_tunnel.forwards[forward];
    const int id = m_nextProbe++;
    m_probes.insert(id, forward);
    ssh->probe(id, rule.targetHost, rule.type == PortForward::Type::Socket ? 0 : rule.targetPort);
}

void TunnelRunner::ended(const QString &reason, bool error)
{
    closeConnections();
    if (m_wanted && error && m_tunnel.reconnect && m_wasRunning) {
        setState(State::Retrying, reason);
        m_retry->start(RetrySeconds * 1000);
        return;
    }
    m_wanted = false;
    setState(error ? State::Failed : State::Stopped, reason);
}

void TunnelRunner::setState(State state, const QString &message)
{
    m_state = state;
    m_message = message;
    emit changed();
}

qint64 TunnelRunner::uptime() const
{
    return m_state == State::Running && m_running.isValid() ? m_running.elapsed() / 1000 : -1;
}

int TunnelRunner::secondsToRetry() const
{
    return m_retry->isActive() ? (m_retry->remainingTime() + 999) / 1000 : 0;
}

qint64 TunnelRunner::totalReceived() const
{
    qint64 total = 0;
    for (const auto &state : m_forwardStates)
        total += state.received;
    return total;
}

qint64 TunnelRunner::totalSent() const
{
    qint64 total = 0;
    for (const auto &state : m_forwardStates)
        total += state.sent;
    return total;
}

int TunnelRunner::totalConnections() const
{
    int total = 0;
    for (const auto &state : m_forwardStates)
        total += state.connections;
    return total;
}

// ---- Window ----

TunnelsWindow::TunnelsWindow(ServerResolver resolver, SessionChoices sessions,
                             NewEntry::SessionMaker makeSession, ServerDescriber describer,
                             TabFinder tabs, PortScanner scanner, QWidget *parent)
    : QWidget(parent, Qt::Window), m_resolver(std::move(resolver)), m_sessions(std::move(sessions)),
      m_makeSession(std::move(makeSession)), m_describer(std::move(describer)), m_tabs(std::move(tabs)), m_scanner(std::move(scanner))
{
    setWindowTitle(QStringLiteral("SSH Tunnels"));
    resize(1240, 820);
    auto *layout = new QVBoxLayout(this);

    auto *toolbar = new QHBoxLayout;
    auto *add = new QToolButton(this);
    add->setText(QStringLiteral("+ New Tunnel"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto *kinds = new QMenu(add);
    for (const auto &[type, label] : QList<QPair<PortForward::Type, QString>>{
             {PortForward::Type::Local, QStringLiteral("Local: a port here reaches a host behind the server")},
             {PortForward::Type::Remote, QStringLiteral("Remote: a port on the server reaches a host here")},
             {PortForward::Type::Dynamic, QStringLiteral("SOCKS proxy through the server")},
             {PortForward::Type::ReverseDynamic, QStringLiteral("Reverse SOCKS: a proxy on the server through here")},
             {PortForward::Type::Socket, QStringLiteral("Socket: a port here reaches a Unix socket on the server")},
             {PortForward::Type::HttpProxy, QStringLiteral("HTTP proxy through the server")}})
        kinds->addAction(label, this, [this, type] { newTunnel(type); });
    add->setMenu(kinds);
    auto *import = new QPushButton(QStringLiteral("Import from ~/.ssh/config…"), this);
    auto *startAll = new QPushButton(QStringLiteral("▶ Start All"), this);
    auto *stopAll = new QPushButton(QStringLiteral("■ Stop All"), this);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("Filter: name, host, port"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(260);
    toolbar->addWidget(add);
    toolbar->addWidget(import);
    toolbar->addSpacing(12);
    toolbar->addWidget(startAll);
    toolbar->addWidget(stopAll);
    toolbar->addStretch();
    toolbar->addWidget(m_filter);
    layout->addLayout(toolbar);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_splitter = splitter;
    m_list = new QTreeWidget(splitter);
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setHeaderLabels({QStringLiteral("State"), QStringLiteral("Name"), QStringLiteral("Kind"),
                             QStringLiteral("Listens on"), QStringLiteral("Reaches"), QStringLiteral("Through"),
                             QStringLiteral("Conn."), QStringLiteral("Traffic ↓ / ↑"), QStringLiteral("Up for")});
    m_list->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_list->header()->setStretchLastSection(false);
    auto *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *bottom = new QWidget(scroll);
    m_bottom = bottom;
    bottom->installEventFilter(this);
    auto *bottomLayout = new QVBoxLayout(bottom);
    m_editor = new TunnelEditor(m_sessions, m_makeSession, m_describer, bottom);
    m_empty = new QLabel(QStringLiteral("Choose a tunnel to see and edit it, or make one with + New Tunnel."), bottom);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setForegroundRole(QPalette::PlaceholderText);
    bottomLayout->addWidget(m_editor);
    bottomLayout->addWidget(m_empty);
    bottomLayout->addStretch();
    scroll->setWidget(bottom);
    splitter->addWidget(m_list);
    splitter->addWidget(scroll);
    // A taller window gives the list more rows; the editor keeps its height.
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);
    layout->addWidget(splitter, 1);

    connect(import, &QPushButton::clicked, this, &TunnelsWindow::importSshConfig);
    connect(startAll, &QPushButton::clicked, this, [this] {
        for (int index = 0; index < m_tunnels.size(); ++index) {
            const auto state = m_runners[index]->state();
            if (state == TunnelRunner::State::Stopped || state == TunnelRunner::State::Failed)
                startTunnel(index);
        }
    });
    connect(stopAll, &QPushButton::clicked, this, [this] {
        for (int index = 0; index < m_tunnels.size(); ++index)
            stopTunnel(index);
    });
    connect(m_filter, &QLineEdit::textChanged, this, &TunnelsWindow::refresh);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &TunnelsWindow::refreshEditor);
    connect(m_list, &QTreeWidget::customContextMenuRequested, this, &TunnelsWindow::showMenu);
    connect(m_editor, &TunnelEditor::edited, this, [this](const SavedTunnel &tunnel) {
        const int index = currentIndex();
        if (index < 0 || m_tunnels[index].id != tunnel.id)
            return;
        m_tunnels[index] = tunnel;
        save();
        refresh();
    });
    connect(m_editor, &TunnelEditor::startStopRequested, this, [this] {
        const int index = currentIndex();
        if (index < 0)
            return;
        const auto state = m_runners[index]->state();
        if (state == TunnelRunner::State::Stopped || state == TunnelRunner::State::Failed)
            startTunnel(index);
        else
            stopTunnel(index);
    });
    connect(m_editor, &TunnelEditor::deleteRequested, this, [this] { removeTunnel(currentIndex()); });
    connect(m_editor, &TunnelEditor::checksRequested, this, [this] { runChecks(currentIndex()); });
    connect(m_editor, &TunnelEditor::scanRequested, this, [this](const QString &host) {
        if (m_scanner)
            m_scanner(host);
    });

    // Live columns: traffic, connections, time running.
    m_ticker = new QTimer(this);
    connect(m_ticker, &QTimer::timeout, this, &TunnelsWindow::refresh);
    m_ticker->start(1000);

    m_tunnels = loadTunnels();
    for (qsizetype i = 0; i < m_tunnels.size(); ++i)
        addRunner();
    refresh();
    select(m_tunnels.isEmpty() ? -1 : 0);
}

void TunnelsWindow::addRunner()
{
    auto *runner = new TunnelRunner(this);
    connect(runner, &TunnelRunner::changed, this, [this, runner] {
        refresh();
        const int index = currentIndex();
        if (index >= 0 && m_runners[index] == runner)
            m_editor->setRunner(runner);
    });
    connect(runner, &TunnelRunner::probeFinished, this,
            [this, runner](int forward, bool reachable, const QString &message, int milliseconds) {
                const int index = currentIndex();
                if (index < 0 || m_runners[index] != runner || forward != m_editor->currentForward())
                    return;
                m_editor->setCheck(TunnelEditor::Target,
                                   reachable ? TunnelCard::Level::Ok : TunnelCard::Level::Problem,
                                   reachable ? QStringLiteral("The server reaches it (%1 ms)").arg(milliseconds)
                                             : QStringLiteral("The server cannot reach it: %1").arg(message));
            });
    m_runners.append(runner);
}

void TunnelsWindow::startAutomatic()
{
    for (qsizetype i = 0; i < m_tunnels.size(); ++i) {
        if (m_tunnels[i].autoStart)
            startTunnel(int(i));
    }
}

int TunnelsWindow::runningCount() const
{
    int count = 0;
    for (const auto *runner : m_runners)
        count += runner->state() == TunnelRunner::State::Running ? 1 : 0;
    return count;
}

int TunnelsWindow::currentIndex() const
{
    auto *item = m_list->currentItem();
    return item ? m_list->indexOfTopLevelItem(item) : -1;
}

void TunnelsWindow::select(int index)
{
    if (index >= 0 && index < m_list->topLevelItemCount())
        m_list->setCurrentItem(m_list->topLevelItem(index));
    refreshEditor();
}

void TunnelsWindow::save()
{
    saveTunnels(m_tunnels);
}

void TunnelsWindow::refresh()
{
    const QString filter = m_filter->text().trimmed();
    const QSignalBlocker blocker(m_list);
    const int current = currentIndex();
    // More or fewer tunnels: the list may want another height.
    if (m_list->topLevelItemCount() != m_tunnels.size())
        QTimer::singleShot(0, this, [this] { fitEditor(); });
    while (m_list->topLevelItemCount() > m_tunnels.size())
        delete m_list->takeTopLevelItem(m_list->topLevelItemCount() - 1);
    for (qsizetype i = 0; i < m_tunnels.size(); ++i) {
        auto *item = m_list->topLevelItem(int(i));
        if (!item) {
            item = new QTreeWidgetItem;
            m_list->addTopLevelItem(item);
        }
        const auto &tunnel = m_tunnels[i];
        const auto *runner = m_runners[i];
        const TunnelServerInfo server = m_describer(tunnel.server, tunnel.via);
        QStringList kinds, listens, reaches;
        for (const auto &forward : tunnel.forwards) {
            kinds << PortForward::typeName(forward.type);
            listens << QStringLiteral("%1%2").arg(forward.listensOnServer() ? QStringLiteral("server:") : QString())
                           .arg(forward.listenPort);
            reaches << (forward.type == PortForward::Type::Socket ? forward.targetHost
                        : forward.hasTarget() ? QStringLiteral("%1:%2").arg(forward.targetHost).arg(forward.targetPort)
                                              : QStringLiteral("any address"));
        }
        kinds.removeDuplicates();
        QString through = server.valid ? server.label : tunnel.server;
        if (!server.jumps.isEmpty())
            through = QStringLiteral("%1 → %2").arg(server.jumps.join(QStringLiteral(" → ")), through);
        const bool active = runner->state() == TunnelRunner::State::Running;
        item->setIcon(0, stateIcon(runner->state()));
        item->setText(0, stateText(runner));
        item->setToolTip(0, runner->message());
        item->setText(1, tunnel.name.isEmpty() ? QStringLiteral("(no name)") : tunnel.name);
        item->setText(2, kinds.join(QStringLiteral(", ")));
        item->setText(3, listens.join(QStringLiteral(", ")));
        item->setText(4, reaches.join(QStringLiteral(", ")));
        item->setText(5, through);
        item->setText(6, active ? QString::number(runner->totalConnections()) : QString());
        item->setText(7, active ? QStringLiteral("%1 / %2").arg(amount(runner->totalReceived()), amount(runner->totalSent()))
                                : QString());
        item->setText(8, duration(runner->uptime()));
        const QString haystack = QStringList{item->text(1), item->text(3), item->text(4), item->text(5)}.join(QLatin1Char(' '));
        item->setHidden(!filter.isEmpty() && !haystack.contains(filter, Qt::CaseInsensitive));
    }
    if (current >= 0 && current < m_list->topLevelItemCount())
        m_list->setCurrentItem(m_list->topLevelItem(current));
}

namespace {
// Rows the list of tunnels always shows, empty ones too; more scroll.
constexpr int MaximumTunnelRows = 10;
}

bool TunnelsWindow::eventFilter(QObject *watched, QEvent *event)
{
    // The editor's content changed size (another tunnel, kind, jump host,
    // More Options): fit once things settle.
    if (watched == m_bottom && event->type() == QEvent::LayoutRequest && !m_fitPending) {
        m_fitPending = true;
        QTimer::singleShot(0, this, [this] {
            m_fitPending = false;
            fitEditor();
        });
    }
    return QWidget::eventFilter(watched, event);
}

void TunnelsWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_sized)
        return;
    m_sized = true;
    // The first time: as tall as ten rows of the list and the editor need,
    // within the screen.
    const int rows = MaximumTunnelRows;
    const int wanted = height() - m_splitter->height() + listHeightFor(rows) + m_splitter->handleWidth()
                       + m_bottom->sizeHint().height();
    int limit = wanted;
    if (const auto *screen = this->screen())
        limit = int(screen->availableGeometry().height() * 0.9);
    resize(width(), qMin(wanted, limit));
    fitEditor();
}

int TunnelsWindow::listHeightFor(int rows) const
{
    const int row = m_list->sizeHintForRow(0) > 0 ? m_list->sizeHintForRow(0) : fontMetrics().height() + 6;
    return m_list->header()->sizeHint().height() + rows * row + 2 * m_list->frameWidth() + 2;
}

void TunnelsWindow::fitEditor(bool mayGrow)
{
    const int total = m_splitter->height() - m_splitter->handleWidth();
    if (total <= 0)
        return;
    // The list ten rows tall (more scroll), the editor as tall as its
    // content. Short of room: a taller window, within the screen, but only
    // when the editor grew (a window made smaller by hand stays so until
    // then); only then does the list give up rows, down to three.
    const int needed = m_bottom->sizeHint().height();
    const int wanted = listHeightFor(MaximumTunnelRows) + needed;
    const bool grew = wanted > m_fittedFor;
    m_fittedFor = wanted;
    if (wanted > total && grew && mayGrow && m_sized
        && !(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))) {
        int limit = height() + wanted - total;
        if (const auto *screen = this->screen())
            limit = qMin(limit, screen->availableGeometry().height() * 9 / 10);
        if (limit > height()) {
            resize(width(), limit);
            // Fitted again once the new height is in place.
            QTimer::singleShot(50, this, [this] { fitEditor(false); });
            return;
        }
    }
    const int editor = qMax(0, qMin(needed, total - listHeightFor(3)));
    m_splitter->setSizes({total - editor, editor});
}

void TunnelsWindow::refreshEditor()
{
    const int index = currentIndex();
    m_editor->setVisible(index >= 0);
    m_empty->setVisible(index < 0);
    if (index < 0)
        return;
    m_editor->setTunnel(m_tunnels[index]);
    m_editor->setRunner(m_runners[index]);
}

void TunnelsWindow::newTunnel(PortForward::Type type)
{
    SavedTunnel tunnel;
    tunnel.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    tunnel.name = QStringLiteral("New %1 tunnel").arg(PortForward::typeName(type));
    const auto sessions = m_sessions();
    tunnel.server = sessions.isEmpty() ? QString() : JumpHost::SessionPrefix + sessions.first().first;
    tunnel.forwards << defaultForward(type);
    m_tunnels.append(tunnel);
    addRunner();
    save();
    refresh();
    select(int(m_tunnels.size()) - 1);
}

void TunnelsWindow::importSshConfig()
{
    QString path = QDir::home().filePath(QStringLiteral(".ssh/config"));
    if (!QFileInfo::exists(path)) {
        path = QFileDialog::getOpenFileName(this, QStringLiteral("SSH Config File"), QDir::homePath());
        if (path.isEmpty())
            return;
    }
    QString problem;
    const auto found = tunnelsFromSshConfig(path, &problem);
    if (found.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Import Tunnels"),
                                 problem.isEmpty() ? QStringLiteral("%1 has no LocalForward, RemoteForward, or "
                                                                    "DynamicForward lines.")
                                                         .arg(QDir::toNativeSeparators(path))
                                                   : problem);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Import Tunnels"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Forwards found in %1:").arg(QDir::toNativeSeparators(path)), &dialog));
    auto *list = new QListWidget(&dialog);
    for (const auto &tunnel : found) {
        QStringList parts;
        for (const auto &forward : tunnel.forwards)
            parts << forward.description();
        auto *item = new QListWidgetItem(QStringLiteral("%1 — %2 via %3")
                                             .arg(tunnel.name, parts.join(QStringLiteral(", ")), tunnel.server),
                                         list);
        item->setCheckState(Qt::Checked);
    }
    layout->addWidget(list);
    if (!problem.isEmpty())
        layout->addWidget(new QLabel(problem, &dialog));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Import"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->checkState() != Qt::Checked)
            continue;
        m_tunnels.append(found[row]);
        addRunner();
    }
    save();
    refresh();
    select(int(m_tunnels.size()) - 1);
}

void TunnelsWindow::startTunnel(int index)
{
    if (index < 0 || index >= m_tunnels.size())
        return;
    const auto &tunnel = m_tunnels[index];
    QString problem = tunnel.server.isEmpty() ? QStringLiteral("Choose the SSH server.") : QString();
    for (const auto &forward : tunnel.forwards) {
        if (problem.isEmpty())
            problem = forwardProblem(forward);
    }
    if (tunnel.forwards.isEmpty())
        problem = QStringLiteral("The tunnel has no forward.");
    if (!problem.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                             QStringLiteral("%1 cannot start: %2").arg(tunnel.name, problem));
        return;
    }
    SshConnectionSettings server;
    if (!m_resolver(tunnel, &server))
        return;
    SshSession *tab = nullptr;
    if (tunnel.reuseTab && tunnel.server.startsWith(JumpHost::SessionPrefix) && m_tabs)
        tab = m_tabs(tunnel.server.mid(JumpHost::SessionPrefix.size()));
    m_runners[index]->start(std::move(server), tunnel, tab);
}

void TunnelsWindow::stopTunnel(int index)
{
    if (index >= 0 && index < m_runners.size())
        m_runners[index]->stop();
}

void TunnelsWindow::removeTunnel(int index)
{
    if (index < 0 || index >= m_tunnels.size())
        return;
    if (QMessageBox::question(this, QStringLiteral("Delete Tunnel"),
                              QStringLiteral("Delete the tunnel %1?").arg(m_tunnels[index].name))
        != QMessageBox::Yes)
        return;
    m_runners[index]->stop();
    delete m_runners.takeAt(index);
    m_tunnels.removeAt(index);
    save();
    refresh();
    select(qMin(index, int(m_tunnels.size()) - 1));
}

void TunnelsWindow::duplicateTunnel(int index)
{
    if (index < 0 || index >= m_tunnels.size())
        return;
    SavedTunnel copy = m_tunnels[index];
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy.name += QStringLiteral(" (copy)");
    copy.autoStart = false;
    for (auto &forward : copy.forwards)
        forward.listenPort = quint16(qMin(65535, forward.listenPort + 1));
    m_tunnels.insert(index + 1, copy);
    // addRunner() appends; the new runner moves next to the original.
    addRunner();
    m_runners.move(m_runners.size() - 1, index + 1);
    save();
    refresh();
    select(index + 1);
}

void TunnelsWindow::showMenu(const QPoint &position)
{
    auto *item = m_list->itemAt(position);
    if (!item)
        return;
    const int index = m_list->indexOfTopLevelItem(item);
    const auto state = m_runners[index]->state();
    const bool active = state != TunnelRunner::State::Stopped && state != TunnelRunner::State::Failed;
    QMenu menu(this);
    if (active)
        menu.addAction(QStringLiteral("Stop"), this, [this, index] { stopTunnel(index); });
    else
        menu.addAction(QStringLiteral("Start"), this, [this, index] { startTunnel(index); });
    menu.addAction(QStringLiteral("Copy Command"), this, [this, index] {
        QApplication::clipboard()->setText(
            tunnelCommand(m_tunnels[index], m_describer(m_tunnels[index].server, m_tunnels[index].via)));
    });
    menu.addAction(QStringLiteral("Duplicate"), this, [this, index] { duplicateTunnel(index); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Delete…"), this, [this, index] { removeTunnel(index); });
    menu.exec(m_list->viewport()->mapToGlobal(position));
}

// The checks of the forward shown: the port here, the server, and the
// destination (through the server, or here for a remote forward).
void TunnelsWindow::runChecks(int index)
{
    if (index < 0 || index >= m_tunnels.size())
        return;
    const auto &tunnel = m_tunnels[index];
    const int shown = m_editor->currentForward();
    if (shown < 0 || shown >= tunnel.forwards.size())
        return;
    const auto &forward = tunnel.forwards[shown];
    auto *runner = m_runners[index];
    const bool running = runner->state() == TunnelRunner::State::Running;

    // 1. The port here (forwards that listen on this computer).
    if (!forward.listensOnServer() && !running) {
        QTcpServer probe;
        const bool free = probe.listen(forward.allowLan ? QHostAddress::Any : QHostAddress::LocalHost,
                                       forward.listenPort);
        m_editor->setCheck(TunnelEditor::Here, free ? TunnelCard::Level::Ok : TunnelCard::Level::Problem,
                           free ? QStringLiteral("Port %1 is free").arg(forward.listenPort)
                                : QStringLiteral("Port %1 is in use here: %2").arg(forward.listenPort)
                                      .arg(probe.errorString()));
    }

    // 2. The server answers (directly reachable servers).
    const TunnelServerInfo server = m_describer(tunnel.server, tunnel.via);
    if (running) {
        m_editor->setCheck(TunnelEditor::Server, TunnelCard::Level::Ok, QStringLiteral("Connected"));
    } else if (!server.valid) {
        m_editor->setCheck(TunnelEditor::Server, TunnelCard::Level::Problem, QStringLiteral("Choose the SSH server"));
    } else if (!server.jumps.isEmpty()) {
        m_editor->setCheck(TunnelEditor::Server, TunnelCard::Level::None,
                           QStringLiteral("Behind jump hosts: checked when the tunnel starts"));
    } else {
        m_editor->setCheck(TunnelEditor::Server, TunnelCard::Level::Working,
                           QStringLiteral("Checking port %1…").arg(server.port));
        auto *socket = new QTcpSocket(this);
        auto *timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        const auto done = [this, socket, port = server.port](bool ok, const QString &why) {
            socket->disconnect(this);
            socket->deleteLater();
            m_editor->setCheck(TunnelEditor::Server, ok ? TunnelCard::Level::Ok : TunnelCard::Level::Problem,
                               ok ? QStringLiteral("Answers on port %1").arg(port)
                                  : QStringLiteral("Does not answer on port %1: %2").arg(port).arg(why));
        };
        connect(socket, &QTcpSocket::connected, this, [done] { done(true, {}); });
        connect(socket, &QTcpSocket::errorOccurred, this, [done, socket] { done(false, socket->errorString()); });
        connect(timeout, &QTimer::timeout, this, [done] { done(false, QStringLiteral("no answer in 5 s")); });
        timeout->start(5000);
        socket->connectToHost(server.host, server.port);
    }

    // 3. The destination.
    if (forward.type == PortForward::Type::Remote) {
        // Its destination is here: check from this computer.
        auto *socket = new QTcpSocket(this);
        auto *timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        const QString target = QStringLiteral("%1:%2").arg(forward.targetHost).arg(forward.targetPort);
        const auto done = [this, socket, target](bool ok, const QString &why) {
            socket->disconnect(this);
            socket->deleteLater();
            m_editor->setCheck(TunnelEditor::Here, ok ? TunnelCard::Level::Ok : TunnelCard::Level::Problem,
                               ok ? QStringLiteral("%1 answers").arg(target)
                                  : QStringLiteral("%1 does not answer: %2").arg(target, why));
        };
        connect(socket, &QTcpSocket::connected, this, [done] { done(true, {}); });
        connect(socket, &QTcpSocket::errorOccurred, this, [done, socket] { done(false, socket->errorString()); });
        connect(timeout, &QTimer::timeout, this, [done] { done(false, QStringLiteral("no answer in 5 s")); });
        timeout->start(5000);
        socket->connectToHost(forward.targetHost, forward.targetPort);
    } else if (forward.type == PortForward::Type::Local || forward.type == PortForward::Type::Socket) {
        if (running) {
            m_editor->setCheck(TunnelEditor::Target, TunnelCard::Level::Working, QStringLiteral("Asking the server…"));
            runner->probe(shown);
        } else {
            m_editor->setCheck(TunnelEditor::Target, TunnelCard::Level::None,
                               QStringLiteral("Start the tunnel to check what the server reaches"));
        }
    }
}
