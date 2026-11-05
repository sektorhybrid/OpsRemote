// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "automation.h"
#include "preferences.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <yaml-cpp/yaml.h>

namespace {
// Output is matched without colours, cursor movement and window titles.
QString plainText(const QByteArray &data)
{
    static const QRegularExpression escapes(
        QStringLiteral("\\x1b\\[[0-9;?<>=]*[ -/]*[@-~]|\\x1b\\][^\\x07\\x1b]*(?:\\x07|\\x1b\\\\)|"
                       "\\x1b[()][0-9A-Za-z]|\\x1b[=>78DEHMNOZc]|\\r"));
    return QString::fromUtf8(data).remove(escapes);
}

// What is kept of the output while waiting: enough for a prompt, not a
// whole file listing.
constexpr qsizetype BufferLimit = 8192;

void keepTail(QString &buffer)
{
    if (buffer.size() > BufferLimit)
        buffer.remove(0, buffer.size() - BufferLimit);
}

QString text(const YAML::Node &node, const char *key)
{
    try {
        if (node[key])
            return QString::fromStdString(node[key].as<std::string>());
    } catch (const YAML::Exception &) {
    }
    return {};
}

bool flag(const YAML::Node &node, const char *key, bool fallback)
{
    try {
        if (node[key])
            return node[key].as<bool>();
    } catch (const YAML::Exception &) {
    }
    return fallback;
}

int number(const YAML::Node &node, const char *key, int fallback, int low, int high)
{
    try {
        if (node[key])
            return qBound(low, node[key].as<int>(), high);
    } catch (const YAML::Exception &) {
    }
    return fallback;
}

QList<OutputTrigger> triggersFromYaml(const YAML::Node &node)
{
    QList<OutputTrigger> triggers;
    if (!node || !node.IsSequence())
        return triggers;
    QStringList actions;
    for (const auto &[action, label] : triggerActions())
        actions << action;
    for (const auto &item : node) {
        OutputTrigger trigger;
        trigger.pattern = text(item, "pattern");
        trigger.action = text(item, "action");
        if (!actions.contains(trigger.action))
            trigger.action = QStringLiteral("notify");
        trigger.text = text(item, "text");
        trigger.once = flag(item, "once", false);
        trigger.enabled = flag(item, "enabled", true);
        if (!trigger.pattern.isEmpty())
            triggers.append(trigger);
    }
    return triggers;
}

YAML::Node triggersToYaml(const QList<OutputTrigger> &triggers)
{
    YAML::Node node(YAML::NodeType::Sequence);
    for (const auto &trigger : triggers) {
        YAML::Node item(YAML::NodeType::Map);
        item["pattern"] = trigger.pattern.toStdString();
        item["action"] = trigger.action.toStdString();
        if (!trigger.text.isEmpty())
            item["text"] = trigger.text.toStdString();
        if (trigger.once)
            item["once"] = true;
        if (!trigger.enabled)
            item["enabled"] = false;
        node.push_back(item);
    }
    return node;
}

// Add, Remove, Up and Down under a table.
QHBoxLayout *rowButtons(QTableWidget *table, const std::function<void()> &add, QWidget *parent)
{
    auto *layout = new QHBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    auto *addButton = new QPushButton(QStringLiteral("Add"), parent);
    auto *remove = new QPushButton(QStringLiteral("Remove"), parent);
    auto *up = new QPushButton(QStringLiteral("Up"), parent);
    auto *down = new QPushButton(QStringLiteral("Down"), parent);
    for (auto *button : {addButton, remove, up, down})
        layout->addWidget(button);
    layout->addStretch();
    QObject::connect(addButton, &QPushButton::clicked, table, [table, add] {
        add();
        table->setCurrentCell(table->rowCount() - 1, 0);
        table->editItem(table->item(table->rowCount() - 1, table->columnCount() > 4 ? 1 : 0));
    });
    QObject::connect(remove, &QPushButton::clicked, table, [table] {
        if (table->currentRow() >= 0)
            table->removeRow(table->currentRow());
    });
    // Moves a row with its items and cell widgets.
    const auto move = [table](int by) {
        const int row = table->currentRow();
        const int target = row + by;
        if (row < 0 || target < 0 || target >= table->rowCount())
            return;
        for (int column = 0; column < table->columnCount(); ++column) {
            auto *a = table->takeItem(row, column);
            auto *b = table->takeItem(target, column);
            table->setItem(row, column, b);
            table->setItem(target, column, a);
            auto *combo = qobject_cast<QComboBox *>(table->cellWidget(row, column));
            auto *other = qobject_cast<QComboBox *>(table->cellWidget(target, column));
            if (combo && other) {
                const int index = combo->currentIndex();
                combo->setCurrentIndex(other->currentIndex());
                other->setCurrentIndex(index);
            }
        }
        table->setCurrentCell(target, table->currentColumn());
    };
    QObject::connect(up, &QPushButton::clicked, table, [move] { move(-1); });
    QObject::connect(down, &QPushButton::clicked, table, [move] { move(1); });
    return layout;
}

QTableWidgetItem *checkItem(bool checked)
{
    auto *item = new QTableWidgetItem;
    item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

QString cellText(const QTableWidget *table, int row, int column)
{
    const auto *item = table->item(row, column);
    return item ? item->text() : QString();
}

bool cellChecked(const QTableWidget *table, int row, int column)
{
    const auto *item = table->item(row, column);
    return item && item->checkState() == Qt::Checked;
}

QTableWidget *makeTable(const QStringList &headers, QWidget *parent)
{
    auto *table = new QTableWidget(0, int(headers.size()), parent);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setMinimumHeight(140);
    return table;
}
}

QRegularExpression outputPattern(const QString &text)
{
    if (text.size() >= 2 && text.startsWith(QLatin1Char('/')) && text.endsWith(QLatin1Char('/')))
        return QRegularExpression(text.mid(1, text.size() - 2));
    return QRegularExpression(QRegularExpression::escape(text),
                              QRegularExpression::CaseInsensitiveOption);
}

QList<QPair<QString, QString>> triggerActions()
{
    return {{QStringLiteral("send"), QStringLiteral("Type the text")},
            {QStringLiteral("notify"), QStringLiteral("Notify")},
            {QStringLiteral("beep"), QStringLiteral("Beep")},
            {QStringLiteral("mark"), QStringLiteral("Mark the tab")}};
}

AutomationOptions automationFromYaml(const YAML::Node &node)
{
    AutomationOptions options;
    if (!node || !node.IsMap())
        return options;
    options.mode = text(node, "mode");
    if (options.mode != QStringLiteral("own") && options.mode != QStringLiteral("none"))
        options.mode.clear();
    if (const auto steps = node["login_script"]; steps && steps.IsSequence()) {
        for (const auto &item : steps) {
            LoginStep step;
            step.expect = text(item, "expect");
            step.send = text(item, "send");
            step.enter = flag(item, "enter", true);
            step.timeout = number(item, "timeout", 0, 0, 3600);
            if (!step.expect.isEmpty() || !step.send.isEmpty())
                options.loginScript.append(step);
        }
    }
    options.triggers = triggersFromYaml(node["triggers"]);
    options.afterLoginScript = text(node, "after_login_script");
    return options;
}

void automationToYaml(const AutomationOptions &options, YAML::Node &node)
{
    if (!options.mode.isEmpty())
        node["mode"] = options.mode.toStdString();
    if (!options.loginScript.isEmpty()) {
        YAML::Node steps(YAML::NodeType::Sequence);
        for (const auto &step : options.loginScript) {
            YAML::Node item(YAML::NodeType::Map);
            if (!step.expect.isEmpty())
                item["expect"] = step.expect.toStdString();
            item["send"] = step.send.toStdString();
            if (!step.enter)
                item["enter"] = false;
            if (step.timeout > 0)
                item["timeout"] = step.timeout;
            steps.push_back(item);
        }
        node["login_script"] = steps;
    }
    if (!options.triggers.isEmpty())
        node["triggers"] = triggersToYaml(options.triggers);
    if (!options.afterLoginScript.isEmpty())
        node["after_login_script"] = options.afterLoginScript.toStdString();
}

AutomationOptions automationFromText(const QString &text)
{
    if (text.isEmpty())
        return {};
    try {
        return automationFromYaml(YAML::Load(text.toStdString()));
    } catch (const YAML::Exception &) {
        return {};
    }
}

QString automationToText(const AutomationOptions &options)
{
    if (options.isEmpty())
        return {};
    YAML::Node node(YAML::NodeType::Map);
    automationToYaml(options, node);
    YAML::Emitter emitter;
    emitter << YAML::Flow << node;
    return QString::fromUtf8(emitter.c_str());
}

AutomationPreferences loadAutomationPreferences()
{
    const auto node = loadPreferencesSection("automation");
    AutomationPreferences value;
    value.loginScripts = flag(node, "login_scripts", value.loginScripts);
    value.triggers = flag(node, "triggers", value.triggers);
    value.stepTimeout = number(node, "step_timeout", value.stepTimeout, 1, 3600);
    value.stopOnTimeout = flag(node, "stop_on_timeout", value.stopOnTimeout);
    value.triggersMaySendPassword = flag(node, "triggers_may_send_password", value.triggersMaySendPassword);
    value.globalTriggers = triggersFromYaml(node["global_triggers"]);
    return value;
}

void saveAutomationPreferences(const AutomationPreferences &value)
{
    YAML::Node node(YAML::NodeType::Map);
    node["login_scripts"] = value.loginScripts;
    node["triggers"] = value.triggers;
    node["step_timeout"] = value.stepTimeout;
    node["stop_on_timeout"] = value.stopOnTimeout;
    node["triggers_may_send_password"] = value.triggersMaySendPassword;
    node["global_triggers"] = triggersToYaml(value.globalTriggers);
    savePreferencesSection("automation", node);
}

AutomationRunner::AutomationRunner(Sender send, QObject *parent)
    : QObject(parent), m_send(std::move(send)), m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &AutomationRunner::stepTimedOut);
}

