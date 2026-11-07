// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "filedrop.h"

#include "preferences.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

namespace FileDrop {

namespace {

QString readText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(file.readAll()) : QString();
}

// A process's command line, as typed ("ssh web01").
QString commandLine(int pid)
{
    auto parts = readText(QStringLiteral("/proc/%1/cmdline").arg(pid)).split(QChar(0), Qt::SkipEmptyParts);
    if (!parts.isEmpty())
        parts.first() = QFileInfo(parts.first()).fileName(); // "ssh web01", not its path
    return parts.join(QLatin1Char(' '));
}

} // namespace

bool isNestedProgram(const QString &line)
{
    static const QStringList programs{
        QStringLiteral("ssh"),     QStringLiteral("su"),      QStringLiteral("sudo"),
        QStringLiteral("doas"),    QStringLiteral("docker"),  QStringLiteral("podman"),
        QStringLiteral("kubectl"), QStringLiteral("oc"),      QStringLiteral("mosh"),
        QStringLiteral("telnet"),  QStringLiteral("wsl"),     QStringLiteral("machinectl"),
        QStringLiteral("nsenter"), QStringLiteral("chroot"),  QStringLiteral("lxc"),
        QStringLiteral("incus"),   QStringLiteral("toolbox"), QStringLiteral("distrobox"),
        QStringLiteral("rlogin"),  QStringLiteral("screen"),  QStringLiteral("tmux")};
    return programs.contains(QFileInfo(line.section(QLatin1Char(' '), 0, 0)).fileName());
}

Folder localFolder(int shellPid, int foregroundPid)
{
    Folder result;
    result.where = QStringLiteral("this computer");
#ifdef Q_OS_LINUX
    const int pid = foregroundPid > 0 ? foregroundPid : shellPid;
    if (pid <= 0) {
        result.error = QStringLiteral("The terminal's program has ended.");
        return result;
    }
    result.path = QFileInfo(QStringLiteral("/proc/%1/cwd").arg(pid)).symLinkTarget();
    if (result.path.isEmpty())
        result.error = QStringLiteral("The folder of the program in this terminal cannot be read "
                                      "(it may run as another user, for example as root).");
    if (pid != shellPid) {
        result.program = commandLine(pid);
        result.nested = isNestedProgram(result.program);
    }
#else
    Q_UNUSED(shellPid);
    Q_UNUSED(foregroundPid);
    result.error = QStringLiteral("The folder of a local terminal is known only on Linux.");
#endif
    return result;
}

Folder remoteFolder(const QString &answer, const QString &where, const QString &reported)
{
    Folder result;
    result.where = where;
    const auto lines = answer.split(QLatin1Char('\n'));
    if (lines.value(0).trimmed() != QStringLiteral("tag") || lines.size() < 7) {
        // The server did not take the shell's tag, or has no /proc.
        if (!reported.isEmpty()) {
            result.path = reported;
            result.note = QStringLiteral("This is the folder the shell last reported; the server could not "
                                         "be asked which folder it is in now.");
        } else {
            result.error = QStringLiteral("the server could not be asked which folder the shell is in (it "
                                          "needs /proc, as on Linux, and to accept the LC_* variables "
                                          "OpenSSH servers usually accept).");
        }
        return result;
    }
    const auto name = lines.value(2).trimmed();
    const auto user = lines.value(3).trimmed();
    result.path = lines.value(4).trimmed();
    const bool shell = lines.value(5).trimmed() == QStringLiteral("shell");
    const auto me = lines.value(6).trimmed();
    if (!shell) {
        auto command = lines.mid(7).join(QLatin1Char(' ')).trimmed();
        if (command.isEmpty())
            command = name;
        auto parts = command.split(QLatin1Char(' '));
        parts.first() = QFileInfo(parts.first()).fileName();
        result.program = parts.join(QLatin1Char(' '));
        const bool otherUser = !user.isEmpty() && user != me;
        if (otherUser)
            result.program += QStringLiteral(" (as %1)").arg(user);
        result.nested = otherUser || isNestedProgram(result.program);
    }
    if (result.path.isEmpty())
        result.error = QStringLiteral("the folder of %1 cannot be read on the server (it runs as another "
                                      "user).").arg(result.program.isEmpty() ? name : result.program);
    return result;
}

