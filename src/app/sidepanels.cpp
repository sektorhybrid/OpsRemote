// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sidepanels.h"
#include "scripts.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int IndexRole = Qt::UserRole + 1;

QString groupOf(const QString &group)
{
    return group.isEmpty() ? QStringLiteral("General") : group;
}

// The folder for a group, made when it is not there yet.
QTreeWidgetItem *groupItem(QTreeWidget *tree, const QString &group)
{
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        if (tree->topLevelItem(index)->text(0) == group)
            return tree->topLevelItem(index);
    }
    auto *item = new QTreeWidgetItem(tree, {group});
    item->setFlags(Qt::ItemIsEnabled);
    auto font = item->font(0);
    font.setBold(true);
    item->setFont(0, font);
    item->setExpanded(true);
    return item;
}

QTreeWidget *makeTree(QWidget *parent)
{
    auto *tree = new QTreeWidget(parent);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(true);
    tree->setIndentation(14);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    return tree;
}

QLineEdit *makeSearch(const QString &placeholder, QWidget *parent)
{
    auto *search = new QLineEdit(parent);
    search->setPlaceholderText(placeholder);
    search->setClearButtonEnabled(true);
    return search;
}

bool matches(const QString &words, const QStringList &texts)
{
    for (const auto &word : words.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        bool found = false;
        for (const auto &text : texts)
            found = found || text.contains(word, Qt::CaseInsensitive);
        if (!found)
            return false;
    }
    return true;
}
}

