// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QString>
#include <QStringList>

class QFileDialog;
class QWidget;

// Choosing files and folders, as QFileDialog's static functions (same
// arguments), everywhere in OpsRemote. Where Qt draws its own window (the
// Linux package has no desktop one) the Type column comes from the file
// name: Qt's own reads every file, and a folder like /usr/bin (over 10 000
// programs) stopped OpsRemote answering for about ten seconds.
namespace FileDialogs {

QString openFileName(QWidget *parent, const QString &caption = QString(), const QString &folder = QString(),
                     const QString &filter = QString());
QStringList openFileNames(QWidget *parent, const QString &caption = QString(),
                          const QString &folder = QString(), const QString &filter = QString());
QString saveFileName(QWidget *parent, const QString &caption = QString(), const QString &folder = QString(),
                     const QString &filter = QString());
QString existingDirectory(QWidget *parent, const QString &caption = QString(),
                          const QString &folder = QString());

// The same quick types for a dialog made by hand.
void useQuickTypes(QFileDialog *dialog);

} // namespace FileDialogs
