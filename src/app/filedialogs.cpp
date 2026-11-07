// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "filedialogs.h"

#include <QAbstractFileIconProvider>
#include <QFileDialog>
#include <QFileInfo>

namespace FileDialogs {

namespace {

// The Type column by the name only, never by reading the file.
class QuickTypes final : public QAbstractFileIconProvider
{
public:
    QString type(const QFileInfo &info) const override
    {
        if (info.isDir())
            return QStringLiteral("Folder");
        if (!info.suffix().isEmpty())
            return QStringLiteral("%1 file").arg(info.suffix().toUpper());
        return info.isExecutable() ? QStringLiteral("Program") : QStringLiteral("File");
    }
};

QStringList run(QWidget *parent, const QString &caption, const QString &folder, const QString &filter,
                QFileDialog::FileMode mode, QFileDialog::AcceptMode accept)
{
    QFileDialog dialog(parent, caption, folder, filter);
    dialog.setFileMode(mode);
    dialog.setAcceptMode(accept);
    if (mode == QFileDialog::Directory)
        dialog.setOption(QFileDialog::ShowDirsOnly);
    useQuickTypes(&dialog);
    // A file name given with the folder is selected, as the static
    // functions do.
    if (!folder.isEmpty() && !QFileInfo(folder).isDir())
        dialog.selectFile(folder);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    return dialog.selectedFiles();
}

} // namespace

void useQuickTypes(QFileDialog *dialog)
{
    static QuickTypes types;
    dialog->setIconProvider(&types);
}

QString openFileName(QWidget *parent, const QString &caption, const QString &folder, const QString &filter)
{
    return run(parent, caption, folder, filter, QFileDialog::ExistingFile, QFileDialog::AcceptOpen).value(0);
}

QStringList openFileNames(QWidget *parent, const QString &caption, const QString &folder, const QString &filter)
{
    return run(parent, caption, folder, filter, QFileDialog::ExistingFiles, QFileDialog::AcceptOpen);
}

QString saveFileName(QWidget *parent, const QString &caption, const QString &folder, const QString &filter)
{
    return run(parent, caption, folder, filter, QFileDialog::AnyFile, QFileDialog::AcceptSave).value(0);
}

QString existingDirectory(QWidget *parent, const QString &caption, const QString &folder)
{
    return run(parent, caption, folder, QString(), QFileDialog::Directory, QFileDialog::AcceptOpen).value(0);
}

} // namespace FileDialogs