void AutomationRunner::configure(const QList<LoginStep> &script, const QList<OutputTrigger> &triggers,
                                 const AutomationPreferences &preferences,
                                 const QHash<QString, QString> &values)
{
    stop();
    m_preferences = preferences;
    m_values = values;
    m_script = preferences.loginScripts ? script : QList<LoginStep>();
    m_triggers.clear();
    if (preferences.triggers) {
        for (const auto &rule : triggers) {
            if (rule.enabled && !rule.pattern.isEmpty())
                m_triggers.append({rule, outputPattern(rule.pattern), false});
        }
    }
}

void AutomationRunner::start()
{
    stop();
    m_triggerBuffer.clear();
    for (auto &trigger : m_triggers)
        trigger.fired = false;
    if (m_script.isEmpty())
        return;
    m_step = 0;
    m_scriptBuffer.clear();
    runStep();
}

void AutomationRunner::stop(const QString &why)
{
    m_timer->stop();
    if (m_step >= 0 && !why.isEmpty())
        emit status(why);
    m_step = -1;
}

// Sends the steps that wait for nothing, then waits for the next text.
void AutomationRunner::runStep()
{
    while (m_step >= 0 && m_step < m_script.size()) {
        const auto &step = m_script[m_step];
        if (!step.expect.isEmpty()) {
            m_expect = outputPattern(step.expect);
            emit status(QStringLiteral("Login script: step %1 of %2, waiting for \"%3\"…")
                            .arg(m_step + 1).arg(m_script.size()).arg(step.expect));
            m_timer->start((step.timeout > 0 ? step.timeout : m_preferences.stepTimeout) * 1000);
            // The text may already have arrived.
            if (const auto match = m_expect.match(m_scriptBuffer); match.hasMatch()) {
                m_scriptBuffer.remove(0, match.capturedEnd());
                m_timer->stop();
            } else {
                return;
            }
        }
        m_send(fill(step.send, true) + (step.enter ? QByteArray("\r") : QByteArray()));
        ++m_step;
    }
    if (m_step >= m_script.size()) {
        m_step = -1;
        emit status(QStringLiteral("Login script finished."));
        emit scriptFinished();
    }
}

