// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "newentry.h"
#include "portforward.h"
#include "sshsession.h"
#include "wakeonlan.h"

#include <QElapsedTimer>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QWidget>

#include <functional>

class JumpChain;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTimer;
class QToolButton;
class QTreeWidget;
class TunnelEditor;

// A saved tunnel: port forwards through one SSH server, kept in
// settings/tunnels.yaml.
struct SavedTunnel
{
    QString id;
    QString name;
    // "session:<id>" (a saved SSH session, with its own jump hosts) or
    // "user@host[:port]".
    QString server;
    // A user@host server's jump hosts, nearest first ("user@host[:port]"
    // or "session:<id>"); a session server uses its own.
    QStringList via;
    QList<PortForward> forwards;
    bool autoStart = false;
    bool reconnect = true;
    // A session server: use the connection of an open tab of that session
    // when there is one (no second log-on).
    bool reuseTab = true;
    // Before connecting: wake the server, then wait up to waitSeconds for
    // it to answer (0: no waiting).
    WakeOnLan wake;
    int waitSeconds = 0;
};

QList<SavedTunnel> loadTunnels();
void saveTunnels(const QList<SavedTunnel> &tunnels);

// What the window shows of a tunnel's SSH server.
struct TunnelServerInfo
{
    bool valid = false;
    QString user;
    QString host;
    quint16 port = 22;
    QString label; // "Monitoring (ops@monitor01)"
    QStringList jumps; // user@host[:port] of each hop, nearest first
    bool fromSession = false; // the jumps are the session's own
};

// The ssh command line that makes the same tunnel (the HTTP proxy, which
// ssh lacks, is noted in a comment).
QString tunnelCommand(const SavedTunnel &tunnel, const TunnelServerInfo &server);
// Forwards from ~/.ssh/config (LocalForward, RemoteForward, DynamicForward),
// one tunnel per Host; problem explains what could not be read.
QList<SavedTunnel> tunnelsFromSshConfig(const QString &path, QString *problem);

// Keeps one tunnel open while it is wanted, reconnecting after the network
// drops, over its own SSH connection or an open tab's.
class TunnelRunner final : public QObject
{
    Q_OBJECT

public:
    enum class State { Stopped, Waking, Connecting, Running, Retrying, Failed };
    struct ForwardState
    {
        bool listening = false;
        QString message;
        int connections = 0;
        qint64 received = 0;
        qint64 sent = 0;
    };

    explicit TunnelRunner(QWidget *dialogParent);
    ~TunnelRunner() override;

    // server carries its jump hosts and password; this takes the password.
    // tab: an open tab's connection to use instead of a new one.
    void start(SshConnectionSettings server, const SavedTunnel &tunnel, SshSession *tab);
    void stop();
    // Whether the server reaches the forward's destination (Local, Socket);
    // answered by probeFinished().
    void probe(int forward);

    State state() const { return m_state; }
    QString message() const { return m_message; }
    QList<ForwardState> forwards() const { return m_forwardStates; }
    // Seconds since the tunnel last started running; -1 when not running.
    qint64 uptime() const;
    int secondsToRetry() const;
    bool usesTab() const { return m_tab != nullptr; }
    qint64 totalReceived() const;
    qint64 totalSent() const;
    int totalConnections() const;

signals:
    void changed();
    void probeFinished(int forward, bool reachable, const QString &message, int milliseconds);

private:
    void connectNow();
    void connectServer(quint16 viaLocalPort);
    void attach(SshSession *ssh, int firstIndex);
    void closeConnections();
    void setState(State state, const QString &message);
    void ended(const QString &reason, bool error);
    void waitForServer(int secondsLeft);

    QPointer<QWidget> m_dialogParent;
    SshConnectionSettings m_server;
    SavedTunnel m_tunnel;
    QPointer<JumpChain> m_chain;
    QPointer<SshSession> m_ssh; // our own connection
    QPointer<SshSession> m_tab; // or an open tab's
    int m_firstIndex = 0;
    QList<ForwardState> m_forwardStates;
    QTimer *m_retry = nullptr;
    QElapsedTimer m_running;
    bool m_wanted = false;
    bool m_wasRunning = false;
    State m_state = State::Stopped;
    QString m_message;
    int m_nextProbe = 1;
    QHash<int, int> m_probes; // probe id -> forward
};

// The SSH Tunnels window: the tunnels with their live state, and the
// selected one as a diagram to edit (this computer → jump hosts → SSH
// server → destination), with its ssh command and checks.
class TunnelsWindow final : public QWidget
{
    Q_OBJECT

public:
    // Fills in an SSH server's connection (with its jump hosts and password)
    // for a tunnel; false when it cannot be used (the reason has been shown).
    using ServerResolver = std::function<bool(const SavedTunnel &tunnel, SshConnectionSettings *)>;
    // Saved SSH sessions: (id, label).
    using SessionChoices = std::function<QList<QPair<QString, QString>>()>;
    // What to show of a tunnel's server.
    using ServerDescriber = std::function<TunnelServerInfo(const QString &server, const QStringList &via)>;
    // The connection of an open, connected tab of a session; nullptr if none.
    using TabFinder = std::function<SshSession *(const QString &sessionId)>;
    // Opens the port scanner for a host.
    using PortScanner = std::function<void(const QString &host)>;

    // makeSession: "New…" in the list of servers (see NewEntry).
    TunnelsWindow(ServerResolver resolver, SessionChoices sessions, NewEntry::SessionMaker makeSession,
                  ServerDescriber describer, TabFinder tabs, PortScanner scanner, QWidget *parent);

    // Starts the tunnels marked "Start with OpsRemote".
    void startAutomatic();
    int runningCount() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    // The editor gets the height it needs and the list the rest; the list
    // keeps a few rows (the editor then scrolls).
    // mayGrow: a taller window when the editor is short of room.
    void fitEditor(bool mayGrow = true);
    int listHeightFor(int rows) const;
    void addRunner();
    void refresh();
    void refreshEditor();
    int currentIndex() const;
    void select(int index);
    void newTunnel(PortForward::Type type);
    void importSshConfig();
    void startTunnel(int index);
    void stopTunnel(int index);
    void removeTunnel(int index);
    void duplicateTunnel(int index);
    void showMenu(const QPoint &position);
    void runChecks(int index);
    void save();

    ServerResolver m_resolver;
    SessionChoices m_sessions;
    NewEntry::SessionMaker m_makeSession;
    ServerDescriber m_describer;
    TabFinder m_tabs;
    PortScanner m_scanner;
    QList<SavedTunnel> m_tunnels;
    QList<TunnelRunner *> m_runners; // same order as m_tunnels
    QTreeWidget *m_list = nullptr;
    QLineEdit *m_filter = nullptr;
    TunnelEditor *m_editor = nullptr;
    QSplitter *m_splitter = nullptr;
    QWidget *m_bottom = nullptr;
    bool m_fitPending = false;
    int m_fittedFor = 0; // list + editor height last fitted for
    bool m_sized = false;
    QLabel *m_empty = nullptr;
    QTimer *m_ticker = nullptr;
};
