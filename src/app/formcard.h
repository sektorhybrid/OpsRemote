// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>

class QAbstractItemView;
class QBoxLayout;
class QFormLayout;
class QWidget;

// Related settings of a session window grouped in a light card: a thin
// rounded frame with a small title, adding little width or height.
// Returns the card's form, to add the rows to.
QFormLayout *addFormCard(QBoxLayout *page, const QString &title, QWidget *parent);

// The same card around a whole editor (its own layout inside); returns the card.
QWidget *addWidgetCard(QBoxLayout *page, const QString &title, QWidget *content);

// A list or table shows this many rows, empty ones too, and scrolls for more,
// instead of growing with the window (a steady layout).
void showRows(QAbstractItemView *view, int rows = 5);

// Gives the labels of all forms under root one width (with a minimum shared
// by all session windows), so fields line up from card to card and tab to tab.
void alignFormLabels(QWidget *root);