void AutomationRunner::stepTimedOut()
{
    if (m_step < 0)
        return;
    const auto &step = m_script[m_step];
    const int seconds = step.timeout > 0 ? step.timeout : m_preferences.stepTimeout;
    if (m_preferences.stopOnTimeout) {
        stop(QStringLiteral("Login script stopped: \"%1\" did not appear within %2 s.")
                 .arg(step.expect).arg(seconds));
        return;
    }
    emit status(QStringLiteral("Login script: \"%1\" did not appear within %2 s; going on.")
                    .arg(step.expect).arg(seconds));
    m_send(fill(step.send, true) + (step.enter ? QByteArray("\r") : QByteArray()));
    ++m_step;
    runStep();
}

QByteArray AutomationRunner::fill(const QString &text, bool allowPassword) const
{
    QString result = text;
    for (auto it = m_values.cbegin(); it != m_values.cend(); ++it) {
        if (it.key() == QStringLiteral("password") && !allowPassword)
            continue;
        result.replace(QStringLiteral("{%1}").arg(it.key()), it.value());
    }
    return result.toUtf8();
}

void AutomationRunner::feed(const QByteArray &output)
{
    if (m_step < 0 && m_triggers.isEmpty())
        return;
    const auto text = plainText(output);
    if (m_step >= 0) {
        m_scriptBuffer += text;
        keepTail(m_scriptBuffer);
        if (const auto match = m_expect.match(m_scriptBuffer); match.hasMatch()) {
            m_scriptBuffer.remove(0, match.capturedEnd());
            m_timer->stop();
            const auto &step = m_script[m_step];
            m_send(fill(step.send, true) + (step.enter ? QByteArray("\r") : QByteArray()));
            ++m_step;
            runStep();
        }
    }
    if (m_triggers.isEmpty())
        return;
    m_triggerBuffer += text;
    keepTail(m_triggerBuffer);
    qsizetype consumed = 0;
    for (auto &trigger : m_triggers) {
        if (trigger.rule.once && trigger.fired)
            continue;
        const auto match = trigger.expression.match(m_triggerBuffer);
        if (!match.hasMatch())
            continue;
        trigger.fired = true;
        consumed = qMax(consumed, match.capturedEnd());
        const auto &rule = trigger.rule;
        if (rule.action == QStringLiteral("send"))
            m_send(fill(rule.text, m_preferences.triggersMaySendPassword) + "\r");
        else if (rule.action == QStringLiteral("beep"))
            emit beep();
        else if (rule.action == QStringLiteral("mark"))
            emit markTab();
        else
            emit notify(rule.text.isEmpty() ? QStringLiteral("Seen: %1").arg(match.captured(0).trimmed())
                                            : rule.text);
    }
    // Matched text does not fire the same trigger again.
    if (consumed > 0)
        m_triggerBuffer.remove(0, consumed);
}

