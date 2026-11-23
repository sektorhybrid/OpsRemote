// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QObject>

class QScrollArea;
class QTabWidget;
class QWidget;

// Every window opens within the free area of its screen: one that would be
// taller or wider than it is made smaller, and one partly off the screen is
// moved onto it (once, when it first opens; the user can resize it after).
// Content that can't shrink that far belongs in a scroll area
// (scrollablePage); OPSREMOTE_TEST_WINDOW_FIT=1 logs the windows whose
// smallest size is still bigger than the screen, to find them.
namespace WindowFit {

// Watches the whole application; call once, after QApplication exists.
void install();

// window at the size its content asks for, within the screen's free area
// (QWidget::adjustSize stops at two thirds of the screen), kept on it.
void adjustSize(QWidget *window);

// After content grew in an open window (a note that appeared): window as
// large as its content asks, never smaller than now, within the screen.
void grow(QWidget *window);

// page in a frameless scroll area that resizes it, for a tab or page that
// can get taller than a small screen: tab widgets hold the scroll area
// (indexOfPage finds it). It asks for the page's full size, so it only
// scrolls when its window had to be made smaller than that; inside another
// scroll area (a Preferences page) it never scrolls itself.
QScrollArea *scrollablePage(QWidget *page);

// The same inside a widget that must stay what it is (a tab other code
// finds by its pointer): puts the scroll area in widget and returns the
// widget to build the content on (its layout; children are moved there when
// added to it).
QWidget *scrollingContent(QWidget *widget);

// The index of the tab holding page, itself or in a scroll area; -1 if none.
int indexOfPage(const QTabWidget *tabs, const QWidget *page);

} // namespace WindowFit
