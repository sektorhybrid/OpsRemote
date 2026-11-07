// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "programpicker.h"
#include "filedialogs.h"

#include "statusline.h"

#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {

struct Program
{
    QString name;
    QString command; // as it goes into the field
};

// The program part of a command line, found on PATH or as a path.
QString resolve(const QString &commandLine)
{
    const auto parts = QProcess::splitCommand(commandLine);
    if (parts.isEmpty())
        return QString();
    const QFileInfo direct(parts.first());
    if (direct.isAbsolute())
        return direct.isFile() ? direct.absoluteFilePath() : QString();
    return QStandardPaths::findExecutable(parts.first());
}

QString quoted(const QString &path)
{
    return path.contains(QLatin1Char(' ')) ? QLatin1Char('"') + path + QLatin1Char('"') : path;
}

#ifdef Q_OS_WIN
// Known editors where they install, and Windows' App Paths registrations of
// them (programs that register their location there).
QList<Program> textEditors()
{
    QList<Program> found;
    // Each program once, however it was found.
    const auto add = [&](const QString &name, const QString &path) {
        if (path.isEmpty() || !QFileInfo(path).isFile())
            return;
        const auto file = QFileInfo(path).fileName().toLower();
        for (const auto &program : std::as_const(found))
            if (QFileInfo(QString(program.command).remove(QLatin1Char('"'))).fileName().toLower() == file)
                return;
        found.append({name, quoted(QDir::toNativeSeparators(path))});
    };
    const auto env = [](const char *name) { return qEnvironmentVariable(name); };
    add(QStringLiteral("Notepad"), env("SystemRoot") + QStringLiteral("\\notepad.exe"));
    const QList<std::pair<QString, QStringList>> known{
        {QStringLiteral("Visual Studio Code"),
         {env("LOCALAPPDATA") + QStringLiteral("\\Programs\\Microsoft VS Code\\Code.exe"),
          env("ProgramFiles") + QStringLiteral("\\Microsoft VS Code\\Code.exe")}},
        {QStringLiteral("Notepad++"),
         {env("ProgramFiles") + QStringLiteral("\\Notepad++\\notepad++.exe"),
          env("ProgramFiles(x86)") + QStringLiteral("\\Notepad++\\notepad++.exe")}},
        {QStringLiteral("Sublime Text"), {env("ProgramFiles") + QStringLiteral("\\Sublime Text\\sublime_text.exe")}},
        {QStringLiteral("Notepad3"), {env("ProgramFiles") + QStringLiteral("\\Notepad3\\Notepad3.exe")}},
        {QStringLiteral("Kate"), {env("ProgramFiles") + QStringLiteral("\\Kate\\bin\\kate.exe")}},
    };
    for (const auto &[name, paths] : known)
        for (const auto &path : paths)
            add(name, path);
    for (const auto *root : {"HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE"}) {
        QSettings appPaths(QStringLiteral("%1\\Software\\Microsoft\\Windows\\CurrentVersion\\App Paths").arg(QLatin1String(root)),
                           QSettings::NativeFormat);
        for (const auto &exe : appPaths.childGroups()) {
            static const QStringList editors{QStringLiteral("code.exe"), QStringLiteral("notepad++.exe"),
                                             QStringLiteral("sublime_text.exe"), QStringLiteral("notepad3.exe"),
                                             QStringLiteral("kate.exe"), QStringLiteral("gvim.exe")};
            if (editors.contains(exe.toLower()))
                add(QFileInfo(exe).completeBaseName(),
                    appPaths.value(exe + QStringLiteral("/Default")).toString().remove(QLatin1Char('"')));
        }
    }
    return found;
}
#else
// Installed applications (freedesktop .desktop entries) that open plain text.
QList<Program> textEditors()
{
    QList<Program> found;
    QStringList seen;
    auto folders = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    folders << QStringLiteral("/var/lib/flatpak/exports/share/applications")
            << QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/applications");
    for (const auto &folder : std::as_const(folders)) {
        QDirIterator files(folder, {QStringLiteral("*.desktop")}, QDir::Files);
        while (files.hasNext()) {
            const auto path = files.next();
            if (seen.contains(files.fileName()))
                continue; // the user's own entry hides the system's
            seen.append(files.fileName());
            QSettings entry(path, QSettings::IniFormat);
            entry.beginGroup(QStringLiteral("Desktop Entry"));
            const auto mime = entry.value(QStringLiteral("MimeType")).toString();
            if (!mime.contains(QStringLiteral("text/plain")) || entry.value(QStringLiteral("NoDisplay")).toBool()
                || entry.value(QStringLiteral("Hidden")).toBool())
                continue;
            // Exec without the %f-style field codes: the file is added on use.
            auto exec = entry.value(QStringLiteral("Exec")).toString();
            exec.remove(QRegularExpression(QStringLiteral("\\s*%[fFuUdDnNickvm]")));
            if (exec.trimmed().isEmpty() || resolve(exec).isEmpty())
                continue;
            found.append({entry.value(QStringLiteral("Name")).toString(), exec.trimmed()});
        }
    }
    std::sort(found.begin(), found.end(), [](const Program &a, const Program &b) {
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return found;
}
#endif

} // namespace

ProgramPicker::ProgramPicker(Kind kind, const QString &value, const QString &placeholder, QWidget *parent)
    : QWidget(parent)
{
    Q_UNUSED(kind); // one kind so far
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *row = new QHBoxLayout;
    m_list = new QComboBox(this);
    m_list->addItem(QStringLiteral("Choose a program…"), QString());
    for (const auto &program : textEditors())
        m_list->addItem(program.name, program.command);
    m_list->setToolTip(QStringLiteral("Programs found on this computer"));
    auto *browse = new QPushButton(QStringLiteral("Browse…"), this);
    row->addWidget(m_list, 1);
    row->addWidget(browse);
    layout->addLayout(row);
    m_command = new QLineEdit(value, this);
    m_command->setPlaceholderText(placeholder);
    m_command->setClearButtonEnabled(true);
    m_command->setMinimumWidth(330);
    m_command->setToolTip(QStringLiteral("The program and its arguments; the file goes after them"));
    layout->addWidget(m_command);
    m_status = new StatusLine(this);
    layout->addWidget(m_status);

    connect(m_list, &QComboBox::activated, this, [this](int index) {
        const auto command = m_list->itemData(index).toString();
        if (command.isEmpty())
            return;
        m_command->setText(command);
        check();
        emit changed();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto current = resolve(m_command->text());
        QFileDialog dialog(this, QStringLiteral("Choose a Program"),
                           current.isEmpty() ? QDir::homePath() : QFileInfo(current).absolutePath());
        dialog.setFileMode(QFileDialog::ExistingFile);
        FileDialogs::useQuickTypes(&dialog);
        if (!current.isEmpty())
            dialog.selectFile(current);
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
            return;
        const auto chosen = dialog.selectedFiles().constFirst();
        m_command->setText(quoted(QDir::toNativeSeparators(chosen)));
        check();
        emit changed();
    });
    connect(m_command, &QLineEdit::textChanged, this, &ProgramPicker::check);
    connect(m_command, &QLineEdit::editingFinished, this, &ProgramPicker::changed);
    check();
}

QString ProgramPicker::value() const
{
    return m_command->text().trimmed();
}

void ProgramPicker::check()
{
    const auto command = value();
    // The list shows the program in use when it is one of them (the same
    // command, or the same program file), else "Choose a program…".
    int listed = 0;
    const auto program = resolve(command);
    for (int index = 1; index < m_list->count() && !command.isEmpty(); ++index) {
        const auto entry = m_list->itemData(index).toString();
        if (entry == command || (!program.isEmpty() && resolve(entry) == program)) {
            listed = index;
            break;
        }
    }
    m_list->setCurrentIndex(listed);
    if (command.isEmpty()) {
        m_status->clearStatus();
        return;
    }
    if (program.isEmpty())
        m_status->setStatus(QStringLiteral("Not found: %1").arg(QProcess::splitCommand(command).value(0)), true);
    else
        m_status->setStatus(QStringLiteral("Found: %1").arg(QDir::toNativeSeparators(program)));
}
