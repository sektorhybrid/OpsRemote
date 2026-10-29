// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "rdpoptions.h" // RdpChoices

#include <QProcess>
#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class AvailabilityNote;
namespace YAML { class Node; }

// XDMCP options. The Preferences window keeps the defaults (settings.yaml
// "xdmcp:"); a session may override them; empty fields mean the default.
struct XdmcpOptions
{
    QString mode; // query (the host's own login), indirect (its chooser), broadcast
    QString program; // auto, xephyr, xnest, vcxsrv, or a path to an X server
    QString size; // WIDTHxHEIGHT, or "fullscreen"

    bool isEmpty() const { return *this == XdmcpOptions(); }
    XdmcpOptions overriddenBy(const XdmcpOptions &session) const;
    bool operator==(const XdmcpOptions &) const = default;

    static XdmcpOptions builtIn();
};

RdpChoices xdmcpModeChoices();
RdpChoices xdmcpProgramChoices();
RdpChoices xdmcpSizeChoices();

XdmcpOptions xdmcpOptionsFromYaml(const YAML::Node &node);
void xdmcpOptionsToYaml(const XdmcpOptions &options, YAML::Node &node);
XdmcpOptions loadXdmcpDefaults();
void saveXdmcpDefaults(const XdmcpOptions &options);

// The X server program to show an XDMCP login with: its path and kind
// (xephyr, xnest, vcxsrv, custom), or why there is none.
struct XServerProgram
{
    QString path;
    QString kind;
    QString problem;
};
XServerProgram findXServer(const QString &choice);
// The arguments that start it for host (port: the XDMCP port).
QStringList xServerArguments(const XServerProgram &program, const XdmcpOptions &options, int display,
                             const QString &host, quint16 port, const QString &title);

// Edits XdmcpOptions for a session.
class XdmcpOptionsEditor final : public QWidget
{
    Q_OBJECT

public:
    XdmcpOptionsEditor(const XdmcpOptions &value, const XdmcpOptions &defaults, QWidget *parent = nullptr);

    XdmcpOptions value() const;

private:
    void updateCustom();

    QComboBox *m_mode = nullptr;
    QComboBox *m_program = nullptr;
    QLineEdit *m_path = nullptr;
    QComboBox *m_size = nullptr;
    AvailabilityNote *m_note = nullptr;
    XdmcpOptions m_defaults;
};

// An XDMCP session's tab: the local X server showing the host's login in a
// window of its own, its state and messages, and Restart / Stop.
class XdmcpPage final : public QWidget
{
    Q_OBJECT

public:
    XdmcpPage(const QString &name, const QString &host, quint16 port, const XdmcpOptions &options,
              QWidget *parent = nullptr);
    ~XdmcpPage() override;

    void start();
    void stop();

private:
    void setState(const QString &text);

    QString m_name;
    QString m_host;
    quint16 m_port;
    XdmcpOptions m_options;
    QProcess *m_process = nullptr;
    int m_display = 0;
    QLabel *m_state = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_restart = nullptr;
    QPushButton *m_stop = nullptr;
};
