// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

class QComboBox;
namespace YAML { class Node; }

// A serial line's settings (sessions.yaml "serial:"), for consoles of
// switches, routers, and embedded boards.
struct SerialOptions
{
    int baudRate = 9600;
    int dataBits = 8;
    QString parity = QStringLiteral("none"); // none, even, odd, mark, space
    QString stopBits = QStringLiteral("1"); // 1, 1.5, 2
    QString flowControl = QStringLiteral("none"); // none, hardware, software

    bool isEmpty() const { return *this == SerialOptions(); }
    bool operator==(const SerialOptions &) const = default;
};

SerialOptions serialOptionsFromYaml(const YAML::Node &node);
void serialOptionsToYaml(const SerialOptions &options, YAML::Node &node);
// The serial ports of this computer: (name for the session, description).
QList<QPair<QString, QString>> availableSerialPorts();

// Edits a session's SerialOptions.
class SerialOptionsEditor final : public QWidget
{
    Q_OBJECT

public:
    explicit SerialOptionsEditor(const SerialOptions &value, QWidget *parent = nullptr);

    SerialOptions value() const;

private:
    QComboBox *m_baudRate = nullptr;
    QComboBox *m_dataBits = nullptr;
    QComboBox *m_parity = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_flowControl = nullptr;
};