TriggerTable::TriggerTable(const QList<OutputTrigger> &value, QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_table = makeTable({QStringLiteral("On"), QStringLiteral("When the output contains"),
                         QStringLiteral("Then"), QStringLiteral("Text"), QStringLiteral("Once")},
                        this);
    m_table->setToolTip(QStringLiteral("Plain text matches in any case; write /…/ for a regular "
                                       "expression. \"Type the text\" sends the text and Enter; "
                                       "{user} and {host} are filled in."));
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    for (const auto &trigger : value)
        addRow(trigger);
    layout->addWidget(m_table);
    layout->addLayout(rowButtons(m_table, [this] { addRow({}); }, this));
    connect(m_table, &QTableWidget::itemChanged, this, &TriggerTable::changed);
    connect(m_table->model(), &QAbstractItemModel::rowsRemoved, this, &TriggerTable::changed);
}

void TriggerTable::addRow(const OutputTrigger &trigger)
{
    const QSignalBlocker blocker(m_table);
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, checkItem(trigger.enabled));
    m_table->setItem(row, 1, new QTableWidgetItem(trigger.pattern));
    auto *action = new QComboBox(m_table);
    for (const auto &[choice, label] : triggerActions())
        action->addItem(label, choice);
    action->setCurrentIndex(qMax(0, action->findData(trigger.action)));
    connect(action, &QComboBox::currentIndexChanged, this, &TriggerTable::changed);
    m_table->setCellWidget(row, 2, action);
    m_table->setItem(row, 3, new QTableWidgetItem(trigger.text));
    m_table->setItem(row, 4, checkItem(trigger.once));
}

QList<OutputTrigger> TriggerTable::value() const
{
    QList<OutputTrigger> triggers;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        OutputTrigger trigger;
        trigger.enabled = cellChecked(m_table, row, 0);
        trigger.pattern = cellText(m_table, row, 1).trimmed();
        if (auto *action = qobject_cast<QComboBox *>(m_table->cellWidget(row, 2)))
            trigger.action = action->currentData().toString();
        trigger.text = cellText(m_table, row, 3);
        trigger.once = cellChecked(m_table, row, 4);
        if (!trigger.pattern.isEmpty())
            triggers.append(trigger);
    }
    return triggers;
}

