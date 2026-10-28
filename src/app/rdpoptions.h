// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
namespace YAML { class Node; }

// Remote desktop options. The Preferences window keeps the defaults
// (settings.yaml "rdp:"); a session may override any of them, and empty
// fields (or -1) mean "use the default".
struct RdpOptions
{
    // "follow": the desktop follows the tab size (Display Control).
    // "connect": the tab size at connection time, then scaled.
    // "WIDTHxHEIGHT": a fixed size, scaled to fit the tab.
    QString resolution;
    int colorDepth = 0; // 32, 24, 16, or 15
    QString quality; // lan, broadband, low
    QString security; // auto, nla, tls, rdp
    // On/off options: 1 on, 0 off, -1 the default.
    int adminSession = -1; // console (/admin) session
    int clipboardText = -1; // copy and paste text both ways
    int clipboardFiles = -1; // copy and paste (and drop) files both ways
    int graphicsPipeline = -1; // RDP 8 graphics (RemoteFX, H.264 if available)
    int highDpi = -1; // physical screen resolution with Windows display scaling
    int allMonitors = -1; // full screen spans every monitor
    // RD Gateway "host[:port]"; empty: the default, "none": connect directly.
    QString gateway;
    // The gateway's user ("DOMAIN\\user"); empty: the session's own credentials.
    QString gatewayUser;
    // "auto" (this computer's layout) or a Windows layout id ("00000408").
    QString keyboardLayout;
    int autoReconnect = -1; // reconnect after a network drop
    int askCredentials = -1; // off: log on at the remote screen instead
    // A program started instead of the desktop, and its folder (Windows
    // Server, and xrdp; Windows client editions ignore it).
    QString startProgram;
    QString startFolder;

    bool isEmpty() const;
    RdpOptions overriddenBy(const RdpOptions &session) const;
    bool operator==(const RdpOptions &) const = default;

    // The built-in defaults, every field set.
    static RdpOptions builtIn();
};

// (value, label) choices, shared by the session editor and Preferences.
using RdpChoices = QList<QPair<QString, QString>>;
RdpChoices rdpResolutionChoices();
RdpChoices rdpColorDepthChoices();
RdpChoices rdpQualityChoices();
RdpChoices rdpSecurityChoices();
RdpChoices rdpKeyboardChoices();
// The Windows keyboard layout id for "auto" or a stored id.
quint32 rdpKeyboardLayoutId(const QString &value);

RdpOptions rdpOptionsFromYaml(const YAML::Node &node);
// Writes only the fields that are set.
void rdpOptionsToYaml(const RdpOptions &options, YAML::Node &node);
RdpOptions loadRdpDefaults();
void saveRdpDefaults(const RdpOptions &options);

// Edits RdpOptions. For a session (withDefault), every choice starts with
// "Default (…)", which leaves the field empty; on/off options are checkboxes
// that start at the default and are stored only when they differ from it.
class RdpOptionsEditor final : public QWidget
{
    Q_OBJECT

public:
    RdpOptionsEditor(const RdpOptions &value, bool withDefault, const RdpOptions &defaults,
                     QWidget *parent = nullptr);

    RdpOptions value() const;

signals:
    void changed();

private:
    QComboBox *m_resolution = nullptr;
    QComboBox *m_colorDepth = nullptr;
    QComboBox *m_quality = nullptr;
    QComboBox *m_security = nullptr;
    RdpOptions m_defaults;
    bool m_withDefault = false;
    QCheckBox *m_admin = nullptr;
    QCheckBox *m_clipboardText = nullptr;
    QCheckBox *m_clipboardFiles = nullptr;
    QCheckBox *m_graphics = nullptr;
    QCheckBox *m_highDpi = nullptr;
    QCheckBox *m_allMonitors = nullptr;
    QLineEdit *m_gateway = nullptr;
    QLineEdit *m_gatewayUser = nullptr;
    QComboBox *m_keyboard = nullptr;
    QCheckBox *m_autoReconnect = nullptr;
    QCheckBox *m_askCredentials = nullptr;
    QLineEdit *m_startProgram = nullptr;
    QLineEdit *m_startFolder = nullptr;
};