CommandsPanel::CommandsPanel(std::function<QStringList()> scripts, QWidget *parent)
    : QWidget(parent), m_scripts(std::move(scripts))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    m_search = makeSearch(QStringLiteral("Search commands"), this);
    layout->addWidget(m_search);
    m_tree = makeTree(this);
    m_tree->setToolTip(QStringLiteral("Double-click: send to the current tab. Right-click: more."));
    layout->addWidget(m_tree, 1);
    auto *buttons = new QGridLayout;
    m_send = new QPushButton(QStringLiteral("Send"), this);
    auto *add = new QPushButton(QStringLiteral("Add"), this);
    m_edit = new QPushButton(QStringLiteral("Edit"), this);
    m_remove = new QPushButton(QStringLiteral("Remove"), this);
    m_up = new QPushButton(QStringLiteral("Up"), this);
    m_down = new QPushButton(QStringLiteral("Down"), this);
    m_send->setToolTip(QStringLiteral("Send to the current tab (Shift: to the multi-exec terminals)"));
    buttons->addWidget(m_send, 0, 0);
    buttons->addWidget(add, 0, 1);
    buttons->addWidget(m_edit, 0, 2);
    buttons->addWidget(m_remove, 1, 0);
    buttons->addWidget(m_up, 1, 1);
    buttons->addWidget(m_down, 1, 2);
    layout->addLayout(buttons);

    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuild(selectedIndex()); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &CommandsPanel::updateButtons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        const int index = item ? item->data(0, IndexRole).toInt() - 1 : -1;
        if (index >= 0)
            emit sendRequested(m_snippets[index], false, false);
    });
    connect(m_send, &QPushButton::clicked, this, [this] {
        if (const int index = selectedIndex(); index >= 0)
            emit sendRequested(m_snippets[index], QApplication::keyboardModifiers() & Qt::ShiftModifier, false);
    });
    connect(add, &QPushButton::clicked, this, &CommandsPanel::add);
    connect(m_edit, &QPushButton::clicked, this, &CommandsPanel::edit);
    connect(m_remove, &QPushButton::clicked, this, &CommandsPanel::remove);
    connect(m_up, &QPushButton::clicked, this, [this] { move(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { move(1); });
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &at) {
        const int index = selectedIndex();
        QMenu menu(this);
        if (index >= 0) {
            const auto snippet = m_snippets[index];
            menu.addAction(QStringLiteral("Send"), this, [this, snippet] { emit sendRequested(snippet, false, false); });
            menu.addAction(QStringLiteral("Send Without Enter"), this,
                           [this, snippet] { emit sendRequested(snippet, false, true); });
            menu.addAction(QStringLiteral("Send to Multi-exec Terminals"), this,
                           [this, snippet] { emit sendRequested(snippet, true, false); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Edit…"), this, &CommandsPanel::edit);
            menu.addAction(QStringLiteral("Remove"), this, &CommandsPanel::remove);
            menu.addSeparator();
        }
        menu.addAction(QStringLiteral("Add…"), this, &CommandsPanel::add);
        menu.exec(m_tree->viewport()->mapToGlobal(at));
    });
    updateButtons();
}

void CommandsPanel::setSnippets(const QList<Snippet> &snippets)
{
    if (snippets == m_snippets)
        return;
    const int selected = selectedIndex();
    m_snippets = snippets;
    rebuild(selected);
}

void CommandsPanel::rebuild(int select)
{
    m_tree->clear();
    const auto words = m_search->text();
    QTreeWidgetItem *selected = nullptr;
    for (int index = 0; index < m_snippets.size(); ++index) {
        const auto &snippet = m_snippets[index];
        if (!matches(words, {snippet.name, snippet.text, snippet.group}))
            continue;
        const auto script = scriptFromMarker(snippet.text);
        auto *item = new QTreeWidgetItem(groupItem(m_tree, groupOf(snippet.group)), {snippet.name});
        item->setData(0, IndexRole, index + 1);
        item->setToolTip(0, script.isEmpty() ? snippet.text.left(400) : QStringLiteral("Runs the script %1").arg(script));
        if (index == select)
            selected = item;
    }
    m_tree->expandAll();
    if (m_snippets.isEmpty()) {
        auto *hint = new QTreeWidgetItem(m_tree, {QStringLiteral("No commands yet: Add makes one")});
        hint->setFlags(Qt::ItemIsEnabled);
        hint->setForeground(0, palette().brush(QPalette::PlaceholderText));
    }
    if (selected)
        m_tree->setCurrentItem(selected);
    updateButtons();
}

int CommandsPanel::selectedIndex() const
{
    const auto *item = m_tree->currentItem();
    return item ? item->data(0, IndexRole).toInt() - 1 : -1;
}

void CommandsPanel::updateButtons()
{
    const int index = selectedIndex();
    for (auto *button : {m_send, m_edit, m_remove, m_up, m_down})
        button->setEnabled(index >= 0);
}

bool CommandsPanel::editSnippet(Snippet *snippet, const QString &title)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(460);
    auto *form = new QFormLayout(&dialog);
    auto *name = new QLineEdit(snippet->name, &dialog);
    const auto script = scriptFromMarker(snippet->text);
    auto *text = new QPlainTextEdit(script.isEmpty() ? snippet->text : QString(), &dialog);
    text->setPlaceholderText(QStringLiteral("The command; several lines are sent one after another"));
    text->setMinimumHeight(90);
    auto *scripts = new QComboBox(&dialog);
    scripts->addItem(QStringLiteral("Send the text above"), QString());
    for (const auto &each : m_scripts ? m_scripts() : QStringList())
        scripts->addItem(QStringLiteral("Run the script %1").arg(each), each);
    if (!script.isEmpty() && scripts->findData(script) < 0)
        scripts->addItem(QStringLiteral("Run the script %1 (missing)").arg(script), script);
    scripts->setCurrentIndex(qMax(0, scripts->findData(script)));
    auto *enter = new QCheckBox(QStringLiteral("Press Enter after it"), &dialog);
    enter->setChecked(snippet->pressEnter);
    auto *group = new QComboBox(&dialog);
    group->setEditable(true);
    QStringList groups;
    for (const auto &each : std::as_const(m_snippets)) {
        if (!each.group.isEmpty() && !groups.contains(each.group))
            groups << each.group;
    }
    group->addItems(groups);
    group->setEditText(snippet->group);
    group->lineEdit()->setPlaceholderText(QStringLiteral("General"));
    form->addRow(QStringLiteral("Name:"), name);
    form->addRow(QStringLiteral("Text:"), text);
    form->addRow(QStringLiteral("Or:"), scripts);
    form->addRow(QString(), enter);
    form->addRow(QStringLiteral("Group:"), group);
    const auto updateText = [text, enter, scripts] {
        const bool own = scripts->currentData().toString().isEmpty();
        text->setEnabled(own);
        enter->setEnabled(own);
    };
    connect(scripts, &QComboBox::currentIndexChanged, &dialog, updateText);
    updateText();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (name->text().trimmed().isEmpty())
            QMessageBox::warning(&dialog, dialog.windowTitle(), QStringLiteral("Give the command a name."));
        else
            dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    name->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return false;
    snippet->name = name->text().trimmed();
    const auto chosen = scripts->currentData().toString();
    snippet->text = chosen.isEmpty() ? text->toPlainText() : scriptMarker(chosen);
    snippet->pressEnter = enter->isChecked();
    snippet->group = group->currentText().trimmed();
    return true;
}