AutomationEditor::AutomationEditor(const AutomationOptions &value, const QString &inheritLabel,
                                   const QStringList &scripts, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_mode = new QComboBox(this);
    m_mode->addItem(inheritLabel, QString());
    m_mode->addItem(QStringLiteral("Its own, below"), QStringLiteral("own"));
    m_mode->addItem(QStringLiteral("None"), QStringLiteral("none"));
    m_mode->setCurrentIndex(qMax(0, m_mode->findData(value.mode)));
    form->addRow(QStringLiteral("Login script and triggers:"), m_mode);
    layout->addLayout(form);

    m_own = new QWidget(this);
    auto *own = new QVBoxLayout(m_own);
    own->setContentsMargins(0, 0, 0, 0);
    own->addWidget(new QLabel(QStringLiteral("Login script: after connecting, wait for each text, "
                                             "then type the answer."), m_own));
    m_steps = makeTable({QStringLiteral("Wait for"), QStringLiteral("Then type"),
                         QStringLiteral("Enter"), QStringLiteral("Timeout (s)")},
                        m_own);
    m_steps->setToolTip(QStringLiteral("An empty \"Wait for\" types at once. Write /…/ for a regular "
                                       "expression. {password}, {user} and {host} are filled in "
                                       "from the session. Timeout 0 uses the Preferences' default."));
    m_steps->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_steps->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (const auto &step : value.loginScript)
        addStep(step);
    own->addWidget(m_steps);
    own->addLayout(rowButtons(m_steps, [this] { addStep({}); }, m_own));
    own->addWidget(new QLabel(QStringLiteral("Triggers: while the tab is open, when the output "
                                             "shows a text, do something."), m_own));
    m_triggers = new TriggerTable(value.triggers, m_own);
    own->addWidget(m_triggers);
    auto *afterForm = new QFormLayout;
    m_afterLogin = new QComboBox(m_own);
    m_afterLogin->addItem(QStringLiteral("Nothing"), QString());
    for (const auto &script : scripts)
        m_afterLogin->addItem(script, script);
    if (!value.afterLoginScript.isEmpty() && m_afterLogin->findData(value.afterLoginScript) < 0)
        m_afterLogin->addItem(QStringLiteral("%1 (missing)").arg(value.afterLoginScript),
                              value.afterLoginScript);
    m_afterLogin->setCurrentIndex(qMax(0, m_afterLogin->findData(value.afterLoginScript)));
    m_afterLogin->setToolTip(QStringLiteral("A saved script (Tools → Scripts) run once logged on, "
                                            "after the login script."));
    afterForm->addRow(QStringLiteral("After logging on, run script:"), m_afterLogin);
    own->addLayout(afterForm);
    layout->addWidget(m_own);
    connect(m_mode, &QComboBox::currentIndexChanged, this, &AutomationEditor::updateEnabled);
    updateEnabled();
}

void AutomationEditor::addStep(const LoginStep &step)
{
    const int row = m_steps->rowCount();
    m_steps->insertRow(row);
    m_steps->setItem(row, 0, new QTableWidgetItem(step.expect));
    m_steps->setItem(row, 1, new QTableWidgetItem(step.send));
    m_steps->setItem(row, 2, checkItem(step.enter));
    m_steps->setItem(row, 3, new QTableWidgetItem(QString::number(step.timeout)));
}

void AutomationEditor::updateEnabled()
{
    m_own->setEnabled(m_mode->currentData().toString() == QStringLiteral("own"));
}

AutomationOptions AutomationEditor::value() const
{
    AutomationOptions options;
    options.mode = m_mode->currentData().toString();
    // The rows are kept even when not used now, so switching back keeps them.
    for (int row = 0; row < m_steps->rowCount(); ++row) {
        LoginStep step;
        step.expect = cellText(m_steps, row, 0);
        step.send = cellText(m_steps, row, 1);
        step.enter = cellChecked(m_steps, row, 2);
        step.timeout = qBound(0, cellText(m_steps, row, 3).toInt(), 3600);
        if (!step.expect.isEmpty() || !step.send.isEmpty())
            options.loginScript.append(step);
    }
    options.triggers = m_triggers->value();
    options.afterLoginScript = m_afterLogin->currentData().toString();
    return options;
}
