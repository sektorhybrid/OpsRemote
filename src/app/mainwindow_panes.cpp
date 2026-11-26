// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// MainWindow: split panes: placing, moving and closing them, multi-exec, the pane bars, messages and transfers per tab.

#include "mainwindow_p.h"

using namespace mainwindow_p;

// ---- Split tabs ----

QList<TerminalPage *> MainWindow::terminalPagesIn(QWidget *tab) const
{
    if (auto *split = qobject_cast<SplitTab *>(tab))
        return split->panes();
    if (auto *terminal = qobject_cast<TerminalPage *>(tab))
        return {terminal};
    return {};
}

QList<TerminalPage *> MainWindow::allTerminalPages() const
{
    QList<TerminalPage *> pages;
    for (int index = 0; index < m_sessionTabs->count(); ++index)
        pages += terminalPagesIn(m_sessionTabs->widget(index));
    return pages;
}

// The terminal a tab's commands act on: the tab's, or its active pane.
TerminalPage *MainWindow::activeTerminal(QWidget *tab) const
{
    if (auto *split = qobject_cast<SplitTab *>(tab))
        return split->activePane();
    return qobject_cast<TerminalPage *>(tab);
}

// The tab showing this page, as itself or as a pane; -1 if none.
int MainWindow::tabIndexOf(QWidget *page) const
{
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *tab = m_sessionTabs->widget(index);
        if (tab == page || (qobject_cast<SplitTab *>(tab) && tab->isAncestorOf(page)))
            return index;
    }
    return -1;
}

// Split Right / Split Down: asks what to open, then opens it the usual way;
// addSessionTab() puts the new terminal next to the active pane.
// what: "same" (the active pane's session, or a local terminal for a local
// one), "local", or "choose" (the New Pane chooser).
void MainWindow::splitActivePane(Qt::Orientation orientation, const QString &what)
{
    auto *beside = activeTerminal(m_sessionTabs->currentWidget());
    if (!beside) {
        tabMessage(m_sessionTabs->currentWidget(), QStringLiteral("Only terminal tabs can be split"));
        return;
    }
    const QString baseId = beside->property("sessionId").toString().section(QLatin1Char('#'), 0, 0);
    if (what == QStringLiteral("local")) {
        openInSplit(orientation, QStringLiteral("local"));
    } else if (what == QStringLiteral("same") && (baseId.isEmpty() || m_sessions.contains(baseId))) {
        // A local terminal (no session) splits into another local terminal.
        openInSplit(orientation, baseId.isEmpty() ? QStringLiteral("local") : baseId);
    } else if (const auto choice = choosePaneSession(baseId)) {
        openInSplit(orientation, *choice);
    }
}

// Split Right ▸ / Split Down ▸ for the right-click menus.
void MainWindow::addSplitMenus(QMenu *menu, const std::function<bool()> &makeActive)
{
    for (const auto &[label, orientation] : {std::pair{QStringLiteral("Split Right"), Qt::Horizontal},
                                             std::pair{QStringLiteral("Split Down"), Qt::Vertical}}) {
        auto *sub = menu->addMenu(label);
        const auto add = [this, sub, makeActive, orientation](const QString &text, const QString &what) {
            sub->addAction(text, this, [this, makeActive, orientation, what] {
                if (makeActive())
                    splitActivePane(orientation, what);
            });
        };
        add(QStringLiteral("Same Session"), QStringLiteral("same"));
        if (LocalTerminalSupported)
            add(QStringLiteral("Local Terminal"), QStringLiteral("local"));
        add(QStringLiteral("Other Session…"), QStringLiteral("choose"));
    }
}

// Opens "local" or a saved session's id next to the current tab's active
// terminal.
void MainWindow::openInSplit(Qt::Orientation orientation, const QString &choice)
{
    auto *beside = activeTerminal(m_sessionTabs->currentWidget());
    if (!beside)
        return;
    m_pendingSplit = {beside, orientation};
    if (choice == QStringLiteral("local")) {
        openLocalTerminal();
    } else if (choice == QStringLiteral("local-administrator")) {
        openLocalTerminalAsAdministrator();
    } else if (const auto session = m_sessions.constFind(choice); session != m_sessions.cend()) {
        // A copy under its own id, so the open tab of the session is not
        // simply brought forward.
        auto copy = *session;
        copy.id = session->id + QStringLiteral("#") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        openSession(copy);
    }
    // Nothing opened (a locked vault, a cancelled prompt): no split waits.
    m_pendingSplit = {};
}

