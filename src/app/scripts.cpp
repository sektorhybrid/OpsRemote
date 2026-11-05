// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "scripts.h"
#include "appdirs.h"
#include "preferences.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>

#include <yaml-cpp/yaml.h>

namespace {
constexpr char HeaderPrefix[] = "# OpsRemote-";

QString extensionFor(const QString &interpreter)
{
    const auto program = QFileInfo(interpreter.section(QLatin1Char(' '), 0, 0)).fileName();
    if (program.startsWith(QStringLiteral("python")))
        return QStringLiteral("py");
    if (program == QStringLiteral("pwsh") || program.startsWith(QStringLiteral("powershell")))
        return QStringLiteral("ps1");
    if (program == QStringLiteral("perl"))
        return QStringLiteral("pl");
    return QStringLiteral("sh");
}

// A script file: "# OpsRemote-Key: value" lines at the top (after an
// optional #! line), then the script.
std::optional<SavedScript> readScript(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    SavedScript script;
    script.name = QFileInfo(path).completeBaseName();
    const auto lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    int index = 0;
    if (index < lines.size() && lines[index].startsWith(QStringLiteral("#!")))
        ++index;
    for (; index < lines.size() && lines[index].startsWith(QLatin1String(HeaderPrefix)); ++index) {
        const auto entry = lines[index].mid(int(qstrlen(HeaderPrefix)));
        const auto key = entry.section(QLatin1Char(':'), 0, 0).trimmed().toLower();
        const auto value = entry.section(QLatin1Char(':'), 1).trimmed();
        if (key == QStringLiteral("interpreter") && !value.isEmpty())
            script.interpreter = value;
        else if (key == QStringLiteral("sudo"))
            script.sudo = value == QStringLiteral("yes");
        else if (key == QStringLiteral("group"))
            script.group = value;
        else if (key == QStringLiteral("description"))
            script.description = value;
    }
    script.body = lines.mid(index).join(QLatin1Char('\n'));
    if (script.body.endsWith(QLatin1Char('\n')))
        script.body.chop(1);
    return script;
}

// The file of a script, whatever its extension; empty when none.
QString fileOf(const QString &name)
{
    const QDir folder(scriptsFolder());
    for (const auto &file : folder.entryList(QDir::Files)) {
        if (QFileInfo(file).completeBaseName() == name)
            return folder.filePath(file);
    }
    return {};
}

QString shellQuote(const QString &text)
{
    return QLatin1Char('\'') + QString(text).replace(QLatin1Char('\''), QStringLiteral("'\\''"))
           + QLatin1Char('\'');
}

QString interpreterCommand(const SavedScript &script)
{
    return script.interpreter.trimmed().isEmpty() ? QStringLiteral("sh") : script.interpreter.trimmed();
}

QString base64Of(const QString &body)
{
    auto text = body;
    if (!text.endsWith(QLatin1Char('\n')))
        text += QLatin1Char('\n');
    return QString::fromLatin1(text.toUtf8().toBase64());
}

const QStringList &builtInFields()
{
    static const QStringList fields{QStringLiteral("user"), QStringLiteral("host"),
                                    QStringLiteral("password")};
    return fields;
}
}

QStringList SavedScript::parameters() const
{
    static const QRegularExpression field(QStringLiteral("\\{([A-Za-z_][A-Za-z0-9_-]*)\\}"));
    QStringList names;
    for (const auto &match : field.globalMatch(body)) {
        const auto name = match.captured(1);
        if (!builtInFields().contains(name) && !names.contains(name))
            names.append(name);
    }
    return names;
}

QString scriptsFolder()
{
    return portableFilePath(QStringLiteral("settings/scripts"));
}