Choice confirm(QWidget *parent, const QStringList &files, Folder *folder,
               const std::function<QString()> &chooseFolder)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Upload Dropped Files"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QLabel(&dialog);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setMinimumWidth(420);
    layout->addWidget(text);
    auto *buttons = new QDialogButtonBox(&dialog);
    auto *upload = buttons->addButton(QStringLiteral("Upload"), QDialogButtonBox::AcceptRole);
    auto *choose = buttons->addButton(QStringLiteral("Choose Another Folder…"), QDialogButtonBox::ActionRole);
    auto *paths = buttons->addButton(QStringLiteral("Paste the Paths"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    choose->setVisible(bool(chooseFolder));

    const auto what = files.size() == 1
                          ? QStringLiteral("<b>%1</b>").arg(QFileInfo(files.first()).fileName().toHtmlEscaped())
                          : QStringLiteral("<b>%1 items</b>").arg(files.size());
    const auto show = [&] {
        QString message;
        if (folder->path.isEmpty()) {
            message = QStringLiteral("Where to put %1 is not known: %2<br>Choose a folder, or paste the "
                                     "paths instead.")
                          .arg(what, folder->error.toHtmlEscaped());
        } else {
            message = QStringLiteral("Upload %1 into<br><b>%2</b><br>on %3?")
                          .arg(what, folder->path.toHtmlEscaped(), folder->where.toHtmlEscaped());
            if (folder->nested)
                message += QStringLiteral("<br><br>⚠ This terminal is in <b>%1</b> now: the files would go to "
                                          "%2, not to where that program is.")
                               .arg(folder->program.toHtmlEscaped(), folder->where.toHtmlEscaped());
            else if (!folder->program.isEmpty())
                message += QStringLiteral("<br><br>⚠ <b>%1</b> is running in this terminal; the folder is "
                                          "the one it is in.")
                               .arg(folder->program.toHtmlEscaped());
        }
        if (!folder->note.isEmpty())
            message += QStringLiteral("<br><br><i>%1</i>").arg(folder->note.toHtmlEscaped());
        text->setText(message);
        upload->setEnabled(!folder->path.isEmpty());
        // Nested: uploading is still possible, but not the default.
        (folder->path.isEmpty() || folder->nested ? choose : upload)->setDefault(true);
    };
    show();
    Choice choice = Choice::Cancel;
    QObject::connect(upload, &QPushButton::clicked, &dialog, [&] {
        choice = Choice::Upload;
        dialog.accept();
    });
    QObject::connect(paths, &QPushButton::clicked, &dialog, [&] {
        choice = Choice::PastePaths;
        dialog.accept();
    });
    QObject::connect(choose, &QPushButton::clicked, &dialog, [&] {
        const auto chosen = chooseFolder();
        if (chosen.isEmpty())
            return;
        folder->path = chosen;
        folder->program.clear();
        folder->nested = false;
        folder->note.clear();
        show();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
    return choice;
}

bool existingAction(QWidget *parent, const QStringList &clashing, Existing *action)
{
    const auto setting = loadSftpPreferences().conflict;
    *action = setting == QStringLiteral("overwrite") ? Existing::Overwrite
              : setting == QStringLiteral("rename")  ? Existing::Rename
                                                     : Existing::Skip;
    if (clashing.isEmpty() || setting != QStringLiteral("ask"))
        return true;
    QMessageBox box(QMessageBox::Question, QStringLiteral("Files Already There"),
                    clashing.size() == 1
                        ? QStringLiteral("\"%1\" is already there.").arg(clashing.first())
                        : QStringLiteral("%1 of them are already there:\n%2")
                              .arg(clashing.size())
                              .arg(clashing.mid(0, 8).join(QLatin1Char('\n'))
                                   + (clashing.size() > 8 ? QStringLiteral("\n…") : QString())),
                    QMessageBox::Cancel, parent);
    auto *overwrite = box.addButton(QStringLiteral("Replace"), QMessageBox::AcceptRole);
    auto *rename = box.addButton(QStringLiteral("Keep Both"), QMessageBox::AcceptRole);
    auto *skip = box.addButton(QStringLiteral("Skip"), QMessageBox::AcceptRole);
    box.setDefaultButton(rename);
    box.exec();
    if (box.clickedButton() == overwrite)
        *action = Existing::Overwrite;
    else if (box.clickedButton() == rename)
        *action = Existing::Rename;
    else if (box.clickedButton() == skip)
        *action = Existing::Skip;
    else
        return false;
    return true;
}

QString freeName(const QString &folder, const QString &name, const std::function<bool(const QString &)> &exists)
{
    const QFileInfo info(name);
    const auto base = info.isDir() || info.suffix().isEmpty() ? name : info.completeBaseName();
    const auto suffix = base == name ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 2;; ++n) {
        const auto candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix);
        if (!exists(QDir(folder).filePath(candidate)))
            return candidate;
    }
}

namespace {

// Copies a file or a whole folder; problems are added to the list.
void copyItem(const QString &source, const QString &target, bool overwrite, QStringList *problems)
{
    const QFileInfo info(source);
    if (info.isDir() && !info.isSymLink()) {
        if (!QDir().mkpath(target)) {
            problems->append(QStringLiteral("%1: the folder could not be created").arg(target));
            return;
        }
        QDirIterator entries(source, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (entries.hasNext()) {
            entries.next();
            copyItem(entries.filePath(), QDir(target).filePath(entries.fileName()), overwrite, problems);
        }
        return;
    }
    if (QFileInfo::exists(target)) {
        if (!overwrite)
            return;
        if (!QFile::remove(target)) {
            problems->append(QStringLiteral("%1: could not be replaced").arg(target));
            return;
        }
    }
    QFile file(source);
    if (!file.copy(target))
        problems->append(QStringLiteral("%1: %2").arg(info.fileName(), file.errorString()));
}

} // namespace

void copyLocal(QWidget *context, const QStringList &files, const QString &folder, Existing action,
               const std::function<void(const QStringList &problems)> &done)
{
    if (!QFileInfo(folder).isWritable()) {
        done({QStringLiteral("You cannot write to %1.").arg(folder)});
        return;
    }
    QPointer<QWidget> guarded(context);
    auto *thread = QThread::create([files, folder, action, done, guarded] {
        QStringList problems;
        for (const auto &file : files) {
            auto name = QFileInfo(file).fileName();
            const auto target = QDir(folder).filePath(name);
            if (QFileInfo::exists(target)) {
                if (action == Existing::Skip)
                    continue;
                if (action == Existing::Rename)
                    name = freeName(folder, name, [](const QString &path) { return QFileInfo::exists(path); });
            }
            copyItem(file, QDir(folder).filePath(name), action == Existing::Overwrite, &problems);
        }
        QMetaObject::invokeMethod(qApp, [guarded, done, problems] {
            if (guarded)
                done(problems);
        });
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void report(QWidget *parent, int count, const QString &target, const QStringList &problems)
{
    if (problems.isEmpty())
        return;
    // What went wrong in the message itself; a long list under Details.
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Upload Dropped Files"),
                    problems.size() >= count ? QStringLiteral("Nothing could be copied to %1.").arg(target)
                                             : QStringLiteral("Not everything reached %1.").arg(target),
                    QMessageBox::Ok, parent);
    box.setInformativeText(problems.size() <= 3
                               ? problems.join(QLatin1Char('\n'))
                               : QStringLiteral("%1 problems; see Details.").arg(problems.size()));
    if (problems.size() > 3)
        box.setDetailedText(problems.join(QLatin1Char('\n')));
    box.exec();
}

} // namespace FileDrop
