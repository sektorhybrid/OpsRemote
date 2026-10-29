// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "listenaddress.h"

#include <QComboBox>
#include <QNetworkInterface>

namespace ListenAddress {

bool resolve(const QString &choice, QHostAddress *address, QString *error)
{
    if (choice.isEmpty() || choice == All) {
        *address = QHostAddress::Any;
        return true;
    }
    if (choice == Local) {
        *address = QHostAddress::LocalHost;
        return true;
    }
    const QHostAddress wanted(choice);
    for (const auto &own : QNetworkInterface::allAddresses()) {
        if (!wanted.isNull() && own.isEqual(wanted)) {
            *address = wanted;
            return true;
        }
    }
    *error = QStringLiteral("%1 is not an address of this computer any more; choose where to listen "
                            "again.").arg(choice);
    return false;
}

QString describe(const QString &choice)
{
    if (choice.isEmpty() || choice == All)
        return QStringLiteral("all network interfaces");
    if (choice == Local)
        return QStringLiteral("this computer only");
    return choice;
}

QComboBox *makeBox(const QString &current, QWidget *parent)
{
    auto *box = new QComboBox(parent);
    box->addItem(QStringLiteral("All network interfaces"), All);
    box->addItem(QStringLiteral("This computer only (127.0.0.1)"), Local);
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp)
            || (interface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const auto &entry : interface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            box->addItem(QStringLiteral("%1 (%2)").arg(address.toString(), interface.humanReadableName()),
                         address.toString());
        }
    }
    int index = box->findData(current.isEmpty() ? All : current);
    if (index < 0) {
        box->addItem(QStringLiteral("%1 (not available now)").arg(current), current);
        index = box->count() - 1;
    }
    box->setCurrentIndex(index);
    box->setToolTip(QStringLiteral("All network interfaces: devices on every network this computer "
                                   "is on can connect. This computer only: nothing else can. An "
                                   "address: only that network."));
    return box;
}

}