// What a new pane shows: the same session (the default), a local terminal,
// or another saved terminal session. nullopt when cancelled.
std::optional<QString> MainWindow::choosePaneSession(const QString &currentId)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New Pane"));
    dialog.setMinimumSize(420, 380);
    auto *layout = new QVBoxLayout(&dialog);
    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(QStringLiteral("Search sessions"));
    filter->setClearButtonEnabled(true);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Open in the new pane:"), &dialog));
    layout->addWidget(filter);
    layout->addWidget(list, 1);
    const auto add = [list](const QString &text, const QString &value, const QIcon &icon) {
        auto *item = new QListWidgetItem(icon, text, list);
        item->setData(Qt::UserRole, value);
    };
    const QIcon terminalIcon = themedIcon(QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    const QIcon sessionIcon = themedIcon(QStringLiteral("network-server"), QStyle::SP_ComputerIcon);
    if (m_sessions.contains(currentId))
        add(QStringLiteral("Same session: %1").arg(m_sessions.value(currentId).name), currentId, sessionIcon);
    if (LocalTerminalSupported)
        add(QStringLiteral("Local terminal"), QStringLiteral("local"), terminalIcon);
    if (Platform::has(Platform::Feature::AdministratorTerminals))
        add(QStringLiteral("Local terminal as administrator"), QStringLiteral("local-administrator"),
            style()->standardIcon(QStyle::SP_VistaShield));
    // Saved terminal sessions, in the list's order, with their folder.
    for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
        if ((*it)->data(0, ItemKindRole).toString() != QStringLiteral("session"))
            continue;
        const auto session = m_sessions.value((*it)->data(0, SessionIdRole).toString());
        if (!isTerminalProtocol(session.protocol))
            continue;
        QStringList path;
        for (auto *parent = (*it)->parent(); parent; parent = parent->parent())
            path.prepend(parent->text(0));
        path.append(session.name);
        add(QStringLiteral("%1  (%2)").arg(path.join(QStringLiteral(" / ")), session.protocol),
            session.id, sessionIcon);
    }
    list->setCurrentRow(0);
    connect(filter, &QLineEdit::textChanged, list, [list](const QString &text) {
        const auto words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QListWidgetItem *first = nullptr;
        for (int row = 0; row < list->count(); ++row) {
            auto *item = list->item(row);
            bool match = true;
            for (const auto &word : words)
                match = match && item->text().contains(word, Qt::CaseInsensitive);
            item->setHidden(!match);
            if (match && !first)
                first = item;
        }
        list->setCurrentItem(first);
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
    // Up and Down in the search box move through the list.
    filter->installEventFilter(&dialog);
    struct Keys : QObject
    {
        QListWidget *list;
        bool eventFilter(QObject *, QEvent *event) override
        {
            if (event->type() != QEvent::KeyPress)
                return false;
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key != Qt::Key_Down && key != Qt::Key_Up)
                return false;
            int row = list->currentRow();
            do
                row += key == Qt::Key_Down ? 1 : -1;
            while (row >= 0 && row < list->count() && list->item(row)->isHidden());
            if (row >= 0 && row < list->count())
                list->setCurrentRow(row);
            return true;
        }
    };
    auto *keys = new Keys;
    keys->setParent(&dialog);
    keys->list = list;
    filter->removeEventFilter(&dialog);
    filter->installEventFilter(keys);
    filter->setFocus();
    if (dialog.exec() != QDialog::Accepted || !list->currentItem())
        return std::nullopt;
    return list->currentItem()->data(Qt::UserRole).toString();
}

