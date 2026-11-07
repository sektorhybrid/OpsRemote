// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "preferences.h"

#include <QObject>
#include <QString>

// Selecting, copying and pasting with the mouse, the same in terminals and
// in OpsRemote's text fields, on Linux and Windows (Preferences → Workspace
// → Mouse and clipboard). Terminals call it; text fields follow it through
// installTextFieldBehaviour().
namespace Clipboard {

const MouseClipboardPreferences &preferences();
// The settings in use (MainWindow, after loading or a change; not saved here).
void setPreferences(const MouseClipboardPreferences &value);

// Emits changed() when the settings change (terminals take the new word
// characters and link settings).
class Notifier final : public QObject
{
    Q_OBJECT

signals:
    void changed();
};
Notifier *notifier();

// The characters double-click keeps in a word besides letters and digits.
QString wordCharacters();
bool isWordCharacter(QChar c);

// A selection made in OpsRemote (terminal or text field) was finished:
// remembered as the newest, and on the clipboard with copy on select.
void selectionFinished(const QString &text);

// What a paste pastes: from a middle-click, or from anything else (right-
// click, Ctrl+Shift+V, a menu), by the paste rule.
QString textToPaste(bool middleClick);

// OpsRemote's text fields (line edits, text editors) follow the middle-click,
// right-click, double-click and copy-on-select settings too.
void installTextFieldBehaviour();

} // namespace Clipboard
