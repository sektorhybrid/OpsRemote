// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QTableWidget;
namespace YAML { class Node; }

// Text to look for in a terminal's output: plain text (any case), or a
// regular expression written as /…/.
QRegularExpression outputPattern(const QString &text);

// One step of a login script: wait for text, then type something.
struct LoginStep
{
    QString expect; // empty: send at once
    QString send; // {password}, {user} and {host} are filled in
    bool enter = true;
    int timeout = 0; // seconds; 0: the Preferences' default

    bool operator==(const LoginStep &) const = default;
};

// Watches the output for as long as the tab is open.
struct OutputTrigger
{
    QString pattern;
    // send (text, Enter appended), notify (desktop and status bar), beep,
    // or mark (the tab is marked like on activity).
    QString action = QStringLiteral("notify");
    QString text;
    bool once = false;
    bool enabled = true;

    bool operator==(const OutputTrigger &) const = default;
};

QList<QPair<QString, QString>> triggerActions();

// A session's or a folder's login script and triggers.
struct AutomationOptions
{
    // "": the folder's (in the end none); "own"; "none".
    QString mode;
    QList<LoginStep> loginScript;
    QList<OutputTrigger> triggers;
    // A saved script (Tools → Scripts) run once logged on; empty: none.
    QString afterLoginScript;

    bool isEmpty() const { return *this == AutomationOptions(); }
    bool operator==(const AutomationOptions &) const = default;
};

AutomationOptions automationFromYaml(const YAML::Node &node);
void automationToYaml(const AutomationOptions &options, YAML::Node &node);
// The same as one line of YAML, for folders (kept in the session tree).
AutomationOptions automationFromText(const QString &text);
QString automationToText(const AutomationOptions &options);

// Preferences → Terminal → Automation (settings.yaml "automation:").
struct AutomationPreferences
{
    bool loginScripts = true;
    bool triggers = true;
    int stepTimeout = 20; // seconds
    // A step that times out stops the script (or the next one is tried).
    bool stopOnTimeout = true;
    // Whether triggers may type {password}; login scripts always may.
    bool triggersMaySendPassword = false;
    // For every terminal tab, before the session's own.
    QList<OutputTrigger> globalTriggers;

    bool operator==(const AutomationPreferences &) const = default;
};

AutomationPreferences loadAutomationPreferences();
void saveAutomationPreferences(const AutomationPreferences &preferences);

// Runs a login script and triggers against a terminal's output.
class AutomationRunner final : public QObject
{
    Q_OBJECT

public:
    using Sender = std::function<void(const QByteArray &)>;

    AutomationRunner(Sender send, QObject *parent = nullptr);

    // values: password, user, host (for the {…} fields).
    void configure(const QList<LoginStep> &script, const QList<OutputTrigger> &triggers,
                   const AutomationPreferences &preferences, const QHash<QString, QString> &values);
    bool hasScript() const { return !m_script.isEmpty(); }
    bool isRunning() const { return m_step >= 0; }
    // Starts the login script from its first step (after connecting).
    void start();
    void stop(const QString &why = QString());
    void feed(const QByteArray &output);

signals:
    void status(const QString &message);
    void notify(const QString &message);
    void beep();
    void markTab();
    // The login script finished all its steps.
    void scriptFinished();

private:
    void runStep();
    void stepTimedOut();
    QByteArray fill(const QString &text, bool allowPassword) const;

    Sender m_send;
    QList<LoginStep> m_script;
    struct Trigger
    {
        OutputTrigger rule;
        QRegularExpression expression;
        bool fired = false;
    };
    QList<Trigger> m_triggers;
    AutomationPreferences m_preferences;
    QHash<QString, QString> m_values;
    int m_step = -1;
    QRegularExpression m_expect;
    QString m_scriptBuffer;
    QString m_triggerBuffer;
    class QTimer *m_timer = nullptr;
};

// The rows of triggers, for sessions, folders and the Preferences.
class TriggerTable final : public QWidget
{
    Q_OBJECT

public:
    TriggerTable(const QList<OutputTrigger> &value, QWidget *parent = nullptr);
    QList<OutputTrigger> value() const;

signals:
    void changed();

private:
    void addRow(const OutputTrigger &trigger);
    QTableWidget *m_table = nullptr;
};

// The session window's (and folder window's) Automation tab.
class AutomationEditor final : public QWidget
{
    Q_OBJECT

public:
    // inheritLabel: what the first choice says, e.g. "The folder's (none)".
    // scripts: the saved scripts' names for "After logging on".
    AutomationEditor(const AutomationOptions &value, const QString &inheritLabel,
                     const QStringList &scripts, QWidget *parent = nullptr);
    AutomationOptions value() const;

private:
    void addStep(const LoginStep &step);
    void updateEnabled();
    QComboBox *m_mode = nullptr;
    QTableWidget *m_steps = nullptr;
    TriggerTable *m_triggers = nullptr;
    QComboBox *m_afterLogin = nullptr;
    QWidget *m_own = nullptr;
};
