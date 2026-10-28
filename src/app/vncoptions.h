// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "rdpoptions.h" // RdpChoices

#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
namespace YAML { class Node; }

// VNC options. The Preferences window keeps the defaults (settings.yaml
// "vnc:"); a session may override any of them, and empty fields (or -1)
// mean "use the default". Listening and repeaters belong to a session only.
struct VncOptions
{
    // "follow": the remote desktop follows the tab size when the server
    // allows it; "fit": the server's size, scaled down to fit the tab.
    QString resolution;
    int colorDepth = 0; // 32, 16, or 8
    QString quality; // best, good, medium, poor
    QString encryption; // prefer, require
    // On/off options: 1 on, 0 off, -1 the default.
    int remoteCursor = -1; // the server's pointer shape, drawn locally
    int clipboard = -1; // copy and paste text both ways
    int autoReconnect = -1;
    int highDpi = -1; // one remote pixel per physical pixel
    int allMonitors = -1; // full screen spans every monitor
    // Through an SSH server: "session:<saved session id>", or
    // "user@host[:port]"; empty: the default, "none": directly.
    QString sshTunnel;
    // Session only: wait for the server to connect (reverse VNC).
    int listen = -1;
    int listenPort = 0; // 5500 when 0
    // Session only: an UltraVNC repeater "host[:port]" and the server's ID
    // there (without an ID, the session's host and port are asked for).
    QString repeater;
    QString repeaterId;

    bool isEmpty() const;
    VncOptions overriddenBy(const VncOptions &session) const;
    bool operator==(const VncOptions &) const = default;

    static VncOptions builtIn();
};

RdpChoices vncResolutionChoices();
RdpChoices vncColorDepthChoices();
RdpChoices vncQualityChoices();
RdpChoices vncEncryptionChoices();

VncOptions vncOptionsFromYaml(const YAML::Node &node);
void vncOptionsToYaml(const VncOptions &options, YAML::Node &node);
VncOptions loadVncDefaults();
void saveVncDefaults(const VncOptions &options);

// Edits VncOptions for a session: choices start with "Default (…)", on/off
// options are checkboxes stored only when they differ from the default.
// sshSessions lists the saved SSH sessions (id, label) for the tunnel.
class VncOptionsEditor final : public QWidget
{
    Q_OBJECT

public:
    VncOptionsEditor(const VncOptions &value, const VncOptions &defaults,
                     const QList<QPair<QString, QString>> &sshSessions, QWidget *parent = nullptr);

    VncOptions value() const;

signals:
    void changed();

private:
    VncOptions m_defaults;
    QComboBox *m_resolution = nullptr;
    QComboBox *m_colorDepth = nullptr;
    QComboBox *m_quality = nullptr;
    QComboBox *m_encryption = nullptr;
    QCheckBox *m_remoteCursor = nullptr;
    QCheckBox *m_clipboard = nullptr;
    QCheckBox *m_autoReconnect = nullptr;
    QCheckBox *m_highDpi = nullptr;
    QCheckBox *m_allMonitors = nullptr;
    QComboBox *m_sshTunnel = nullptr;
    QCheckBox *m_listen = nullptr;
    QSpinBox *m_listenPort = nullptr;
    QLineEdit *m_repeater = nullptr;
    QLineEdit *m_repeaterId = nullptr;
};
