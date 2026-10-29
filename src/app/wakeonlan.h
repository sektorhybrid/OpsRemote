// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>
#include <QWidget>

#include <yaml-cpp/yaml.h>

class QCheckBox;
class QLineEdit;
class QSpinBox;

// Wake-on-LAN for a saved session: a "magic packet" (6 x 0xFF, then the MAC
// address 16 times) sent as a UDP broadcast.
struct WakeOnLan
{
    QString mac; // "aa:bb:cc:dd:ee:ff"; empty: not set up
    QString broadcast; // empty: 255.255.255.255
    quint16 port = 0; // 0: 9
    bool beforeConnecting = false;
    int waitSeconds = 0; // 0: 20

    bool isSet() const { return !mac.isEmpty(); }
    bool isEmpty() const { return *this == WakeOnLan(); }
    bool operator==(const WakeOnLan &) const = default;

    // Sends the packet; false with the reason in *error.
    bool send(QString *error) const;
    int effectiveWait() const { return waitSeconds > 0 ? waitSeconds : 20; }
};

WakeOnLan wakeOnLanFromYaml(const YAML::Node &node);
YAML::Node wakeOnLanToYaml(const WakeOnLan &value);
// The 6 bytes of "aa:bb:cc:dd:ee:ff" (also with '-' or none); empty if invalid.
QByteArray parseMacAddress(const QString &text);

// The session window's "Wake-on-LAN" tab.
class WakeOnLanEditor final : public QWidget
{
    Q_OBJECT

public:
    explicit WakeOnLanEditor(const WakeOnLan &value, QWidget *parent = nullptr);
    WakeOnLan value() const;
    // An explanation of what is wrong, or empty.
    QString problem() const;

private:
    QLineEdit *m_mac = nullptr;
    QLineEdit *m_broadcast = nullptr;
    QSpinBox *m_port = nullptr;
    QCheckBox *m_beforeConnecting = nullptr;
    QSpinBox *m_wait = nullptr;
};