// Puts a new terminal next to beside, turning beside's tab into a split tab.
void MainWindow::insertPane(TerminalPage *beside, TerminalPage *added, Qt::Orientation orientation)
{
    if (tabIndexOf(beside) < 0) {
        added->deleteLater();
        return;
    }
    placePane(added, beside, orientation, false);
    connect(added, &TerminalPage::closeRequested, this, [this, added] { closePane(added); });
}

// The split tab beside is in; a plain terminal tab becomes one in the same
// place first.
SplitTab *MainWindow::splitTabOf(TerminalPage *beside)
{
    const int index = tabIndexOf(beside);
    if (index < 0)
        return nullptr;
    if (auto *split = qobject_cast<SplitTab *>(m_sessionTabs->widget(index)))
        return split;
    const QString title = m_sessionTabs->tabText(index);
    const QString toolTip = m_sessionTabs->tabToolTip(index);
    const QIcon icon = m_sessionTabs->tabIcon(index);
    rememberTab(beside, index);
    m_sessionTabs->removeTab(index);
    auto *split = new SplitTab(beside, m_sessionTabs);
    connect(split, &SplitTab::activePaneChanged, this, [this, split] {
        if (m_sessionTabs->currentWidget() == split)
            showSftpPanelForCurrentTab();
    });
    m_sessionTabs->insertTab(index, split, icon, title);
    m_sessionTabs->setTabToolTip(index, toolTip);
    return split;
}

// Puts pane (in no tab) next to beside: right of it or below it, or with
// before left of it or above it.
void MainWindow::placePane(TerminalPage *pane, TerminalPage *beside, Qt::Orientation orientation, bool before)
{
    auto *split = splitTabOf(beside);
    if (!split)
        return;
    split->split(beside, pane, orientation, before);
    m_sessionTabs->setCurrentWidget(split);
    showSftpPanelForCurrentTab();
    panesChanged();
}

// A tab's title, tooltip and icon kept with its terminal, for when the
// terminal gets a tab of its own again (moved out of a split tab).
void MainWindow::rememberTab(QWidget *page, int index)
{
    page->setProperty("opsremoteTabTitle", m_sessionTabs->tabText(index));
    page->setProperty("opsremoteTabToolTip", m_sessionTabs->tabToolTip(index));
    page->setProperty("opsremoteTabIcon", m_sessionTabs->tabIcon(index));
}

// A split tab left with one pane becomes a plain terminal tab again.
void MainWindow::unsplitIfSingle(SplitTab *split)
{
    const int index = m_sessionTabs->indexOf(split);
    const auto remaining = split->panes();
    if (index < 0 || remaining.size() != 1)
        return;
    auto *last = remaining.constFirst();
    const QString title = m_sessionTabs->tabText(index);
    const QString toolTip = m_sessionTabs->tabToolTip(index);
    const QIcon icon = m_sessionTabs->tabIcon(index);
    const bool current = m_sessionTabs->currentIndex() == index;
    split->removePane(last);
    m_sessionTabs->removeTab(index);
    m_sessionTabs->insertTab(index, last, icon, title);
    m_sessionTabs->setTabToolTip(index, toolTip);
    if (current)
        m_sessionTabs->setCurrentIndex(index);
    split->deleteLater();
}

// Takes pane out of its tab (the tab goes when nothing is left in it); the
// caller puts it somewhere else.
void MainWindow::takePaneOut(TerminalPage *pane)
{
    const int index = tabIndexOf(pane);
    if (index < 0)
        return;
    auto *split = qobject_cast<SplitTab *>(m_sessionTabs->widget(index));
    if (!split) {
        rememberTab(pane, index);
        m_sessionTabs->removeTab(index);
        return;
    }
    split->removePane(pane);
    if (split->panes().isEmpty()) {
        m_sessionTabs->removeTab(index);
        split->deleteLater();
    } else {
        unsplitIfSingle(split);
    }
}

bool MainWindow::canDropOn(QWidget *dragged, TerminalPage *target) const
{
    if (!dragged || !target || dragged == target || dragged->isAncestorOf(target))
        return false;
    return qobject_cast<TerminalPage *>(dragged) || qobject_cast<SplitTab *>(dragged);
}

