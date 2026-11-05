// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class AvailabilityNote;
struct SshConnectionSettings;
namespace YAML { class Node; }

// Session types that run a program here or in a container: Local (also as
// administrator), WSL, Docker and Kubernetes. Sessions saved as "Local
// (Administrator)" are read as Local with administrator on.
QStringList localSessionProtocols();
bool isLocalSessionProtocol(const QString &protocol);
// Whether the protocol needs a host name (none of these do).
bool protocolNeedsHost(const QString &protocol);

struct LocalSessionOptions
{
    // Local: which shell (see localShellChoices(); empty: the system's
    // default), "custom" for program (with its arguments); the start folder
    // (also WSL).
    QString localShell;
    QString program;
    // Local: run as administrator (root on Linux), and how; empty: the
    // Preferences' (Terminal → Administrator). Windows: "sudo" (Windows sudo
    // when it is on) or "helper" (OpsRemote's elevated helper); Linux:
    // "sudo" or "pkexec".
    QString elevation;
    bool administrator = false;
    QString folder;
    // WSL: the distribution (empty: the default) and user.
    QString distribution;
    QString user; // also Docker and Kubernetes
    // Docker: docker or podman. Where it runs: "" this computer, "@context"
    // (dockerContext), "@tcp" (a remote engine at dockerHost, with the TLS
    // files below), or an SSH session's id (also Kubernetes).
    QString engine;
    QString via;
    QString dockerContext;
    QString dockerHost;
    // Kubernetes: how kubectl logs on: "" the default kubeconfig
    // (~/.kube/config or $KUBECONFIG), "saved" (kubeCredential, from Tools →
    // Kubernetes Credentials), "file" (kubeconfig), "token" (server and the
    // session's password as bearer token), or "certificate" (server and
    // clientCertificate, clientKey).
    QString kubeAuth;
    QString kubeCredential;
    QString kubeconfig;
    QString server;
    // TLS: the CA, and the client certificate and key (Kubernetes; Docker
    // over TCP). On an SSH server, paths there.
    QString caFile;
    bool insecure = false; // Kubernetes: do not check the server's certificate
    QString clientCertificate;
    QString clientKey;
    QString container; // also Kubernetes' container in the pod
    QString shell; // empty: bash if there is one, else sh
    // Kubernetes.
    QString context;
    QString kubeNamespace;
    QString pod;

    bool isEmpty() const { return *this == LocalSessionOptions(); }
    bool operator==(const LocalSessionOptions &) const = default;
};

LocalSessionOptions localSessionOptionsFromYaml(const YAML::Node &node);
void localSessionOptionsToYaml(const LocalSessionOptions &options, YAML::Node &node);

// Preferences → Connections → Docker, Kubernetes, WSL (settings.yaml
// "containers:").
struct ContainerPreferences
{
    struct Type
    {
        bool enabled = true; // the session type
        int listTimeout = 15; // seconds a List (containers, pods…) may take
        QString program; // docker or kubectl; empty: found on the PATH
        QString shell; // empty: bash if there is one, else sh

        bool operator==(const Type &) const = default;
    };
    Type docker;
    Type kubernetes;
    QString podman;
    struct Wsl
    {
        bool enabled = true;
        int listTimeout = 15;
        QString distribution; // empty: WSL's default

        bool operator==(const Wsl &) const = default;
    } wsl;

    bool operator==(const ContainerPreferences &) const = default;
};

ContainerPreferences loadContainerPreferences();
void saveContainerPreferences(const ContainerPreferences &preferences);

// The program for docker, podman or kubectl here (from the Preferences or
// the PATH); empty when it is not installed.
QString containerProgram(const QString &name);
// For an empty program field: where the program is on the PATH, or where
// it usually is installed ("e.g. …").
QString programPathHint(const QString &name);
// Why a session type cannot be used on this computer; empty when it can.
QString localSessionProblem(const QString &protocol, const QString &engine = QString());

// The command line run in the terminal: program and arguments here, or one
// shell command on the SSH server (via).
struct LocalCommand
{
    QString program;
    QStringList arguments;
    QString remoteCommand; // Docker and Kubernetes over SSH
    // A private kubeconfig written for the token or certificate log-on; the
    // caller removes it once kubectl has read it.
    QString temporaryFile;
};
// A kubeconfig for a server with a token (kubeAuth "token") or a client
// certificate ("certificate"), its CA or insecure, and namespace.
QString generatedKubeconfig(const LocalSessionOptions &options, const QString &token);
// The session types' names in the protocol list, and the earlier ones.
const QString &dockerProtocol();
const QString &kubernetesProtocol();
QString currentProtocolName(const QString &saved);
// Docker or Kubernetes run on an SSH server (not here, a context, or TCP).
bool runsOverSsh(const LocalSessionOptions &options);
LocalCommand dockerCommand(const LocalSessionOptions &options, const ContainerPreferences &preferences);
// secret: the session's password for the token log-on, or the saved
// credential's kubeconfig.
LocalCommand kubernetesCommand(const LocalSessionOptions &options, const ContainerPreferences &preferences,
                               const QString &secret);