void CommandsPanel::add()
{
    Snippet snippet;
    if (const int index = selectedIndex(); index >= 0)
        snippet.group = m_snippets[index].group;
    else if (auto *item = m_tree->currentItem(); item && !item->parent() && item->text(0) != QStringLiteral("General"))
        snippet.group = item->text(0);
    if (!editSnippet(&snippet, QStringLiteral("New Command")))
        return;
    m_snippets.append(snippet);
    rebuild(int(m_snippets.size()) - 1);
    emit snippetsEdited(m_snippets);
}

void CommandsPanel::edit()
{
    const int index = selectedIndex();
    if (index < 0)
        return;
    auto snippet = m_snippets[index];
    if (!editSnippet(&snippet, QStringLiteral("Edit Command")))
        return;
    m_snippets[index] = snippet;
    rebuild(index);
    emit snippetsEdited(m_snippets);
}

void CommandsPanel::remove()
{
    const int index = selectedIndex();
    if (index < 0)
        return;
    if (QMessageBox::question(this, QStringLiteral("Remove Command"),
                              QStringLiteral("Remove the command %1?").arg(m_snippets[index].name))
        != QMessageBox::Yes)
        return;
    m_snippets.removeAt(index);
    rebuild(qMin(index, int(m_snippets.size()) - 1));
    emit snippetsEdited(m_snippets);
}

// Up or down within its group: swaps with the group's previous or next one.
void CommandsPanel::move(int by)
{
    const int index = selectedIndex();
    if (index < 0)
        return;
    const auto group = groupOf(m_snippets[index].group);
    for (int other = index + by; other >= 0 && other < m_snippets.size(); other += by) {
        if (groupOf(m_snippets[other].group) != group)
            continue;
        m_snippets.swapItemsAt(index, other);
        rebuild(other);
        emit snippetsEdited(m_snippets);
        return;
    }
}

