// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "serialoptions.h"

#include <QComboBox>
#include <QFormLayout>
#include <QIntValidator>
#include <QLineEdit>
#include <QSerialPortInfo>

#include <yaml-cpp/yaml.h>

SerialOptions serialOptionsFromYaml(const YAML::Node &node)
{
    SerialOptions options;
    if (!node || !node.IsMap())
        return options;
    try {
        options.baudRate = qBound(50, node["baud_rate"].as<int>(options.baudRate), 4000000);
        options.dataBits = qBound(5, node["data_bits"].as<int>(options.dataBits), 8);
        const auto choice = [&node](const char *key, QString &target, const QStringList &allowed) {
            const auto value = QString::fromStdString(node[key].as<std::string>(target.toStdString()));
            if (allowed.contains(value))
                target = value;
        };
        choice("parity", options.parity,
               {QStringLiteral("none"), QStringLiteral("even"), QStringLiteral("odd"),
                QStringLiteral("mark"), QStringLiteral("space")});
        choice("stop_bits", options.stopBits,
               {QStringLiteral("1"), QStringLiteral("1.5"), QStringLiteral("2")});
        choice("flow_control", options.flowControl,
               {QStringLiteral("none"), QStringLiteral("hardware"), QStringLiteral("software")});
    } catch (const YAML::Exception &) {
        // Keep what was read; invalid values use the defaults.
    }
    return options;
}

void serialOptionsToYaml(const SerialOptions &options, YAML::Node &node)
{
    node["baud_rate"] = options.baudRate;
    node["data_bits"] = options.dataBits;
    node["parity"] = options.parity.toStdString();
    node["stop_bits"] = options.stopBits.toStdString();
    node["flow_control"] = options.flowControl.toStdString();
}

QList<QPair<QString, QString>> availableSerialPorts()
{
    QList<QPair<QString, QString>> ports;
    for (const auto &info : QSerialPortInfo::availablePorts()) {
#ifdef Q_OS_WIN
        const QString name = info.portName(); // COM3
#else
        const QString name = info.systemLocation(); // /dev/ttyUSB0
#endif
        QString label = info.description();
        if (!info.manufacturer().isEmpty() && !label.contains(info.manufacturer()))
            label = label.isEmpty() ? info.manufacturer() : label + QStringLiteral(" (%1)").arg(info.manufacturer());
        ports.append({name, label});
    }
    return ports;
}

namespace {

QComboBox *choices(const QList<QPair<QString, QString>> &items, const QString &current,
                   QWidget *parent)
{
    auto *box = new QComboBox(parent);
    for (const auto &[value, label] : items)
        box->addItem(label, value);
    box->setCurrentIndex(qMax(0, box->findData(current)));
    return box;
}

}

SerialOptionsEditor::SerialOptionsEditor(const SerialOptions &value, QWidget *parent)
    : QWidget(parent)
{
    m_baudRate = new QComboBox(this);
    m_baudRate->setEditable(true);
    m_baudRate->setValidator(new QIntValidator(50, 4000000, m_baudRate));
    for (const int rate : {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600})
        m_baudRate->addItem(QString::number(rate));
    m_baudRate->setCurrentText(QString::number(value.baudRate));
    m_baudRate->setToolTip(QStringLiteral("Most network equipment uses 9600; boards often 115200."));
    m_dataBits = choices({{QStringLiteral("8"), QStringLiteral("8")}, {QStringLiteral("7"), QStringLiteral("7")},
                          {QStringLiteral("6"), QStringLiteral("6")}, {QStringLiteral("5"), QStringLiteral("5")}},
                         QString::number(value.dataBits), this);
    m_parity = choices({{QStringLiteral("none"), QStringLiteral("None")},
                        {QStringLiteral("even"), QStringLiteral("Even")},
                        {QStringLiteral("odd"), QStringLiteral("Odd")},
                        {QStringLiteral("mark"), QStringLiteral("Mark")},
                        {QStringLiteral("space"), QStringLiteral("Space")}},
                       value.parity, this);
    m_stopBits = choices({{QStringLiteral("1"), QStringLiteral("1")},
                          {QStringLiteral("1.5"), QStringLiteral("1.5")},
                          {QStringLiteral("2"), QStringLiteral("2")}},
                         value.stopBits, this);
    m_flowControl = choices({{QStringLiteral("none"), QStringLiteral("None")},
                             {QStringLiteral("hardware"), QStringLiteral("Hardware (RTS/CTS)")},
                             {QStringLiteral("software"), QStringLiteral("Software (XON/XOFF)")}},
                            value.flowControl, this);
    auto *form = new QFormLayout(this);
    form->addRow(QStringLiteral("Speed (baud):"), m_baudRate);
    form->addRow(QStringLiteral("Data bits:"), m_dataBits);
    form->addRow(QStringLiteral("Parity:"), m_parity);
    form->addRow(QStringLiteral("Stop bits:"), m_stopBits);
    form->addRow(QStringLiteral("Flow control:"), m_flowControl);
}

SerialOptions SerialOptionsEditor::value() const
{
    SerialOptions options;
    bool ok = false;
    const int rate = m_baudRate->currentText().toInt(&ok);
    if (ok && rate >= 50)
        options.baudRate = rate;
    options.dataBits = m_dataBits->currentData().toString().toInt();
    options.parity = m_parity->currentData().toString();
    options.stopBits = m_stopBits->currentData().toString();
    options.flowControl = m_flowControl->currentData().toString();
    return options;
}
