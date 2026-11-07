// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>
#include <QVariant>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QWidget;

// A session's (or folder's) setting that comes from the Preferences until
// the session changes it, shown the same in every window: the control
// alone while inherited, showing the inherited value as it is (no
// "Default" label), and once the session has its own value a ↺ button at its
// right back to it ("Revert to default"). Each returns the row's widget to
// put in the form instead of the control.

// A drop-down's first entry, standing for the inherited value: text is that
// value as shown, and the value itself is not listed again after it.
void addInheritedItem(QComboBox *box, const QString &text, const QVariant &data = QVariant());
void insertInheritedItem(QComboBox *box, const QString &text, const QVariant &data = QVariant());

// A switch: inherited while it shows inheritedOn.
QWidget *inheritable(QCheckBox *box, bool inheritedOn);
// A drop-down whose first entry is the inherited one: inherited while it is chosen.
QWidget *inheritable(QComboBox *box, const QString &inheritedText);
// A text field: inherited while empty (its placeholder shows the inherited value).
QWidget *inheritable(QLineEdit *edit, const QString &inheritedText);
// A number field whose lowest value shows the inherited one: inherited while at it.
QWidget *inheritable(QSpinBox *box, const QString &inheritedText);

// The same for a whole session editor, after it is built: every row of its
// forms whose control inherits is wrapped. Recognised: drop-downs whose
// first entry came from addInheritedItem(), number fields with the property
// "inherits" (their lowest value is the inherited one), switches with the
// property "inheritedOn" (bool), and text fields with "inheritedText".
void applyInheritance(QWidget *editor);
