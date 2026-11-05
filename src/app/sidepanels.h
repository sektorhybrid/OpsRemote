// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "preferences.h"

#include <QWidget>

#include <functional>

class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// The main window's Commands tab: the snippets in their groups, sent to the
// current tab (double-click or Send) or to the multi-exec terminals.
class CommandsPanel final : public QWidget
{
    Q_OBJECT

public:
    // scripts: the saved scripts' names, for "run a script" commands.
    CommandsPanel(std::function<QStringList()> scripts, QWidget *parent = nullptr);

    void setSnippets(const QList<Snippet> &snippets);

signals:
    void sendRequested(const Snippet &snippet, bool toMultiExec, bool withoutEnter);
    // The list was edited; the window saves it.
    void snippetsEdited(const QList<Snippet> &snippets);

private:
    void rebuild(int select = -1);
    int selectedIndex() const;
    void updateButtons();
    void add();
    void edit();
    void remove();
    void move(int by);
    // The edit window; false when cancelled.
    bool editSnippet(Snippet *snippet, const QString &title);

    std::function<QStringList()> m_scripts;
    QList<Snippet> m_snippets;
    QLineEdit *m_search = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_send = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_down = nullptr;
};

// The main window's Scripts tab: the saved scripts in their groups.
class ScriptsPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit ScriptsPanel(QWidget *parent = nullptr);
    // Reads settings/scripts/ again.
    void refresh();

signals:
    void runRequested(const QString &name);
    void runOnHostsRequested(const QString &name);
    // Opens the script editor (Tools → Scripts) on a script, or a new one.
    void editRequested(const QString &name);
    void newRequested();
    void scriptsChanged();

protected:
    void showEvent(QShowEvent *event) override;

private:
    QString selectedName() const;
    void updateButtons();
    void filter();

    QLineEdit *m_search = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_run = nullptr;
    QPushButton *m_runOnHosts = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_delete = nullptr;
};
