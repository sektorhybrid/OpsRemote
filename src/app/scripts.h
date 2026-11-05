// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
namespace YAML { class Node; }

// A saved script (Tools → Scripts): a file in settings/scripts/ that runs on
// a server as a whole, not typed line by line.
struct SavedScript
{
    QString name; // the file name without extension
    QString group;
    QString description;
    // sh, bash, python3, pwsh, or another program on the server.
    QString interpreter = QStringLiteral("sh");
    bool sudo = false;
    QString body;

    // The {name} fields asked for when it runs ({user}, {host} and
    // {password} are not asked).
    QStringList parameters() const;
    bool operator==(const SavedScript &) const = default;
};

QString scriptsFolder();
QList<SavedScript> loadSavedScripts();
std::optional<SavedScript> findSavedScript(const QString &name);
// Saves (renaming from oldName when it differs); false with the reason.
bool saveSavedScript(const SavedScript &script, const QString &oldName, QString *error);
bool deleteSavedScript(const QString &name);

// Preferences → Tools → Scripts (settings.yaml "scripts:").
struct ScriptPreferences
{
    bool enabled = true;
    QString interpreter = QStringLiteral("sh"); // for new scripts
    int confirmAbove = 10; // hosts; 0: never ask

    bool operator==(const ScriptPreferences &) const = default;
};

ScriptPreferences loadScriptPreferences();
void saveScriptPreferences(const ScriptPreferences &preferences);

// Asks for the script's {fields}; values has user and host already. False
// when cancelled.
bool askScriptParameters(const SavedScript &script, QHash<QString, QString> *values, QWidget *parent);
// The body with the {fields} filled in.
QString filledScript(const SavedScript &script, const QHash<QString, QString> &values);

// For a terminal: a command (run over its own channel) that stores the
// script in a private temporary file and prints the file's path...
QString storeScriptCommand(const QString &body);
// ...and the short line typed into the shell to run it and remove it.
QString runStoredScriptLine(const SavedScript &script, const QString &path);
// For Run on Hosts: one command that stores, runs, and removes the script,
// with the script's exit status (sudo without asking: sudo -n).
QString runScriptCommand(const SavedScript &script, const QString &body);
// For a shell where no file can be stored (a container): the script as one
// here-document for its interpreter, lines ending in CR as typed.
QString typedScript(const SavedScript &script, const QString &body);
// Run on Hosts' command box holds "#!script NAME" for a saved script.
QString scriptMarker(const QString &name);
QString scriptFromMarker(const QString &command);

// Tools → Scripts: the list of scripts and an editor.
class ScriptsWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit ScriptsWindow(QWidget *parent = nullptr);
    void select(const QString &name);
    // A new script, selected for editing.
    void newScript();

signals:
    // Run the selected script in the current tab, or on hosts.
    void runInTabRequested(const QString &name);
    void runOnHostsRequested(const QString &name);
    void scriptsChanged();

private:
    void refresh(const QString &select = QString());
    void showSelected();
    bool saveCurrent();
    void duplicateScript();
    void deleteScript();
    void importScript();
    SavedScript edited() const;

    QListWidget *m_list = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_group = nullptr;
    QLineEdit *m_description = nullptr;
    QComboBox *m_interpreter = nullptr;
    QCheckBox *m_sudo = nullptr;
    QPlainTextEdit *m_body = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_runInTab = nullptr;
    QPushButton *m_runOnHosts = nullptr;
    QString m_current; // the name of the script being edited
    bool m_loading = false;
};