ScriptsPanel::ScriptsPanel(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    m_search = makeSearch(QStringLiteral("Search scripts"), this);
    layout->addWidget(m_search);
    m_tree = makeTree(this);
    m_tree->setToolTip(QStringLiteral("Double-click: run in the current tab."));
    layout->addWidget(m_tree, 1);
    auto *buttons = new QGridLayout;
    m_run = new QPushButton(QStringLiteral("Run"), this);
    m_runOnHosts = new QPushButton(QStringLiteral("On Hosts…"), this);
    m_runOnHosts->setToolTip(QStringLiteral("Run on the SSH sessions of a folder (Run on Hosts)"));
    auto *add = new QPushButton(QStringLiteral("New"), this);
    m_edit = new QPushButton(QStringLiteral("Edit"), this);
    m_delete = new QPushButton(QStringLiteral("Delete"), this);
    buttons->addWidget(m_run, 0, 0);
    buttons->addWidget(m_runOnHosts, 0, 1);
    buttons->addWidget(add, 1, 0);
    buttons->addWidget(m_edit, 1, 1);
    buttons->addWidget(m_delete, 1, 2);
    layout->addLayout(buttons);

    connect(m_search, &QLineEdit::textChanged, this, &ScriptsPanel::filter);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &ScriptsPanel::updateButtons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (const auto name = selectedName(); !name.isEmpty())
            emit runRequested(name);
    });
    connect(m_run, &QPushButton::clicked, this, [this] { emit runRequested(selectedName()); });
    connect(m_runOnHosts, &QPushButton::clicked, this, [this] { emit runOnHostsRequested(selectedName()); });
    connect(add, &QPushButton::clicked, this, &ScriptsPanel::newRequested);
    connect(m_edit, &QPushButton::clicked, this, [this] { emit editRequested(selectedName()); });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        const auto name = selectedName();
        if (name.isEmpty()
            || QMessageBox::question(this, QStringLiteral("Delete Script"),
                                     QStringLiteral("Delete the script %1?").arg(name)) != QMessageBox::Yes)
            return;
        deleteSavedScript(name);
        refresh();
        emit scriptsChanged();
    });
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &at) {
        const auto name = selectedName();
        QMenu menu(this);
        if (!name.isEmpty()) {
            menu.addAction(QStringLiteral("Run in the Current Tab"), this, [this, name] { emit runRequested(name); });
            menu.addAction(QStringLiteral("Run on Hosts…"), this, [this, name] { emit runOnHostsRequested(name); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Edit…"), this, [this, name] { emit editRequested(name); });
            menu.addSeparator();
        }
        menu.addAction(QStringLiteral("New…"), this, &ScriptsPanel::newRequested);
        menu.exec(m_tree->viewport()->mapToGlobal(at));
    });
    refresh();
}

void ScriptsPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Edited in Tools → Scripts, or in the folder.
    refresh();
}

void ScriptsPanel::refresh()
{
    const auto selected = selectedName();
    m_tree->clear();
    QTreeWidgetItem *current = nullptr;
    const auto scripts = loadSavedScripts();
    for (const auto &script : scripts) {
        auto *item = new QTreeWidgetItem(groupItem(m_tree, groupOf(script.group)), {script.name});
        item->setData(0, IndexRole, script.name);
        item->setData(0, Qt::UserRole + 2, QStringList{script.name, script.description, script.group, script.body});
        item->setToolTip(0, script.description.isEmpty() ? script.interpreter
                                                         : QStringLiteral("%1 (%2)").arg(script.description, script.interpreter));
        if (script.name == selected)
            current = item;
    }
    m_tree->expandAll();
    if (scripts.isEmpty()) {
        auto *hint = new QTreeWidgetItem(m_tree, {QStringLiteral("No scripts yet: New makes one")});
        hint->setFlags(Qt::ItemIsEnabled);
        hint->setForeground(0, palette().brush(QPalette::PlaceholderText));
    }
    if (current)
        m_tree->setCurrentItem(current);
    filter();
    updateButtons();
}

void ScriptsPanel::filter()
{
    const auto words = m_search->text();
    for (int group = 0; group < m_tree->topLevelItemCount(); ++group) {
        auto *folder = m_tree->topLevelItem(group);
        bool any = folder->childCount() == 0;
        for (int index = 0; index < folder->childCount(); ++index) {
            auto *item = folder->child(index);
            const bool shown = matches(words, item->data(0, Qt::UserRole + 2).toStringList());
            item->setHidden(!shown);
            any = any || shown;
        }
        folder->setHidden(!any);
    }
}

QString ScriptsPanel::selectedName() const
{
    const auto *item = m_tree->currentItem();
    return item && item->parent() ? item->data(0, IndexRole).toString() : QString();
}

void ScriptsPanel::updateButtons()
{
    const bool selected = !selectedName().isEmpty();
    for (auto *button : {m_run, m_runOnHosts, m_edit, m_delete})
        button->setEnabled(selected);
}