LocalCommand wslCommand(const LocalSessionOptions &options);
// The local shells on this computer: (id, label); "" is the system's default
// (Windows PowerShell; on Linux the login shell), "custom" another program.
QList<QPair<QString, QString>> localShellChoices();
// A local shell's program, arguments and label; an empty program on Linux
// means the login shell ($SHELL).
struct LocalShellCommand
{
    QString program;
    QStringList arguments;
    QString label;
};
LocalShellCommand localShellCommand(const QString &choice, const QString &customProgram = QString());
// The ways to run as administrator here: (id, label).
QList<QPair<QString, QString>> elevationChoices();

// The session window's Connection tab for these session types.
class LocalSessionEditor final : public QWidget
{
    Q_OBJECT

public:
    // SSH sessions to run containers on: (id, label).
    using SessionChoices = std::function<QList<QPair<QString, QString>>()>;
    // An SSH session's connection, for listing its containers; empty when
    // it cannot be used directly (jump hosts).
    using Resolver = std::function<std::optional<SshConnectionSettings>(const QString &id)>;

    LocalSessionEditor(const LocalSessionOptions &value, SessionChoices sessions, Resolver resolver,
                       QWidget *parent = nullptr);
    void setProtocol(const QString &protocol);
    // For listing: the token (the password field, or the stored password)
    // or a saved credential's kubeconfig (by name), and the saved names.
    void setSecretSource(std::function<QString(const LocalSessionOptions &)> source) { m_secret = std::move(source); }
    void setCredentialNames(std::function<QStringList()> names);

signals:
    // Manage… beside the saved Kubernetes credentials.
    void manageCredentialsRequested();

public:
    LocalSessionOptions value() const;

private:
    // Fills a list from a command's output lines, here or on the SSH server.
    // kubeconfig: one to write for the command (a private file here, removed
    // afterwards; on an SSH server inside the command).
    // noun: what is listed ("containers"), for "No containers found". asked:
    // the ↻ button was pressed, so an error also shows in a message.
    void fillFrom(QComboBox *target, const QString &program, const QStringList &arguments,
                  const QString &kubeconfig, const QString &noun, bool asked);
    // Lists a box's choices (in the background unless asked).
    void list(QComboBox *target, bool asked);
    // Soon, once: a box's list, and the lists that depend on a changed box.
    void scheduleList(QComboBox *target);
    void scheduleLists();
    void listDependents(QComboBox *changed);
    QHash<QComboBox *, class QTimer *> m_listTimers;
    // The line under each list for its state.
    QHash<QComboBox *, class StatusLine *> m_statusLines;
    int listTimeout() const;
    bool overSsh() const;
    void refreshContainers(bool asked);
    void refreshKubernetes(QComboBox *target, const QStringList &arguments, const QString &noun, bool asked);
    void updateRows();
    QString via() const;
    std::function<QString(const LocalSessionOptions &)> m_secret;
    std::function<QStringList()> m_credentialNames;
    QComboBox *m_kubeCredential = nullptr;
    QWidget *m_logOnRows = nullptr;
    QComboBox *m_kubeAuth = nullptr;
    QLineEdit *m_kubeconfig = nullptr;
    QLineEdit *m_server = nullptr;
    QLineEdit *m_caFile = nullptr;
    class QCheckBox *m_insecure = nullptr;
    QLineEdit *m_clientCertificate = nullptr;
    QLineEdit *m_clientKey = nullptr;
    QComboBox *m_dockerContext = nullptr;
    QLineEdit *m_dockerHost = nullptr;
    QLabel *m_tokenNote = nullptr;

    QString m_protocol;
    Resolver m_resolver;
    QWidget *m_localRows = nullptr;
    QWidget *m_wslRows = nullptr;
    QWidget *m_dockerRows = nullptr;
    QWidget *m_kubernetesRows = nullptr;
    QWidget *m_containerRows = nullptr;
    QComboBox *m_localShell = nullptr;
    QComboBox *m_elevation = nullptr;
    class QCheckBox *m_administrator = nullptr;
    QLineEdit *m_program = nullptr;
    QLineEdit *m_folder = nullptr;
    QComboBox *m_distribution = nullptr;
    QLineEdit *m_user = nullptr;
    QComboBox *m_engine = nullptr;
    QComboBox *m_via = nullptr;
    QComboBox *m_container = nullptr;
    QLineEdit *m_shell = nullptr;
    QComboBox *m_context = nullptr;
    QComboBox *m_namespace = nullptr;
    QComboBox *m_pod = nullptr;
    QLabel *m_listStatus = nullptr;
    AvailabilityNote *m_note = nullptr;
};