// The Move menu of a pane (its handle's, its right-click menu's): into a
// tab of its own, next to another tab's active pane, or picked up to be
// dropped where the pointer clicks.
void MainWindow::fillMoveMenu(QMenu *menu, TerminalPage *page)
{
    const QPointer<TerminalPage> pane(page);
    auto *newTab = menu->addAction(QIcon::fromTheme(QStringLiteral("tab-new")), QStringLiteral("Move to a New Tab"),
                                   this, [this, pane] {
                                       if (pane)
                                           moveDropped(pane, PaneDrag::Drop{true});
                                   });
    // A tab of its own already.
    newTab->setEnabled(m_sessionTabs->indexOf(page) < 0);
    auto *next = menu->addMenu(QStringLiteral("Move Next To"));
    const int own = tabIndexOf(page);
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *target = activeTerminal(m_sessionTabs->widget(index));
        if (index == own || !canDropOn(page, target))
            continue;
        next->addAction(m_sessionTabs->tabIcon(index), m_sessionTabs->tabText(index), this,
                        [this, pane, target = QPointer<TerminalPage>(target)] {
                            if (pane && target)
                                moveDropped(pane, PaneDrag::Drop{false, -1, target, Qt::Horizontal, false});
                        });
    }
    next->setEnabled(!next->isEmpty());
    menu->addSeparator();
    menu->addAction(QStringLiteral("Pick Up and Move"), this, [this, pane] {
        // After the menu has closed.
        QTimer::singleShot(0, this, [this, pane] {
            if (pane)
                m_paneDrag->pickUp(pane);
        });
    });
}

// Moves a dragged pane, or a whole tab (a terminal or a split tab), to
// where it was dropped: next to a pane, or into a tab of its own.
void MainWindow::moveDropped(QWidget *dragged, const PaneDrag::Drop &drop)
{
    if (!dragged)
        return;
    auto *pane = qobject_cast<TerminalPage *>(dragged);
    auto *group = qobject_cast<SplitTab *>(dragged);
    if (!pane && !group)
        return;
    if (drop.newTab) {
        if (pane) {
            // A plain tab dropped on the tab strip: it just moves along.
            if (m_sessionTabs->indexOf(pane) >= 0) {
                const int from = m_sessionTabs->indexOf(pane);
                m_sessionTabs->tabBar()->moveTab(from, qBound(0, drop.tabIndex < 0 ? m_sessionTabs->count() - 1
                                                                                 : drop.tabIndex - (drop.tabIndex > from ? 1 : 0),
                                                              m_sessionTabs->count() - 1));
                return;
            }
            takePaneOut(pane);
            const int at = drop.tabIndex < 0 || drop.tabIndex > m_sessionTabs->count() ? m_sessionTabs->count()
                                                                                       : drop.tabIndex;
            const QString title = pane->property("opsremoteTabTitle").toString();
            const int index = m_sessionTabs->insertTab(at, pane, pane->property("opsremoteTabIcon").value<QIcon>(),
                                                       title.isEmpty() ? QStringLiteral("Terminal") : title);
            m_sessionTabs->setTabToolTip(index, pane->property("opsremoteTabToolTip").toString());
            m_sessionTabs->setCurrentIndex(index);
            pane->setFocus();
        } else if (const int from = m_sessionTabs->indexOf(group); from >= 0 && drop.tabIndex >= 0) {
            m_sessionTabs->tabBar()->moveTab(from, qBound(0, drop.tabIndex - (drop.tabIndex > from ? 1 : 0),
                                                          m_sessionTabs->count() - 1));
        }
    } else if (canDropOn(dragged, drop.beside)) {
        auto *beside = drop.beside.data();
        if (pane) {
            takePaneOut(pane);
            placePane(pane, beside, drop.orientation, drop.before);
            pane->setFocus();
        } else if (auto *split = splitTabOf(beside)) {
            split->insertGroup(beside, group, drop.orientation, drop.before);
            if (const int index = m_sessionTabs->indexOf(group); index >= 0)
                m_sessionTabs->removeTab(index);
            group->deleteLater();
            m_sessionTabs->setCurrentWidget(split);
        }
    } else {
        return;
    }
    showSftpPanelForCurrentTab();
    panesChanged();
    updateMultiExecIndicator();
}