QList<SavedScript> loadSavedScripts()
{
    QList<SavedScript> scripts;
    const QDir folder(scriptsFolder());
    for (const auto &file : folder.entryList(QDir::Files, QDir::Name)) {
        if (auto script = readScript(folder.filePath(file)))
            scripts.append(*script);
    }
    std::sort(scripts.begin(), scripts.end(), [](const SavedScript &a, const SavedScript &b) {
        const int group = a.group.compare(b.group, Qt::CaseInsensitive);
        return group != 0 ? group < 0 : a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return scripts;
}

std::optional<SavedScript> findSavedScript(const QString &name)
{
    const auto path = fileOf(name);
    return path.isEmpty() ? std::nullopt : readScript(path);
}

bool saveSavedScript(const SavedScript &script, const QString &oldName, QString *error)
{
    static const QRegularExpression badName(QStringLiteral("[\\\\/:*?\"<>|]"));
    if (script.name.trimmed().isEmpty() || script.name.contains(badName) || script.name.startsWith(QLatin1Char('.'))) {
        *error = QStringLiteral("Give the script a name without / \\ : * ? \" < > |.");
        return false;
    }
    if (script.name != oldName && !fileOf(script.name).isEmpty()) {
        *error = QStringLiteral("There is a script called %1 already.").arg(script.name);
        return false;
    }
    QDir().mkpath(scriptsFolder());
    const auto path = QDir(scriptsFolder()).filePath(script.name + QLatin1Char('.') + extensionFor(script.interpreter));
    QString text;
    const auto header = [&text](const char *key, const QString &value) {
        if (!value.isEmpty())
            text += QStringLiteral("%1%2: %3\n").arg(QLatin1String(HeaderPrefix), QLatin1String(key), value);
    };
    header("Interpreter", script.interpreter);
    header("Sudo", script.sudo ? QStringLiteral("yes") : QString());
    header("Group", script.group);
    header("Description", script.description.simplified());
    text += script.body;
    if (!text.endsWith(QLatin1Char('\n')))
        text += QLatin1Char('\n');
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0 || !file.commit()) {
        *error = QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    // Renamed, or saved with another interpreter's extension.
    if (!oldName.isEmpty()) {
        const QDir folder(scriptsFolder());
        for (const auto &other : folder.entryList(QDir::Files)) {
            const auto otherPath = folder.filePath(other);
            if (QFileInfo(other).completeBaseName() == oldName && QFileInfo(otherPath) != QFileInfo(path))
                QFile::remove(otherPath);
        }
    }
    return true;
}

bool deleteSavedScript(const QString &name)
{
    const auto path = fileOf(name);
    return !path.isEmpty() && QFile::remove(path);
}

ScriptPreferences loadScriptPreferences()
{
    const auto node = loadPreferencesSection("scripts");
    ScriptPreferences value;
    try {
        if (node["enabled"])
            value.enabled = node["enabled"].as<bool>();
        if (node["interpreter"])
            value.interpreter = QString::fromStdString(node["interpreter"].as<std::string>());
        if (node["confirm_above"])
            value.confirmAbove = qBound(0, node["confirm_above"].as<int>(), 10000);
    } catch (const YAML::Exception &) {
    }
    return value;
}

void saveScriptPreferences(const ScriptPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["enabled"] = value.enabled;
    node["interpreter"] = value.interpreter.toStdString();
    node["confirm_above"] = value.confirmAbove;
    savePreferencesSection("scripts", node);
}

bool askScriptParameters(const SavedScript &script, QHash<QString, QString> *values, QWidget *parent)
{
    const auto names = script.parameters();
    if (names.isEmpty())
        return true;
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Run %1").arg(script.name));
    auto *form = new QFormLayout(&dialog);
    if (!script.description.isEmpty()) {
        auto *description = new QLabel(script.description, &dialog);
        description->setWordWrap(true);
        form->addRow(description);
    }
    QHash<QString, QLineEdit *> edits;
    for (const auto &name : names) {
        auto *edit = new QLineEdit(values->value(name), &dialog);
        form->addRow(name + QLatin1Char(':'), edit);
        edits.insert(name, edit);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Run"));
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    for (auto it = edits.cbegin(); it != edits.cend(); ++it)
        values->insert(it.key(), it.value()->text());
    return true;
}

QString filledScript(const SavedScript &script, const QHash<QString, QString> &values)
{
    auto body = script.body;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        body.replace(QStringLiteral("{%1}").arg(it.key()), it.value());
    return body;
}

QString storeScriptCommand(const QString &body)
{
    return QStringLiteral("umask 077; f=$(mktemp \"${TMPDIR:-/tmp}/opsremote-XXXXXX\") && "
                          "printf '%s' '%1' | base64 -d > \"$f\" && echo \"OPSREMOTE-SCRIPT:$f\"")
        .arg(base64Of(body));
}

QString runStoredScriptLine(const SavedScript &script, const QString &path)
{
    // A leading space keeps it out of most shells' history.
    return QStringLiteral(" %1%2 %3; rm -f %3")
        .arg(script.sudo ? QStringLiteral("sudo ") : QString(), interpreterCommand(script), shellQuote(path));
}

QString runScriptCommand(const SavedScript &script, const QString &body)
{
    return QStringLiteral("umask 077; f=$(mktemp \"${TMPDIR:-/tmp}/opsremote-XXXXXX\") || exit 1; "
                          "printf '%s' '%1' | base64 -d > \"$f\"; %2%3 \"$f\"; s=$?; rm -f \"$f\"; exit $s")
        .arg(base64Of(body), script.sudo ? QStringLiteral("sudo -n ") : QString(), interpreterCommand(script));
}

QString typedScript(const SavedScript &script, const QString &body)
{
    // A marker no script line is likely to be.
    const QString marker = QStringLiteral("OPSREMOTE_SCRIPT_END");
    auto text = body;
    if (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    return QStringLiteral(" %1%2 /dev/stdin <<'%3'\r%4\r%3")
        .arg(script.sudo ? QStringLiteral("sudo ") : QString(), interpreterCommand(script), marker, text);
}

QString scriptMarker(const QString &name)
{
    return QStringLiteral("#!script ") + name;
}

QString scriptFromMarker(const QString &command)
{
    const auto line = command.trimmed();
    if (!line.startsWith(QStringLiteral("#!script ")) || line.contains(QLatin1Char('\n')))
        return {};
    return line.mid(9).trimmed();
}

ScriptsWindow::ScriptsWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(QStringLiteral("Scripts"));
    resize(900, 600);
    auto *layout = new QVBoxLayout(this);
    auto *splitter = new QSplitter(this);

    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    m_list = new QListWidget(left);
    leftLayout->addWidget(m_list, 1);
    auto *listButtons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("New"), left);
    auto *duplicate = new QPushButton(QStringLiteral("Duplicate"), left);
    auto *remove = new QPushButton(QStringLiteral("Delete…"), left);
    for (auto *button : {add, duplicate, remove})
        listButtons->addWidget(button);
    leftLayout->addLayout(listButtons);
    auto *moreButtons = new QHBoxLayout;
    auto *import = new QPushButton(QStringLiteral("Import…"), left);
    auto *folder = new QPushButton(QStringLiteral("Open Folder"), left);
    moreButtons->addWidget(import);
    moreButtons->addWidget(folder);
    leftLayout->addLayout(moreButtons);

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto *form = new QFormLayout;
    m_name = new QLineEdit(right);
    m_group = new QLineEdit(right);
    m_group->setPlaceholderText(QStringLiteral("Optional, e.g. Linux or Cisco"));
    m_description = new QLineEdit(right);
    m_interpreter = new QComboBox(right);
    m_interpreter->setEditable(true);
    for (const auto *program : {"sh", "bash", "python3", "pwsh", "perl"})
        m_interpreter->addItem(QString::fromLatin1(program));
    m_interpreter->setToolTip(QStringLiteral("The program on the server that runs the script."));
    m_sudo = new QCheckBox(QStringLiteral("Run as root (sudo)"), right);
    m_sudo->setToolTip(QStringLiteral("In a tab, sudo asks for the password in the terminal; on many "
                                      "hosts it must not need one (sudo -n)."));
    form->addRow(QStringLiteral("Name:"), m_name);
    form->addRow(QStringLiteral("Group:"), m_group);
    form->addRow(QStringLiteral("Description:"), m_description);
    form->addRow(QStringLiteral("Interpreter:"), m_interpreter);
    form->addRow(QString(), m_sudo);
    rightLayout->addLayout(form);
    m_body = new QPlainTextEdit(right);
    m_body->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_body->setPlaceholderText(QStringLiteral("The script. {name} fields are asked for when it runs; "
                                              "{user} and {host} are the session's."));
    rightLayout->addWidget(m_body, 1);
    auto *runButtons = new QHBoxLayout;
    m_save = new QPushButton(QStringLiteral("Save"), right);
    m_runInTab = new QPushButton(QStringLiteral("Run in Current Tab"), right);
    m_runOnHosts = new QPushButton(QStringLiteral("Run on Hosts…"), right);
    runButtons->addWidget(m_save);
    runButtons->addStretch();
    runButtons->addWidget(m_runInTab);
    runButtons->addWidget(m_runOnHosts);
    rightLayout->addLayout(runButtons);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 640});
    layout->addWidget(splitter);
    auto *note = new QLabel(QStringLiteral("Scripts are kept in settings/scripts/ and travel with "
                                           "OpsRemote. They are sent to the server whole and run "
                                           "there, not typed line by line."), this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);

    connect(m_list, &QListWidget::currentItemChanged, this, [this] {
        if (!m_loading)
            showSelected();
    });
    connect(add, &QPushButton::clicked, this, &ScriptsWindow::newScript);
    connect(duplicate, &QPushButton::clicked, this, &ScriptsWindow::duplicateScript);
    connect(remove, &QPushButton::clicked, this, &ScriptsWindow::deleteScript);
    connect(import, &QPushButton::clicked, this, &ScriptsWindow::importScript);
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(scriptsFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(scriptsFolder()));
    });
    connect(m_save, &QPushButton::clicked, this, &ScriptsWindow::saveCurrent);
    connect(m_runInTab, &QPushButton::clicked, this, [this] {
        if (saveCurrent())
            emit runInTabRequested(m_current);
    });
    connect(m_runOnHosts, &QPushButton::clicked, this, [this] {
        if (saveCurrent())
            emit runOnHostsRequested(m_current);
    });
    refresh();
}

