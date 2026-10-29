// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QVariant>

#include <functional>

class QComboBox;
class QWidget;

// "New…" at the end of a list of saved SSH sessions (jump hosts, tunnels,
// SSH Keys): choosing it opens the session window over the tool window and
// then chooses what was made.
namespace NewEntry {

// Makes a new SSH session in the session window, shown over `over`; returns
// its id, or an empty string when cancelled.
using SessionMaker = std::function<QString(QWidget *over)>;
// The saved SSH sessions: (id, label).
using SessionChoices = std::function<QList<QPair<QString, QString>>()>;

// Adds "New…" as the last entry of combo. After a session is made it is
// inserted before "New…" (as text(label) with data(id)), chosen, and
// combo's activated() is emitted for it, as if the user had picked it; a
// cancel goes back to the previous choice. The combo's own activated()
// handlers must skip the entry (isNew).
void add(QComboBox *combo, SessionMaker make, SessionChoices choices,
         std::function<QString(const QString &label)> text,
         std::function<QVariant(const QString &id)> data);
bool isNew(const QComboBox *combo, int index);

}