// A tab was split, or lost a pane: each pane's bar in its place.
void MainWindow::panesChanged()
{
    QTimer::singleShot(0, this, [this] {
        updatePaneBars();
        m_paneBars->placeAll();
    });
}

// Closes one terminal: the whole tab when it is alone, otherwise its pane
// (asking first when it is connected, like closing a tab).
void MainWindow::closePane(TerminalPage *pane)
{
    const int index = tabIndexOf(pane);
    if (index < 0)
        return;
    auto *split = qobject_cast<SplitTab *>(m_sessionTabs->widget(index));
    if (!split || split->panes().size() <= 1) {
        closeSessionTab(index);
        return;
    }
    const QString host = pane->sshSettings().host;
    if (m_startup.confirmCloseConnected && pane->isConnected() && !host.isEmpty()
        && QMessageBox::question(this, QStringLiteral("Close Pane"),
                                 QStringLiteral("Disconnect from %1 and close the pane?").arg(host))
               != QMessageBox::Yes)
        return;
    split->removePane(pane);
    pane->deleteLater();
    // One pane left: the tab becomes a plain terminal tab again.
    if (const auto remaining = split->panes(); remaining.size() == 1) {
        unsplitIfSingle(split);
        m_sessionTabs->setCurrentWidget(remaining.constFirst());
        remaining.constFirst()->setFocus();
    }
    showSftpPanelForCurrentTab();
    panesChanged();
}

// ---- Multi-exec ----