void ScriptsWindow::select(const QString &name)
{
    refresh(name);
}

void ScriptsWindow::refresh(const QString &select)
{
    const QString wanted = select.isEmpty() ? m_current : select;
    m_loading = true;
    m_list->clear();
    for (const auto &script : loadSavedScripts()) {
        auto *item = new QListWidgetItem(script.group.isEmpty()
                                             ? script.name
                                             : QStringLiteral("%1 › %2").arg(script.group, script.name),
                                         m_list);
        item->setData(Qt::UserRole, script.name);
        item->setToolTip(script.description);
        if (script.name == wanted)
            m_list->setCurrentItem(item);
    }
    if (!m_list->currentItem() && m_list->count() > 0)
        m_list->setCurrentRow(0);
    m_loading = false;
    showSelected();
}

void ScriptsWindow::showSelected()
{
    auto *item = m_list->currentItem();
    const auto script = item ? findSavedScript(item->data(Qt::UserRole).toString()) : std::nullopt;
    m_current = script ? script->name : QString();
    const SavedScript shown = script.value_or(SavedScript{});
    m_name->setText(shown.name);
    m_group->setText(shown.group);
    m_description->setText(shown.description);
    m_interpreter->setEditText(shown.interpreter);
    m_sudo->setChecked(shown.sudo);
    m_body->setPlainText(shown.body);
    const bool any = script.has_value();
    for (QWidget *widget : std::initializer_list<QWidget *>{m_name, m_group, m_description, m_interpreter,
                                                            m_sudo, m_body, m_save, m_runInTab, m_runOnHosts})
        widget->setEnabled(any);
}

