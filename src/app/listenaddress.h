// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QHostAddress>
#include <QString>

class QComboBox;
class QWidget;

// Where a server of OpsRemote listens: "all" (every network interface),
// "localhost" (this computer only), or one of this computer's addresses.
namespace ListenAddress {

inline const QString All = QStringLiteral("all");
inline const QString Local = QStringLiteral("localhost");

// The address to bind; false (with error) when a chosen address is no
// longer this computer's.
bool resolve(const QString &choice, QHostAddress *address, QString *error);
// A short description for status lines: "all interfaces", "this computer
// only", or the address.
QString describe(const QString &choice);
// A box with the choices, the current one selected (kept even when that
// address is gone, so it is not silently changed).
QComboBox *makeBox(const QString &current, QWidget *parent);

}