// Tabs → Multi-exec…: which terminals (tabs and split panes) get the typing.
void MainWindow::chooseMultiExecTargets()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Multi-exec"));
    dialog.setMinimumSize(420, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(QStringLiteral("Typing into one of the chosen terminals goes to all of "
                                            "them. Chosen terminals have an accent bar on top."),
                             &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(list, 1);
    QList<QPointer<TerminalPage>> pages;
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        const auto terminals = terminalPagesIn(m_sessionTabs->widget(index));
        for (int pane = 0; pane < terminals.size(); ++pane) {
            auto *terminal = terminals.at(pane);
            QString text = m_sessionTabs->tabText(index);
            if (terminals.size() > 1)
                text += QStringLiteral("  (pane %1)").arg(pane + 1);
            if (!terminal->sshSettings().host.isEmpty())
                text += QStringLiteral(" — %1").arg(terminal->sshSettings().host);
            auto *item = new QListWidgetItem(text, list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(terminal->isMultiExecTarget() ? Qt::Checked : Qt::Unchecked);
            pages.append(terminal);
        }
    }
    if (pages.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Multi-exec"),
                                 QStringLiteral("Open some terminals first."));
        return;
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *all = buttons->addButton(QStringLiteral("All"), QDialogButtonBox::ActionRole);
    auto *none = buttons->addButton(QStringLiteral("None"), QDialogButtonBox::ActionRole);
    const auto checkAll = [list](Qt::CheckState state) {
        for (int row = 0; row < list->count(); ++row)
            list->item(row)->setCheckState(state);
    };
    connect(all, &QPushButton::clicked, list, [checkAll] { checkAll(Qt::Checked); });
    connect(none, &QPushButton::clicked, list, [checkAll] { checkAll(Qt::Unchecked); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    for (int row = 0; row < list->count() && row < pages.size(); ++row) {
        if (pages.at(row))
            pages.at(row)->setMultiExecTarget(list->item(row)->checkState() == Qt::Checked);
    }
    updateMultiExecIndicator();
}

void MainWindow::updateMultiExecIndicator()
{
    int chosen = 0;
    for (auto *page : allTerminalPages())
        chosen += page->isMultiExecTarget() ? 1 : 0;
    m_multiExecIndicator->setText(QStringLiteral("MULTI-EXEC · %1  ✕").arg(chosen));
    m_multiExecIndicator->setVisible(chosen > 0);
}

void MainWindow::notify(const QString &text, bool warning)
{
    if (m_messageLog)
        m_messageLog->add(text, warning ? MessageLog::Level::Warning : MessageLog::Level::Notice);
}

// A tab's (or a split tab's pane's) own messages, kept with the page so they
// move with it when it is split or unsplit and go when it closes.
MessageLog *MainWindow::tabLog(QWidget *page)
{
    if (auto *log = page->findChild<MessageLog *>(QStringLiteral("tabMessages"), Qt::FindDirectChildrenOnly))
        return log;
    auto *log = new MessageLog(page);
    log->setObjectName(QStringLiteral("tabMessages"));
    connect(log, &MessageLog::changed, this, &MainWindow::updatePaneBars);
    return log;
}

void MainWindow::tabMessage(QWidget *page, const QString &text, bool warning)
{
    if (text.isEmpty() && !page)
        return;
    const int index = page ? (m_sessionTabs->indexOf(page) >= 0 ? m_sessionTabs->indexOf(page) : tabIndexOf(page)) : -1;
    if (index < 0) {
        notify(text, warning);
        return;
    }
    // An empty message: the pane connected (again), so what it said before
    // (a disconnect reason) is put away.
    if (text.isEmpty()) {
        tabLog(page)->markAllRead();
        return;
    }
    // The history is at the bottom right, with where it came from (new
    // there only when the pane is out of sight).
    if (m_messageLog)
        m_messageLog->add(QStringLiteral("%1: %2").arg(paneLabel(page), text),
                          warning ? MessageLog::Level::Warning : MessageLog::Level::Notice,
                          m_paneBars->shown() && page->isVisible());
    if (m_paneBars->shown())
        tabLog(page)->add(text, warning ? MessageLog::Level::Warning : MessageLog::Level::Notice);
}

// "web01" or, in a split tab, "web01 › Pane 2".
QString MainWindow::paneLabel(QWidget *page) const
{
    const int index = m_sessionTabs->indexOf(page) >= 0 ? m_sessionTabs->indexOf(page) : tabIndexOf(page);
    QString label = index >= 0 ? m_sessionTabs->tabText(index).remove(QLatin1Char('&')) : QString();
    if (auto *split = index >= 0 ? qobject_cast<SplitTab *>(m_sessionTabs->widget(index)) : nullptr) {
        const auto panes = split->panes();
        for (int pane = 0; pane < panes.size(); ++pane)
            if (panes.size() > 1 && panes[pane] == page)
                label += QStringLiteral(" › Pane %1").arg(pane + 1);
    }
    return label;
}

// The pages of a tab: one, or one per pane ("Pane 2") in a split tab.
QList<QPair<QString, QWidget *>> MainWindow::tabPages(QWidget *tab)
{
    QList<QPair<QString, QWidget *>> pages;
    if (!tab)
        return pages;
    if (auto *split = qobject_cast<SplitTab *>(tab)) {
        const auto panes = split->panes();
        for (int index = 0; index < panes.size(); ++index)
            pages.append({panes.size() > 1 ? QStringLiteral("Pane %1").arg(index + 1) : QString(), panes[index]});
    } else {
        pages.append({QString(), tab});
    }
    return pages;
}

TabPrompt *MainWindow::promptOf(QWidget *page)
{
    return page->findChild<TabPrompt *>(QStringLiteral("tabPrompt"), Qt::FindDirectChildrenOnly);
}

// The transfer a pane's bar shows: its newest running one; else the last
// ended one still shown (done: a few seconds; failed or cancelled: until put
// away).
std::optional<TransferList::Transfer> MainWindow::paneTransfer(QWidget *page) const
{
    constexpr int DoneShownMs = 4000;
    std::optional<TransferList::Transfer> ended;
    for (const auto &transfer : TransferList::instance()->transfers()) {
        if (!transfer.owner || (transfer.owner != page && !page->isAncestorOf(transfer.owner)))
            continue;
        if (transfer.state == TransferList::State::Running)
            return transfer;
        if (ended || m_transfersPutAway.contains(transfer.id))
            continue;
        if (transfer.state != TransferList::State::Done
            || transfer.ended.msecsTo(QDateTime::currentDateTime()) < DoneShownMs)
            ended = transfer;
    }
    return ended;
}

// Every pane's bar: a question first, else its newest message not put away;
// its transfer on the right.
void MainWindow::updatePaneBars()
{
    if (!m_paneBars)
        return;
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        for (const auto &[name, page] : tabPages(m_sessionTabs->widget(index)))
            updatePaneBar(page);
    }
}

void MainWindow::updatePaneBar(QWidget *page)
{
    auto *bar = m_paneBars->bar(page);
    bool needed = false;
    auto *prompt = TabPrompt::inStrip() ? promptOf(page) : nullptr;
    if (prompt && prompt->isActive()) {
        QList<QPair<QString, QString>> buttons;
        for (const auto &action : prompt->actions())
            buttons.append({action.label, action.tip});
        bar->showPrompt(prompt->text(), prompt->isWarning(), {}, buttons, prompt->canDismiss(), 0);
        needed = true;
    } else {
        bar->clearPrompt();
        MessageLog::Entry newest;
        for (const auto &entry : tabLog(page)->entries()) {
            if (!entry.read && (newest.text.isEmpty() || entry.time > newest.time))
                newest = entry;
        }
        if (newest.text.isEmpty()) {
            bar->clear();
        } else {
            bar->showEntry(newest, {});
            needed = true;
            // A notice goes after a while in view; a warning stays until ✕.
            if (newest.level == MessageLog::Level::Notice)
                hideNoticeLater(page, newest.time);
        }
    }
    if (const auto transfer = paneTransfer(page)) {
        static const QString ended[] = {QString(), QStringLiteral("done"), QStringLiteral("failed"),
                                        QStringLiteral("cancelled")};
        const bool running = transfer->state == TransferList::State::Running;
        bar->showTransfer(transfer->name, transfer->upload, transfer->percent(), 0,
                          running ? (transfer->cancel ? transfer->cancelHint : QString())
                                  : QStringLiteral("Put this away"),
                          running ? QString() : ended[int(transfer->state)]);
        needed = true;
        if (transfer->state == TransferList::State::Done) // gone a few seconds after
            QTimer::singleShot(4100, this, &MainWindow::updatePaneBars);
    } else {
        bar->clearTransfer();
    }
    m_paneBars->setNeeded(page, needed);
}

// The notice shown in page's bar is put away after 8 seconds in view (the
// clock waits while its tab is not in front); it stays in Messages.
void MainWindow::hideNoticeLater(QWidget *page, const QDateTime &time)
{
    const auto key = qMakePair(static_cast<QObject *>(page), time.toMSecsSinceEpoch());
    if (m_noticeTimers.contains(key))
        return;
    m_noticeTimers.insert(key);
    const QPointer<QWidget> pane(page);
    auto check = std::make_shared<std::function<void()>>();
    *check = [this, pane, time, key, check] {
        if (!pane) {
            m_noticeTimers.remove(key);
            return;
        }
        if (!pane->isVisible()) {
            QTimer::singleShot(8000, this, *check);
            return;
        }
        m_noticeTimers.remove(key);
        tabLog(pane)->markRead(time);
    };
    QTimer::singleShot(8000, this, *check);
}

// Where a transfer comes from, for the lists: its tab's title, or the
// window it belongs to (Network Tools).
QString MainWindow::transferPlace(QWidget *owner) const
{
    if (!owner)
        return {};
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *tab = m_sessionTabs->widget(index);
        if (tab == owner || tab->isAncestorOf(owner))
            return m_sessionTabs->tabText(index).remove(QLatin1Char('&'));
    }
    return owner->window() != this ? owner->window()->windowTitle() : QString();
}

// The Transfers indicator (all of them) and the current tab's strip.
void MainWindow::updateTransfers()
{
    auto *transfers = TransferList::instance();
    const auto running = transfers->running();
    if (m_transfersIndicator) {
        m_transfersIndicator->setState(running.isEmpty() ? QStringLiteral("Transfers · 0")
                                                         : QStringLiteral("Transfers · %1 · %2%")
                                                               .arg(running.size())
                                                               .arg(transfers->runningPercent()),
                                       !running.isEmpty());
    }
    updatePaneBars();
}