SavedScript ScriptsWindow::edited() const
{
    SavedScript script;
    script.name = m_name->text().trimmed();
    script.group = m_group->text().trimmed();
    script.description = m_description->text().trimmed();
    script.interpreter = m_interpreter->currentText().trimmed();
    script.sudo = m_sudo->isChecked();
    script.body = m_body->toPlainText();
    return script;
}

bool ScriptsWindow::saveCurrent()
{
    if (m_current.isEmpty())
        return false;
    const auto script = edited();
    if (findSavedScript(m_current) == script)
        return true;
    QString error;
    if (!saveSavedScript(script, m_current, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return false;
    }
    m_current = script.name;
    refresh(script.name);
    emit scriptsChanged();
    return true;
}

void ScriptsWindow::newScript()
{
    if (!m_current.isEmpty() && !saveCurrent())
        return;
    SavedScript script;
    script.interpreter = loadScriptPreferences().interpreter;
    script.name = QStringLiteral("New script");
    for (int number = 2; findSavedScript(script.name); ++number)
        script.name = QStringLiteral("New script %1").arg(number);
    script.body = QStringLiteral("echo \"Hello from $(hostname)\"");
    QString error;
    if (!saveSavedScript(script, QString(), &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    refresh(script.name);
    m_name->setFocus();
    m_name->selectAll();
    emit scriptsChanged();
}

void ScriptsWindow::duplicateScript()
{
    if (m_current.isEmpty() || !saveCurrent())
        return;
    auto script = edited();
    const auto base = script.name;
    script.name = QStringLiteral("%1 copy").arg(base);
    for (int number = 2; findSavedScript(script.name); ++number)
        script.name = QStringLiteral("%1 copy %2").arg(base).arg(number);
    QString error;
    if (!saveSavedScript(script, QString(), &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    refresh(script.name);
    emit scriptsChanged();
}

void ScriptsWindow::deleteScript()
{
    if (m_current.isEmpty())
        return;
    if (QMessageBox::question(this, windowTitle(), QStringLiteral("Delete the script %1?").arg(m_current))
        != QMessageBox::Yes)
        return;
    deleteSavedScript(m_current);
    m_current.clear();
    refresh();
    emit scriptsChanged();
}

void ScriptsWindow::importScript()
{
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Import a Script"), QDir::homePath(),
                                                   QStringLiteral("Scripts (*.sh *.bash *.py *.ps1 *.pl);;All files (*)"));
    if (path.isEmpty())
        return;
    auto script = readScript(path);
    if (!script)
        return;
    // The #! line says which interpreter, when there is no header.
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const auto first = QString::fromUtf8(file.readLine()).trimmed();
        if (first.startsWith(QStringLiteral("#!")) && !first.contains(QLatin1String(HeaderPrefix))) {
            auto program = QFileInfo(first.mid(2).section(QLatin1Char(' '), 0, 0)).fileName();
            if (program == QStringLiteral("env"))
                program = first.section(QLatin1Char(' '), 1, 1);
            if (!program.isEmpty())
                script->interpreter = program;
        }
    }
    const auto base = script->name;
    for (int number = 2; findSavedScript(script->name); ++number)
        script->name = QStringLiteral("%1 %2").arg(base).arg(number);
    QString error;
    if (!saveSavedScript(*script, QString(), &error)) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    refresh(script->name);
    emit scriptsChanged();
}
