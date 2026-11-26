// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// The main window: building it, menus, toolbar, command line, preferences
// and events. Its areas are in mainwindow_<area>.cpp; shared helpers in
// mainwindow_p.h.

#include "mainwindow_p.h"

using namespace mainwindow_p;

namespace {

// Renaming in the session tree: the editor holds just the name (the list
// may show "name (host)"), and the new name goes through MainWindow.
// The session list's look: a status dot before every session's icon, a
// chosen space between a folder's arrow and its icon (so both gaps can look
// alike), and folder summaries like 3 ● 1 ○ after the name or in a column
// of their own.
struct SessionListLook
{
    enum class Dot { None, Unknown, Checking, Up, Down };
    std::function<Dot(const QModelIndex &index)> dotOf;
    std::function<bool()> blinkOn;
    // Folders: (hosts that answer, hosts that do not); nothing for others.
    std::function<std::optional<QPair<int, int>>(const QModelIndex &index)> summaryOf;
    // The column showing folder summaries; 0 means after the name.
    std::function<int()> summaryColumn;
    // Pixels between a folder's (or Pinned's, Recent's) arrow and its icon.
    std::function<int()> folderGap;

    // Where the row's icon starts, from the left of its cell.
    int shiftOf(const QModelIndex &index) const
    {
        if (folderGap && dotOf && dotOf(index) == Dot::None)
            return folderGap();
        return DotSpace;
    }

    static constexpr int DotSize = 5;
    static constexpr int DotSpace = DotSize + 7;

    static QColor upColor() { return QColor(0x7c, 0xc5, 0x8a); }
    static QColor downColor() { return QColor(0xe0, 0x8a, 0x8a); }
    static QColor greyColor() { return QColor(0xa0, 0xa0, 0xa0); }

    static void drawDot(QPainter *painter, const QPointF &center, Dot dot, bool blinkOn)
    {
        if (dot == Dot::None)
            return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QColor color = dot == Dot::Up ? upColor() : dot == Dot::Down ? downColor() : greyColor();
        // The shape carries the meaning (filled answers, empty does not), so
        // it reads without telling green from red. Sessions not checked (the
        // checks are off, or through a jump host) are empty too, in grey.
        const QRectF circle(center.x() - DotSize / 2.0, center.y() - DotSize / 2.0, DotSize, DotSize);
        const bool filled = dot == Dot::Up || (dot == Dot::Checking && blinkOn);
        painter->setPen(QPen(color, 1.1));
        painter->setBrush(filled ? QBrush(color) : Qt::NoBrush);
        painter->drawEllipse(circle);
        painter->restore();
    }

    // "3 ● 1 ○" with the status dots; its width with draw = false.
    static int drawSummary(QPainter *painter, const QFontMetrics &metrics, int x, int centerY,
                           QPair<int, int> counts, const QColor &text, bool draw = true)
    {
        if (counts.first + counts.second == 0)
            return 0;
        const auto part = [&](const QString &piece) {
            if (draw) {
                painter->setPen(text);
                painter->drawText(QPoint(x, centerY + (metrics.ascent() - metrics.descent()) / 2), piece);
            }
            x += metrics.horizontalAdvance(piece);
        };
        const auto dot = [&](Dot kind) {
            if (draw)
                drawDot(painter, QPointF(x + DotSize / 2.0 + 1, centerY), kind, false);
            x += DotSize + 2;
        };
        const int start = x;
        if (draw)
            painter->save();
        part(QStringLiteral("%1 ").arg(counts.first));
        dot(Dot::Up);
        part(QStringLiteral(" %1 ").arg(counts.second));
        dot(Dot::Down);
        if (draw)
            painter->restore();
        return x - start;
    }
};

class SessionNameDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    std::function<QString(const QModelIndex &index)> nameOf;
    std::function<void(const QModelIndex &index, const QString &name)> rename;
    SessionListLook look;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        // The row's background (selection, hover) over the dot's space too.
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = QIcon();
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

        if (look.dotOf)
            SessionListLook::drawDot(painter,
                                     QPointF(option.rect.left() + SessionListLook::DotSize / 2.0 + 2,
                                             option.rect.center().y() + 0.5),
                                     look.dotOf(index), look.blinkOn && look.blinkOn());
        QStyleOptionViewItem content = option;
        content.rect.adjust(look.shiftOf(index), 0, 0, 0);
        QStyledItemDelegate::paint(painter, content, index);

        if (look.summaryOf && look.summaryColumn && look.summaryColumn() == 0) {
            if (const auto counts = look.summaryOf(index)) {
                QStyleOptionViewItem text = content;
                initStyleOption(&text, index);
                const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &text, widget);
                const int x = textRect.left() + text.fontMetrics.horizontalAdvance(text.text) + 8;
                if (x < option.rect.right())
                    SessionListLook::drawSummary(painter, text.fontMetrics, x, textRect.center().y(),
                                                 *counts, summaryColor(option));
            }
        }
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.rwidth() += look.shiftOf(index);
        if (look.summaryOf && look.summaryColumn && look.summaryColumn() == 0) {
            if (const auto counts = look.summaryOf(index))
                size.rwidth() += 8 + SessionListLook::drawSummary(nullptr, option.fontMetrics, 0, 0,
                                                                  *counts, QColor(), false);
        }
        return size;
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override
    {
        QStyleOptionViewItem shifted = option;
        shifted.rect.adjust(look.shiftOf(index), 0, 0, 0);
        QStyledItemDelegate::updateEditorGeometry(editor, shifted, index);
    }

    static QColor summaryColor(const QStyleOptionViewItem &option)
    {
        return option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText
                                                                          : QPalette::PlaceholderText);
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (auto *line = qobject_cast<QLineEdit *>(editor)) {
            line->setText(nameOf(index));
            line->selectAll();
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget *editor, QAbstractItemModel *, const QModelIndex &index) const override
    {
        if (auto *line = qobject_cast<QLineEdit *>(editor))
            rename(index, line->text());
    }
};

// The Host, Protocol, and summary columns: plain text, except a folder's
// summary in the column chosen for it.
class SessionColumnDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    SessionListLook look;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        if (!look.summaryOf || !look.summaryColumn || look.summaryColumn() != index.column())
            return;
        if (const auto counts = look.summaryOf(index.siblingAtColumn(0))) {
            QStyleOptionViewItem text = option;
            initStyleOption(&text, index);
            SessionListLook::drawSummary(painter, option.fontMetrics, option.rect.left() + 4,
                                         option.rect.center().y(), *counts,
                                         option.palette.color(option.state & QStyle::State_Selected
                                                                  ? QPalette::HighlightedText
                                                                  : QPalette::PlaceholderText));
        }
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (look.summaryOf && look.summaryColumn && look.summaryColumn() == index.column()) {
            if (const auto counts = look.summaryOf(index.siblingAtColumn(0)))
                size.setWidth(qMax(size.width(), 12 + SessionListLook::drawSummary(
                                                          nullptr, option.fontMetrics, 0, 0, *counts,
                                                          QColor(), false)));
        }
        return size;
    }
};

// Session tree with drag-and-drop moves. canMove() decides whether an item
// may become a child of a new parent (nullptr is the top level); rejected
// targets show the "not allowed" cursor.
class SessionTreeWidget final : public QTreeWidget
{
public:
    explicit SessionTreeWidget(QWidget *parent = nullptr)
        : QTreeWidget(parent)
    {
        setDragDropMode(QAbstractItemView::InternalMove);
        setDefaultDropAction(Qt::MoveAction);
        setDropIndicatorShown(true);
        // F2 renames in place; a double click opens the session instead.
        setEditTriggers(QAbstractItemView::EditKeyPressed);
    }

    QTreeWidgetItem *item(const QModelIndex &index) const { return itemFromIndex(index); }

    std::function<bool(const QTreeWidgetItem *item, const QTreeWidgetItem *newParent)> canMove;
    std::function<void(QTreeWidgetItem *item)> itemMoved;

protected:
    // On a selected folder some styles (Kvantum's light ones) draw the
    // open/close arrow in about the colour of the selection, where it cannot
    // be seen. The style's drawing is checked pixel by pixel (themes paint
    // from their own images, so their palette does not tell): only when
    // nothing in the arrow's cell stands out is the arrow drawn again, in
    // whichever of the palette's text colours stands out from what is there.
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override
    {
        const auto *item = itemFromIndex(index);
        if (!item || item->childCount() == 0 || !selectionModel()->isSelected(index) || rect.isEmpty()) {
            QTreeWidget::drawBranches(painter, rect, index);
            return;
        }
        const qreal ratio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        QImage drawn((rect.size() * ratio), QImage::Format_ARGB32_Premultiplied);
        drawn.setDevicePixelRatio(ratio);
        drawn.fill(palette().color(QPalette::Base));
        {
            QPainter offscreen(&drawn);
            offscreen.translate(-rect.topLeft());
            QTreeWidget::drawBranches(&offscreen, rect, index);
        }
        painter->drawImage(rect.topLeft(), drawn);

        const auto brightness = [](QRgb pixel) {
            return (0.299 * qRed(pixel) + 0.587 * qGreen(pixel) + 0.114 * qBlue(pixel)) / 255.0;
        };
        // The arrow's cell: the last indentation-wide part of the branches.
        const int width = indentation();
        const QRect cell(int((rect.width() - width) * ratio), 0, int(width * ratio), drawn.height());
        const QRect inside = cell.intersected(drawn.rect());
        if (inside.width() < 3 || inside.height() < 3)
            return;
        const double background = brightness(drawn.pixel(inside.left() + 1, inside.center().y()));
        double strongest = 0;
        for (int y = inside.top(); y <= inside.bottom(); ++y)
            for (int x = inside.left(); x <= inside.right(); ++x)
                strongest = std::max(strongest, std::abs(brightness(drawn.pixel(x, y)) - background));
        if (strongest > 0.3)
            return; // the style's arrow shows
        const QColor light = palette().color(QPalette::HighlightedText);
        const QColor dark = palette().color(QPalette::Text);
        const auto lightness = [](const QColor &color) {
            return 0.299 * color.redF() + 0.587 * color.greenF() + 0.114 * color.blueF();
        };
        const QColor arrowColor = std::abs(lightness(light) - background) >= std::abs(lightness(dark) - background)
                                      ? light : dark;
        const QRectF arrowCell(rect.right() - width + 1, rect.top(), width, rect.height());
        const double half = std::min<double>(width, arrowCell.height()) / 4.0;
        const QPointF center = arrowCell.center();
        QPolygonF arrow;
        if (item->isExpanded())
            arrow << center + QPointF(-half, -half / 2) << center + QPointF(half, -half / 2)
                  << center + QPointF(0, half / 2 + 1);
        else if (layoutDirection() == Qt::RightToLeft)
            arrow << center + QPointF(half / 2, -half) << center + QPointF(half / 2, half)
                  << center + QPointF(-half / 2 - 1, 0);
        else
            arrow << center + QPointF(-half / 2, -half) << center + QPointF(-half / 2, half)
                  << center + QPointF(half / 2 + 1, 0);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(arrowColor);
        painter->drawPolygon(arrow);
        painter->restore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        QTreeWidget::dragMoveEvent(event);
        if (event->isAccepted() && !dropAllowed(event->position().toPoint()))
            event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
        auto *item = currentItem();
        if (!item || event->source() != this || !dropAllowed(event->position().toPoint())) {
            event->ignore();
            return;
        }
        // Moving re-parents the same item objects, but Qt collapses them.
        QHash<QTreeWidgetItem *, bool> expanded;
        std::function<void(QTreeWidgetItem *)> remember = [&](QTreeWidgetItem *node) {
            expanded.insert(node, node->isExpanded());
            for (int index = 0; index < node->childCount(); ++index)
                remember(node->child(index));
        };
        remember(item);
        QTreeWidget::dropEvent(event);
        for (auto iterator = expanded.cbegin(); iterator != expanded.cend(); ++iterator)
            iterator.key()->setExpanded(iterator.value());
        if (item->parent())
            item->parent()->setExpanded(true);
        setCurrentItem(item);
        if (itemMoved)
            itemMoved(item);
    }

private:
    bool dropAllowed(const QPoint &position) const
    {
        const auto *item = currentItem();
        if (!item || !canMove)
            return false;
        const auto *target = itemAt(position);
        const QTreeWidgetItem *newParent = nullptr;
        switch (dropIndicatorPosition()) {
        case QAbstractItemView::OnItem:
            newParent = target;
            break;
        case QAbstractItemView::AboveItem:
        case QAbstractItemView::BelowItem:
            newParent = target ? target->parent() : nullptr;
            break;
        case QAbstractItemView::OnViewport:
            break;
        }
        for (auto *ancestor = newParent; ancestor; ancestor = ancestor->parent()) {
            if (ancestor == item)
                return false;
        }
        return canMove(item, newParent);
    }
};

// The session tabs; tells when a tab is added, wherever that happens.
class SessionTabWidget final : public QTabWidget
{
public:
    explicit SessionTabWidget(QWidget *parent) : QTabWidget(parent)
    {
        m_corners = new RoundedCorners(this);
        if (auto *pages = findChild<QStackedWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            m_pages = pages;
            pages->installEventFilter(this);
        }
    }
    std::function<void(int)> inserted;
    // After the panes' bars were placed: the corners go back on top of them.
    void raiseCorners()
    {
        if (m_pages)
            m_corners->setArea(m_pages->geometry());
    }

protected:
    void tabInserted(int index) override
    {
        QTabWidget::tabInserted(index);
        if (inserted)
            inserted(index);
    }
    // The tabs' area: round at its corners (with the panes' bars inside it).
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize)
            m_corners->setArea(static_cast<QWidget *>(watched)->geometry());
        return QTabWidget::eventFilter(watched, event);
    }

private:
    RoundedCorners *m_corners = nullptr;
    QWidget *m_pages = nullptr;
};

} // namespace

// The window's widgets, the tabs among them, are destroyed after this, while
// the window is half gone; and as tabs go, their questions and transfers end
// and they say so. Nothing may reach the window any more: it walked the tabs
// being destroyed (a crash when closing with several tabs open, on Windows).
MainWindow::~MainWindow()
{
    disconnect(TabPrompt::hub(), nullptr, this, nullptr);
    disconnect(TransferList::instance(), nullptr, this, nullptr);
    TransferList::ownerName = {};
    m_paneBars->stop();
    for (QWidget *area : {static_cast<QWidget *>(m_sessionTabs), static_cast<QWidget *>(m_sftpStack)}) {
        if (!area)
            continue;
        disconnect(area, nullptr, this, nullptr);
        for (auto *object : area->findChildren<QObject *>())
            disconnect(object, nullptr, this, nullptr);
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_vault(stateFilePath(QStringLiteral("credentials.vault")))
{
    migrateStateFromDataDirectory();
    // A new installation starts with the OpsRemote Light Orange look, matching the icon.
    const bool firstStart = !QFileInfo::exists(settingsFilePath());
    SftpPanel::removeStaleEditFolders();
    m_systemStyleKey = QApplication::style()->objectName();
    m_systemPalette = QApplication::palette();
    m_systemFont = QApplication::font();
    applyInterfaceFont(m_systemFont, loadInterfaceFont());
    setWindowBordersEnabled(loadWindowBorderPreference());
    RoundedCorners::setRadius(loadCornerRadiusPreference());
    ::setSeeThroughLists(loadSeeThroughListsPreference());
    initializeIconThemes();
    m_iconTheme = loadIconThemePreference();
    bool knownIconTheme = false;
    for (const auto &theme : availableIconThemes())
        knownIconTheme = knownIconTheme || theme.first == m_iconTheme;
    if (!knownIconTheme)
        m_iconTheme.clear();
    setIconTheme(m_iconTheme);
    m_selectedStyleKey = loadUiStylePreference();
    m_colorThemeKey = loadColorThemePreference();
    if (m_colorThemeKey != QStringLiteral("system")
        && m_colorThemeKey != QStringLiteral("light")
        && m_colorThemeKey != QStringLiteral("dark")
        && m_colorThemeKey != QStringLiteral("custom")
        && m_colorThemeKey != QStringLiteral("auto")
        && !findPalettePreset(m_colorThemeKey))
        m_colorThemeKey = QStringLiteral("system");
    m_customColors = loadCustomColors();
    m_userLooks = loadUserLooks();
    m_baseLookId = loadBaseLookId();
    m_terminalAppearance = loadTerminalAppearance();
    m_darkTerminalColorScheme = loadDarkTerminalColorScheme();
    m_behaviour = loadTerminalBehaviour();
    m_startup = loadStartupPreferences();
    Clipboard::setPreferences(loadMouseClipboardPreferences());
    Clipboard::installTextFieldBehaviour();
    m_shortcuts = loadShortcuts();
    RemoteScreenPage::setFullScreenKey(m_shortcuts.value(QStringLiteral("remote_full_screen")));
    m_security = loadSecurityPreferences();
    m_logging = loadLoggingPreferences();
    m_presentation = loadPresentationPreferences();
    m_highlightRules = loadHighlightRules();
    m_snippets = loadSnippets();
    // A copy of settings/ per start, so a bad edit or import can be undone.
    if (const auto backup = loadBackupPreferences(); backup.backupOnStart) {
        createBackup();
        pruneBackups(backup.keep);
    }
    // "Automatic" colour mode follows the OS light/dark setting live.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_colorThemeKey == QStringLiteral("auto")) {
            applyColorTheme(m_colorThemeKey);
            applyTerminalAppearanceToTabs();
        }
    });
    applyUiStyle(m_selectedStyleKey);

    setWindowTitle(QStringLiteral("OpsRemote"));
    resize(1100, 720);

    // Fixed layout: the left panel and tabs share a splitter and cannot be
    // undocked, floated, or rearranged.
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    // The side panel keeps off the window's left edge as far as the tabs keep
    // off the panel.
    splitter->setContentsMargins(splitter->handleWidth(), 0, 0, 0);
    setCentralWidget(splitter);

    m_leftTabs = new QTabWidget(splitter);
    m_leftTabs->setDocumentMode(true);
    m_leftTabs->setMinimumWidth(220);
    auto *sessionTree = new SessionTreeWidget(m_leftTabs);
    auto *nameDelegate = new SessionNameDelegate(sessionTree);
    nameDelegate->nameOf = [this, sessionTree](const QModelIndex &index) {
        auto *item = sessionTree->item(index);
        if (!item || isFolder(item))
            return item ? item->text(0) : QString();
        return m_sessions.value(item->data(0, SessionIdRole).toString()).name;
    };
    nameDelegate->rename = [this, sessionTree](const QModelIndex &index, const QString &name) {
        auto *item = sessionTree->item(index);
        // A copy in Pinned or Recent renames the session itself.
        if (item && item->data(0, ItemKindRole).toString() == QStringLiteral("clone"))
            item = realSessionItem(item->data(0, SessionIdRole).toString());
        if (item)
            renameTreeItem(item, name);
    };
    SessionListLook look;
    look.dotOf = [this, sessionTree](const QModelIndex &index) {
        auto *item = sessionTree->item(index);
        if (!item || isFolder(item) || item->data(0, ItemKindRole).toString() == QStringLiteral("quick"))
            return SessionListLook::Dot::None;
        if (!m_sessionList.checkStatus)
            return SessionListLook::Dot::Unknown;
        switch (m_hostStatus->status(item->data(0, SessionIdRole).toString())) {
        case HostStatusChecker::Status::Up:
            return SessionListLook::Dot::Up;
        case HostStatusChecker::Status::Down:
            return SessionListLook::Dot::Down;
        case HostStatusChecker::Status::Checking:
            return SessionListLook::Dot::Checking;
        case HostStatusChecker::Status::Unknown:
            break;
        }
        return SessionListLook::Dot::Unknown;
    };
    look.blinkOn = [this] { return m_blinkOn; };
    look.folderGap = [this] { return m_sessionList.folderGap; };
    look.summaryOf = [this, sessionTree](const QModelIndex &index) -> std::optional<QPair<int, int>> {
        auto *item = sessionTree->item(index);
        if (!m_sessionList.checkStatus || !item
            || (!isFolder(item) && item->data(0, ItemKindRole).toString() != QStringLiteral("quick")))
            return std::nullopt;
        return folderStatusCounts(item);
    };
    look.summaryColumn = [this] { return summaryColumn(); };
    nameDelegate->look = look;
    sessionTree->setItemDelegate(nameDelegate);
    auto *columnDelegate = new SessionColumnDelegate(sessionTree);
    columnDelegate->look = look;
    for (const int column : {HostColumn, ProtocolColumn, SummaryColumn})
        sessionTree->setItemDelegateForColumn(column, columnDelegate);
    sessionTree->canMove = [this](const QTreeWidgetItem *item, const QTreeWidgetItem *newParent) {
        // Pinned, Recent, and their copies stay where they are.
        if (newParent && newParent->data(0, ItemKindRole).toString() == QStringLiteral("quick"))
            return false;
        // Sessions go in a folder or the top level; folders keep the nesting limit.
        if (!isFolder(item))
            return !newParent || isFolder(newParent);
        if (newParent && !isFolder(newParent))
            return false;
        std::function<int(const QTreeWidgetItem *)> subtreeDepth = [&](const QTreeWidgetItem *node) {
            int deepest = 0;
            for (int index = 0; index < node->childCount(); ++index) {
                if (isFolder(node->child(index)))
                    deepest = std::max(deepest, subtreeDepth(node->child(index)));
            }
            return deepest + 1;
        };
        return (newParent ? folderDepth(newParent) : 0) + subtreeDepth(item) <= MaximumFolderDepth;
    };
    sessionTree->itemMoved = [this](QTreeWidgetItem *item) {
        saveSessions();
        saveFolderExpansion();
    };
    m_sessionTree = sessionTree;
    m_sessionTree->setObjectName(QStringLiteral("sessionTree"));
    m_sessionTree->setColumnCount(4);
    m_sessionTree->setHeaderLabels({QStringLiteral("Saved sessions"), QStringLiteral("Host"),
                                    QStringLiteral("Protocol"), QString()});
    m_sessionList = loadSessionListPreferences();
    m_localShell = loadLocalShellPreferences();
    // Each password's store: the session's own choice, else the Preferences'.
    m_vault.setStoreRule([this](const QString &key) {
        const auto session = m_sessions.constFind(key.section(QLatin1Char('#'), 0, 0));
        QString choice = session != m_sessions.cend() ? session->passwordStore : QString();
        if (choice.isEmpty())
            choice = m_security.passwordStore;
        return choice == QStringLiteral("system") ? PasswordStore::System
               : choice == QStringLiteral("both") ? PasswordStore::Both
                                                  : PasswordStore::Vault;
    });
    m_recentSessions = loadRecentSessions();
    m_hostStatus = new HostStatusChecker(this);
    connect(m_hostStatus, &HostStatusChecker::changed, this, [this] {
        // The dot blinks (hollow, filled) while its check runs.
        if (m_hostStatus->isChecking() && !m_blinkTimer->isActive())
            m_blinkTimer->start();
        m_sessionTree->viewport()->update();
        if (!m_hostStatus->isChecking())
            fitSessionPanel();
    });
    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(500);
    connect(m_blinkTimer, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        if (!m_hostStatus->isChecking()) {
            m_blinkTimer->stop();
            m_blinkOn = false;
        }
        m_sessionTree->viewport()->update();
    });
    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::checkHostStatus);
    m_sessionTree->setContextMenuPolicy(Qt::CustomContextMenu);
    // The Sessions tab: a search box above the list.
    auto *sessionsPage = new QWidget(m_leftTabs);
    auto *sessionsLayout = new QVBoxLayout(sessionsPage);
    sessionsLayout->setContentsMargins(0, 4, 0, 0);
    sessionsLayout->setSpacing(4);
    m_sessionSearch = new QLineEdit(sessionsPage);
    m_sessionSearch->setObjectName(QStringLiteral("sessionSearch"));
    m_sessionSearch->setPlaceholderText(QStringLiteral("Search sessions"));
    m_sessionSearch->setClearButtonEnabled(true);
    m_sessionSearch->setToolTip(QStringLiteral(
        "Parts of a name, host, user, protocol, or folder; every word must match.\n"
        "Down: go to the results. Enter: open the first one. Esc: clear."));
    m_sessionSearch->installEventFilter(this);
    connect(m_sessionSearch, &QLineEdit::textChanged, this, &MainWindow::applySessionFilter);
    m_noSessionMatches = new QLabel(QStringLiteral("No session matches."), sessionsPage);
    m_noSessionMatches->setAlignment(Qt::AlignCenter);
    m_noSessionMatches->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    m_noSessionMatches->hide();
    // The search box, like the tabs above, starts where the list's box does
    // (inside its frame); see alignSidePanelRows().
    m_sessionSearchRow = new QHBoxLayout;
    m_sessionSearchRow->addWidget(m_sessionSearch);
    sessionsLayout->addLayout(m_sessionSearchRow);
    sessionsLayout->addWidget(m_noSessionMatches);
    sessionsLayout->addWidget(m_sessionTree, 1);
    RoundedCorners::follow(m_sessionTree);
    m_leftTabs->addTab(sessionsPage, QStringLiteral("Sessions"));
    m_leftTabs->tabBar()->installEventFilter(this); // moved in by alignSidePanelRows()
    alignSidePanelRows();
    connect(this, &MainWindow::appearanceChanged, this, &MainWindow::alignSidePanelRows);
    // Its header is the first row when the search box is hidden: the row height
    // the tab strip has too.
    m_sessionTree->header()->setFixedHeight(panelRowHeight(m_sessionTree));
    connect(this, &MainWindow::appearanceChanged, this, [this] {
        m_sessionTree->header()->setFixedHeight(panelRowHeight(m_sessionTree));
    });

    // Each SSH tab owns its own SFTP panel; the stack shows the active one.
    m_sftpStack = new QStackedWidget(m_leftTabs);
    // A Files panel without a session: the same look, the reason in its list.
    m_sftpPlaceholder = new SftpPanel(m_sftpStack);
    m_sftpPlaceholder->showPlaceholder(QStringLiteral("Files"),
                                       QStringLiteral("Open an SSH session to browse its files here."));
    m_sftpStack->addWidget(m_sftpPlaceholder);
    // "Files": SFTP, SCP or FTP; the panel's header says which.
    m_leftTabs->addTab(m_sftpStack, QStringLiteral("Files"));
    m_commandsPanel = new CommandsPanel([this] { return savedScriptNames(); }, m_leftTabs);
    m_commandsPanel->setSnippets(m_snippets);
    connect(m_commandsPanel, &CommandsPanel::sendRequested, this, &MainWindow::sendSnippet);
    connect(m_commandsPanel, &CommandsPanel::snippetsEdited, this, &MainWindow::setSnippets);
    m_leftTabs->addTab(m_commandsPanel, QStringLiteral("Commands"));
    m_scriptsPanel = new ScriptsPanel(m_leftTabs);
    connect(m_scriptsPanel, &ScriptsPanel::runRequested, this, [this](const QString &name) {
        if (auto *page = activeTerminal(m_sessionTabs->currentWidget()))
            runSavedScriptInTab(page, name);
        else
            notify(QStringLiteral("Open an SSH tab (or a local terminal) first."));
    });
    connect(m_scriptsPanel, &ScriptsPanel::runOnHostsRequested, this, [this](const QString &name) {
        auto *item = selectedItem();
        showRunCommand(item && isFolder(item) ? item : nullptr, scriptMarker(name));
    });
    connect(m_scriptsPanel, &ScriptsPanel::editRequested, this, [this](const QString &name) { showScripts(name); });
    connect(m_scriptsPanel, &ScriptsPanel::newRequested, this, [this] {
        showScripts();
        m_scriptsWindow->newScript();
    });
    m_leftTabs->addTab(m_scriptsPanel, QStringLiteral("Scripts"));

    auto *sessionTabs = new SessionTabWidget(splitter);
    // Each new tab gets its (idle) activity mark.
    sessionTabs->inserted = [this](int index) { showTabActivity(index); };
    m_sessionTabs = sessionTabs;
    // A bar on each pane with what it says (see PaneBars); the history of
    // messages and transfers is at the bottom right.
    m_paneBars = new PaneBars(sessionTabs);
    // Terminals and whole tabs dragged to other places (see PaneDrag): a pane
    // by its bar or grip, a tab pulled down out of the tab strip.
    m_paneDrag = new PaneDrag(sessionTabs);
    // Moving can be switched off (Preferences → Workspace → Panes).
    m_paneBars->movable = [this](QWidget *page) {
        return m_startup.moveTerminals && qobject_cast<TerminalPage *>(page) != nullptr;
    };
    m_paneDrag->movable = [this](QWidget *page) {
        return m_startup.moveTerminals
            && (qobject_cast<TerminalPage *>(page) != nullptr || qobject_cast<SplitTab *>(page) != nullptr);
    };
    m_paneDrag->canDrop = [this](QWidget *dragged, TerminalPage *target) { return canDropOn(dragged, target); };
    m_paneDrag->panesOf = [this](QWidget *tab) { return terminalPagesIn(tab); };
    m_paneDrag->activePaneOf = [this](QWidget *tab) { return activeTerminal(tab); };
    m_paneDrag->dropped = [this](QWidget *dragged, const PaneDrag::Drop &drop) { moveDropped(dragged, drop); };
    connect(m_paneBars, &PaneBars::dragRequested, m_paneDrag, &PaneDrag::start);
    connect(m_paneBars, &PaneBars::menuRequested, this, [this](QWidget *page, const QPoint &global) {
        auto *pane = qobject_cast<TerminalPage *>(page);
        if (!pane)
            return;
        QMenu menu(this);
        fillMoveMenu(&menu, pane);
        menu.exec(global);
    });
    // Questions about a transfer name its tab ("smoke@host › Pane 2").
    TransferList::ownerName = [this](QWidget *owner) { return paneLabel(owner); };
    m_paneBars->floats = [](QWidget *page) { return qobject_cast<RemoteScreenPage *>(page) != nullptr; };
    m_paneBars->placed = [sessionTabs] { sessionTabs->raiseCorners(); };
    m_paneBars->setPlace(m_startup.messageStrip, m_startup.messageBar);
    applyMoveSettings();
    TabPrompt::setInStrip(m_paneBars->shown());
    connect(m_paneBars, &PaneBars::barCreated, this, [this](QWidget *page, TabMessageStrip *bar) {
        const QPointer<QWidget> pane(page);
        connect(bar, &TabMessageStrip::promptActionRequested, this, [pane](int action) {
            if (auto *prompt = pane ? promptOf(pane) : nullptr)
                prompt->run(action);
        });
        connect(bar, &TabMessageStrip::promptDismissRequested, this, [pane] {
            if (auto *prompt = pane ? promptOf(pane) : nullptr)
                prompt->dismiss();
        });
        connect(bar, &TabMessageStrip::dismissRequested, this, [this, pane] {
            if (pane)
                tabLog(pane)->markAllRead();
        });
        connect(bar, &TabMessageStrip::transfersRequested, this, [this] {
            if (m_transfersIndicator)
                m_transfersIndicator->click(); // the list of all of them, at the bottom right
        });
        // ✕ on the transfer: cancels a running one, puts an ended one away.
        connect(bar, &TabMessageStrip::cancelTransferRequested, this, [this, pane] {
            if (!pane)
                return;
            const auto shown = paneTransfer(pane);
            if (!shown)
                return;
            if (shown->state == TransferList::State::Running)
                TransferList::instance()->cancel(shown->id);
            else {
                m_transfersPutAway.insert(shown->id);
                updatePaneBars();
            }
        });
    });
    connect(TabPrompt::hub(), &TabPrompt::changed, this, &MainWindow::updatePaneBars);
    connect(m_sessionTabs, &QTabWidget::currentChanged, this, [this] {
        updatePaneBars();
        m_paneBars->placeAll();
    });
    // Colours from the look follow a new look, once its palette is applied.
    connect(this, &MainWindow::appearanceChanged, this, [this] {
        QTimer::singleShot(0, this, [this] {
            for (int index = 0; index < m_sessionTabs->count(); ++index)
                showTabActivity(index);
        });
    });
    m_sessionTabs->setTabsClosable(true);
    m_sessionTabs->setDocumentMode(true);
    // Session tabs can be dragged into a different order; they stay inside
    // the window (no detaching).
    m_sessionTabs->setMovable(true);
    connect(m_sessionTabs, &QTabWidget::tabCloseRequested,
            this, &MainWindow::closeSessionTab);
    connect(m_sessionTabs, &QTabWidget::currentChanged,
            this, &MainWindow::showSftpPanelForCurrentTab);
    // The current tab is no longer marked as having new output.
    connect(m_sessionTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index >= 0)
            clearTabActivity(index);
    });
    m_sessionTabs->tabBar()->installEventFilter(this);
    m_sessionTabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sessionTabs->tabBar(), &QWidget::customContextMenuRequested,
            this, &MainWindow::showTabContextMenu);

    splitter->addWidget(m_leftTabs);
    splitter->addWidget(m_sessionTabs);
    // A double-click on the panel's edge fits the panel to what it shows.
    splitter->handle(1)->installEventFilter(this);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({300, 800});
    connect(m_sessionTree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item) {
                if (item && !isFolder(item))
                    openSelectedSession();
            });
    connect(m_sessionTree, &QTreeWidget::customContextMenuRequested,
            this, &MainWindow::showTreeContextMenu);

    createMenus();
    createToolbar();
    loadSessions();
    if (m_sessionList.foldersAtStart == QStringLiteral("remember")) {
        restoreFolderExpansion();
    } else {
        // All open or all closed; saved, so the state is remembered (a search
        // puts the folders back as they were) from here on.
        const bool open = m_sessionList.foldersAtStart == QStringLiteral("expanded");
        for (QTreeWidgetItemIterator it(m_sessionTree); *it; ++it) {
            if (isFolder(*it))
                (*it)->setExpanded(open);
        }
        saveFolderExpansion();
    }
    refreshSessionList();
    applySessionListPreferences();
    m_tunnelsWindow = new TunnelsWindow(
        [this](const SavedTunnel &tunnel, SshConnectionSettings *settings) {
            return resolveTunnel(tunnel, settings);
        },
        [this] { return jumpHostChoices(); }, [this](QWidget *over) { return createSshSession(over); },
        [this](const QString &server, const QStringList &via) { return describeTunnelServer(server, via); },
        [this](const QString &sessionId) { return openTabConnection(sessionId); },
        [this](const QString &host) {
            if (!m_networkTools)
                m_networkTools = new NetworkToolsWindow(this);
            m_networkTools->showPortScan(host);
        },
        this);
    // Once the main window is up, so prompts have a parent to show over.
    QTimer::singleShot(0, m_tunnelsWindow, [this] { m_tunnelsWindow->startAutomatic(); });
    connect(m_sessionTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *) {
        if (!m_filtering)
            saveFolderExpansion();
        fitSessionPanel();
    });
    connect(m_sessionTree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *) {
        if (!m_filtering)
            saveFolderExpansion();
        fitSessionPanel();
    });

    if (m_startup.rememberWindow) {
        const auto geometry = loadWindowGeometry();
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
    }
    // The first fit happens while building the widget tree, before the
    // splitter has its final size. Fit again after the restored window shows.
    QTimer::singleShot(0, this, &MainWindow::fitSessionPanel);
    QTimer::singleShot(0, this, &MainWindow::rememberStartLook);
    // After the window is shown, so a vault prompt has a parent on screen.
    QTimer::singleShot(0, this, &MainWindow::restoreStartupTabs);

    // Auto-lock: any keyboard or mouse input in the application counts as activity.
    m_lastInput.start();
    qApp->installEventFilter(this);
    auto *idleCheck = new QTimer(this);
    connect(idleCheck, &QTimer::timeout, this, [this] {
        if (m_security.autoLockMinutes > 0 && m_vault.isUnlocked()
            && m_lastInput.elapsed() > qint64(m_security.autoLockMinutes) * 60000)
            lockVaultFor(QStringLiteral("after %1 min without activity").arg(m_security.autoLockMinutes));
    });
    idleCheck->start(15000);

    // Preferences takes most of a second to build, the first time: done
    // while nobody uses the window (no input for 2 s, no dialog or menu
    // open), so opening it later is instant.
    auto *prepare = new QTimer(this);
    connect(prepare, &QTimer::timeout, this, [this, prepare] {
        if (m_preferencesWindow) {
            prepare->deleteLater();
            return;
        }
        if (m_lastInput.elapsed() < 2000 || QApplication::activeModalWidget()
            || QApplication::activePopupWidget())
            return;
        m_preferencesWindow = new PreferencesWindow(this);
        prepare->deleteLater();
    });
    QTimer::singleShot(5000, prepare, [prepare] { prepare->start(1000); });

    m_broadcastIndicator = new QLabel(QStringLiteral("  BROADCAST  "), this);
    m_broadcastIndicator->setStyleSheet(QStringLiteral(
        "background: #c0392b; color: white; font-weight: bold; border-radius: 3px;"));
    m_broadcastIndicator->setToolTip(QStringLiteral("Typing goes to every connected SSH tab"));
    m_broadcastIndicator->hide();
    statusBar()->addPermanentWidget(m_broadcastIndicator);
    m_multiExecIndicator = new QToolButton(this);
    m_multiExecIndicator->setAutoRaise(true);
    m_multiExecIndicator->setStyleSheet(QStringLiteral(
        "QToolButton { background: palette(highlight); color: palette(highlighted-text); "
        "font-weight: bold; border-radius: 3px; padding: 0 6px; }"));
    m_multiExecIndicator->setToolTip(QStringLiteral("Typing into a chosen terminal goes to all chosen "
                                                    "terminals. Click to stop."));
    m_multiExecIndicator->hide();
    connect(m_multiExecIndicator, &QToolButton::clicked, this, [this] {
        for (auto *page : allTerminalPages())
            page->setMultiExecTarget(false);
        updateMultiExecIndicator();
    });
    statusBar()->addPermanentWidget(m_multiExecIndicator);
    m_messageLog = new MessageLog(this);
    m_messagesIndicator = new MessagesIndicator(m_messageLog, this);
    statusBar()->addPermanentWidget(m_messagesIndicator->button());
    m_transfersIndicator = new StatusIndicator(themedIcon(QStringLiteral("network-transmit-receive"), QStyle::SP_BrowserReload),
                                               QStringLiteral("Transfers · 88 · 88%"), this);
    statusBar()->addPermanentWidget(m_transfersIndicator);
    connect(m_transfersIndicator, &QAbstractButton::clicked, this, [this] {
        showTransferList(m_transfersIndicator, {}, [this](QWidget *owner) { return transferPlace(owner); }, false);
    });
    connect(TransferList::instance(), &TransferList::changed, this, &MainWindow::updateTransfers);
    updateTransfers();

    if (firstStart) {
        for (const auto &look : builtInLooks()) {
            if (look.id == QStringLiteral("builtin:opsremote-light-orange"))
                applyLook(look);
        }
    }
}

void MainWindow::createMenus()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto *localTerminalAction = fileMenu->addAction(QStringLiteral("New Local Terminal"));
    setThemedIcon(localTerminalAction, QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    bindShortcut(localTerminalAction, QStringLiteral("new_local_terminal"));
    connect(localTerminalAction, &QAction::triggered, this, &MainWindow::openLocalTerminal);
    disableLocalTerminalWhereUnsupported(localTerminalAction);
    // Windows: through UAC; Linux: sudo or pkexec (see administratorTerminal()).
    auto *administratorAction = fileMenu->addAction(QStringLiteral("New Local Terminal (Administrator)"));
    administratorAction->setIcon(style()->standardIcon(QStyle::SP_VistaShield));
    connect(administratorAction, &QAction::triggered, this, [this] { openLocalTerminalAsAdministrator(); });
    disableLocalTerminalWhereUnsupported(administratorAction);
    auto *newSessionAction = fileMenu->addAction(QStringLiteral("New Session…"));
    setThemedIcon(newSessionAction, QStringLiteral("document-new"), QStyle::SP_FileIcon);
    bindShortcut(newSessionAction, QStringLiteral("new_session"));
    connect(newSessionAction, &QAction::triggered, this, &MainWindow::createSession);
    fileMenu->addSeparator();
    auto *exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    setThemedIcon(exitAction, QStringLiteral("application-exit"), QStyle::SP_DialogCloseButton);
    bindShortcut(exitAction, QStringLiteral("quit"));
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    auto *sessionsMenu = menuBar()->addMenu(QStringLiteral("&Sessions"));
    auto *newFolderAction = sessionsMenu->addAction(QStringLiteral("New Folder…"));
    setThemedIcon(newFolderAction, QStringLiteral("folder-new"), QStyle::SP_FileDialogNewFolder);
    connect(newFolderAction, &QAction::triggered, this, &MainWindow::createFolder);
    auto *deleteFolderAction = sessionsMenu->addAction(QStringLiteral("Delete…"));
    setThemedIcon(deleteFolderAction, QStringLiteral("edit-delete"), QStyle::SP_TrashIcon);
    bindShortcut(deleteFolderAction, QStringLiteral("delete_folder"));
    connect(deleteFolderAction, &QAction::triggered, this, &MainWindow::deleteSelectedItem);
    sessionsMenu->addAction(QStringLiteral("Folder Settings…"), this, [this] {
        editFolderSettings(selectedFolder());
    });
    auto *searchAction = sessionsMenu->addAction(QStringLiteral("Search Sessions"));
    setThemedIcon(searchAction, QStringLiteral("edit-find"), QStyle::SP_FileDialogContentsView);
    bindShortcut(searchAction, QStringLiteral("search_sessions"));
    connect(searchAction, &QAction::triggered, this, &MainWindow::focusSessionSearch);
    auto *tunnelsAction = sessionsMenu->addAction(QStringLiteral("SSH Tunnels…"));
    setThemedIcon(tunnelsAction, QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    connect(tunnelsAction, &QAction::triggered, this, &MainWindow::showTunnels);
    auto *addSessionAction = sessionsMenu->addAction(QStringLiteral("New Session…"));
    setThemedIcon(addSessionAction, QStringLiteral("list-add"), QStyle::SP_FileIcon);
    connect(addSessionAction, &QAction::triggered, this, &MainWindow::createSession);
    auto *editSessionAction = sessionsMenu->addAction(QStringLiteral("Edit…"));
    setThemedIcon(editSessionAction, QStringLiteral("edit-rename"), QStyle::SP_FileDialogDetailedView);
    bindShortcut(editSessionAction, QStringLiteral("edit_session"));
    connect(editSessionAction, &QAction::triggered, this, &MainWindow::editSelectedSession);
    sessionsMenu->addSeparator();
    auto *openAction = sessionsMenu->addAction(QStringLiteral("Open"));
    setThemedIcon(openAction, QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay);
    bindShortcut(openAction, QStringLiteral("open_session"));
    connect(openAction, &QAction::triggered, this, &MainWindow::openSelectedSession);
    // Rename (F2 in the session list) and Duplicate act on the selection.
    sessionsMenu->addAction(QStringLiteral("Rename"), this, [this] {
        if (auto *item = selectedItem())
            m_sessionTree->editItem(item, 0);
    });
    auto *duplicateAction = sessionsMenu->addAction(QStringLiteral("Duplicate"), this, [this] {
        duplicateSession(selectedItem());
    });
    connect(sessionsMenu, &QMenu::aboutToShow, this, [this, duplicateAction] {
        auto *item = selectedItem();
        duplicateAction->setEnabled(item && !isFolder(item));
    });
    auto *snippetsMenu = sessionsMenu->addMenu(QStringLiteral("Send Snippet"));
    snippetsMenu->setIcon(themedIcon(QStringLiteral("insert-text"), QStyle::SP_FileDialogContentsView));
    connect(snippetsMenu, &QMenu::aboutToShow, this, [this, snippetsMenu] {
        snippetsMenu->clear();
        auto *page = activeTerminal(m_sessionTabs->currentWidget());
        for (const auto &snippet : std::as_const(m_snippets)) {
            auto *action = snippetsMenu->addAction(snippet.name, this, [page, snippet] {
                if (page)
                    page->typeText(snippet.text + (snippet.pressEnter ? QStringLiteral("\r") : QString()));
            });
            action->setEnabled(page);
        }
        if (!m_snippets.isEmpty())
            snippetsMenu->addSeparator();
        snippetsMenu->addAction(QStringLiteral("Edit Commands…"), this, [this] {
            m_leftTabs->show();
            m_leftTabs->setTabVisible(m_leftTabs->indexOf(m_commandsPanel), true);
            m_leftTabs->setCurrentWidget(m_commandsPanel);
        });
    });
    sessionsMenu->addSeparator();
    auto *importAction = sessionsMenu->addAction(QStringLiteral("Import from YAML…"));
    setThemedIcon(importAction, QStringLiteral("document-import"), QStyle::SP_DialogOpenButton);
    connect(importAction, &QAction::triggered, this, &MainWindow::importSessions);
    // Sessions saved in other programs, each in a folder of its own.
    auto *importFrom = sessionsMenu->addMenu(QStringLiteral("Import From"));
    importFrom->addAction(QStringLiteral("OpenSSH Config (~/.ssh/config)…"), this, [this] {
        const auto path = FileDialogs::openFileName(
            this, QStringLiteral("Import OpenSSH Config"), SessionImport::defaultOpenSshConfigPath());
        if (!path.isEmpty())
            importResult(QStringLiteral("OpenSSH"), SessionImport::fromOpenSshConfig(path));
    });
    importFrom->addAction(QStringLiteral("PuTTY"), this, [this] {
        importResult(QStringLiteral("PuTTY"), SessionImport::fromPutty());
    });
    importFrom->addAction(QStringLiteral("MobaXterm (.mxtsessions, MobaXterm.ini)…"), this, [this] {
        const auto path = FileDialogs::openFileName(
            this, QStringLiteral("Import MobaXterm Sessions"), QString(),
            QStringLiteral("MobaXterm sessions (*.mxtsessions *.ini);;All files (*)"));
        if (!path.isEmpty())
            importResult(QStringLiteral("MobaXterm"), SessionImport::fromMobaXterm(path));
    });
    importFrom->addAction(QStringLiteral("mRemoteNG (confCons.xml)…"), this, [this] {
        const auto path = FileDialogs::openFileName(
            this, QStringLiteral("Import mRemoteNG Connections"), QString(),
            QStringLiteral("mRemoteNG connections (*.xml);;All files (*)"));
        if (!path.isEmpty())
            importResult(QStringLiteral("mRemoteNG"), SessionImport::fromMRemoteNg(path));
    });
    importFrom->addAction(QStringLiteral("Remmina…"), this, [this] {
        const auto folder = FileDialogs::existingDirectory(
            this, QStringLiteral("Remmina Connections Folder"), SessionImport::defaultRemminaFolder());
        if (!folder.isEmpty())
            importResult(QStringLiteral("Remmina"), SessionImport::fromRemmina(folder));
    });
    auto *exportAction = sessionsMenu->addAction(QStringLiteral("Export to YAML…"));
    setThemedIcon(exportAction, QStringLiteral("document-export"), QStyle::SP_DialogSaveButton);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportSessions);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    auto *sessionsPanelAction = viewMenu->addAction(QStringLiteral("Sessions and Files Panel"));
    sessionsPanelAction->setCheckable(true);
    sessionsPanelAction->setChecked(true);
    connect(sessionsPanelAction, &QAction::toggled, m_leftTabs, &QWidget::setVisible);
    bindShortcut(sessionsPanelAction, QStringLiteral("toggle_side_panel"));
    auto *fullScreenAction = viewMenu->addAction(QStringLiteral("Full Screen"));
    fullScreenAction->setCheckable(true);
    setThemedIcon(fullScreenAction, QStringLiteral("view-fullscreen"), QStyle::SP_TitleBarMaxButton);
    bindShortcut(fullScreenAction, QStringLiteral("full_screen"));
    m_presentationAction = viewMenu->addAction(QStringLiteral("Presentation Mode"));
    m_presentationAction->setCheckable(true);
    setThemedIcon(m_presentationAction, QStringLiteral("video-display"), QStyle::SP_DesktopIcon);
    bindShortcut(m_presentationAction, QStringLiteral("presentation"));
    connect(m_presentationAction, &QAction::toggled, this, &MainWindow::setPresentationMode);
    m_broadcastAction = viewMenu->addAction(QStringLiteral("Broadcast Typing to All SSH Tabs"));
    m_broadcastAction->setCheckable(true);
    setThemedIcon(m_broadcastAction, QStringLiteral("network-transmit"), QStyle::SP_ArrowRight);
    bindShortcut(m_broadcastAction, QStringLiteral("broadcast"));
    connect(m_broadcastAction, &QAction::toggled, this, &MainWindow::setBroadcast);
    viewMenu->addSeparator();
    // The side panel's optional tabs (Sessions always shows).
    const auto panel = loadCommandsPreferences();
    for (const auto &[label, page, shown] : {std::tuple{QStringLiteral("Files Tab"), static_cast<QWidget *>(m_sftpStack), panel.showFiles},
                                             std::tuple{QStringLiteral("Commands Tab"), static_cast<QWidget *>(m_commandsPanel), panel.showCommands},
                                             std::tuple{QStringLiteral("Scripts Tab"), static_cast<QWidget *>(m_scriptsPanel), panel.showScripts}}) {
        auto *action = viewMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(shown);
        m_leftTabs->setTabVisible(m_leftTabs->indexOf(page), shown);
        connect(action, &QAction::toggled, this, [this, page](bool on) {
            m_leftTabs->setTabVisible(m_leftTabs->indexOf(page), on);
            auto preferences = loadCommandsPreferences();
            if (page == m_sftpStack)
                preferences.showFiles = on;
            else if (page == m_commandsPanel)
                preferences.showCommands = on;
            else
                preferences.showScripts = on;
            saveCommandsPreferences(preferences);
        });
    }
    auto *desktopFullScreen = viewMenu->addAction(QStringLiteral("Remote Desktop Full Screen"));
    bindShortcut(desktopFullScreen, QStringLiteral("remote_full_screen"));
    desktopFullScreen->setToolTip(QStringLiteral("Shows the current remote desktop tab on the whole "
                                                 "screen (or every monitor, if its options say so)."));
    connect(desktopFullScreen, &QAction::triggered, this, [this] {
        if (auto *screen = qobject_cast<RemoteScreenPage *>(m_sessionTabs->currentWidget()))
            screen->setFullScreen(true);
    });
    connect(viewMenu, &QMenu::aboutToShow, this, [this, desktopFullScreen] {
        desktopFullScreen->setEnabled(qobject_cast<RemoteScreenPage *>(m_sessionTabs->currentWidget()));
    });
    connect(fullScreenAction, &QAction::toggled, this, [this](bool on) {
        setWindowState(on ? windowState() | Qt::WindowFullScreen
                          : windowState() & ~Qt::WindowFullScreen);
    });

    // Tab actions live on the window so their shortcuts work from any tab.
    // The Tabs menu: its actions also work by shortcut anywhere in the window.
    auto *tabsMenu = menuBar()->addMenu(QStringLiteral("T&abs"));
    auto addTabAction = [this, tabsMenu](const QString &text, const QString &id, auto slot) {
        auto *action = new QAction(text, this);
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
        tabsMenu->addAction(action);
        bindShortcut(action, id);
        connect(action, &QAction::triggered, this, slot);
    };
    addTabAction(QStringLiteral("Next Tab"), QStringLiteral("next_tab"), [this] {
        if (m_sessionTabs->count() > 1)
            m_sessionTabs->setCurrentIndex((m_sessionTabs->currentIndex() + 1) % m_sessionTabs->count());
    });
    addTabAction(QStringLiteral("Previous Tab"), QStringLiteral("previous_tab"), [this] {
        if (m_sessionTabs->count() > 1)
            m_sessionTabs->setCurrentIndex((m_sessionTabs->currentIndex() + m_sessionTabs->count() - 1)
                                           % m_sessionTabs->count());
    });
    // In a split tab, the active pane closes first (as in Terminator).
    addTabAction(QStringLiteral("Close Tab or Pane"), QStringLiteral("close_tab"), [this] {
        auto *split = qobject_cast<SplitTab *>(m_sessionTabs->currentWidget());
        if (split && split->activePane())
            closePane(split->activePane());
        else if (m_sessionTabs->currentIndex() >= 0)
            closeSessionTab(m_sessionTabs->currentIndex());
    });
    addTabAction(QStringLiteral("Multi-exec…"), QStringLiteral("multi_exec"),
                 [this] { chooseMultiExecTargets(); });
    // The plain splits open the same session at once; "With…" asks.
    addTabAction(QStringLiteral("Split Right"), QStringLiteral("split_right"),
                 [this] { splitActivePane(Qt::Horizontal, QStringLiteral("same")); });
    addTabAction(QStringLiteral("Split Down"), QStringLiteral("split_down"),
                 [this] { splitActivePane(Qt::Vertical, QStringLiteral("same")); });
    tabsMenu->addAction(QStringLiteral("Split Right With…"), this,
                        [this] { splitActivePane(Qt::Horizontal, QStringLiteral("choose")); });
    tabsMenu->addAction(QStringLiteral("Split Down With…"), this,
                        [this] { splitActivePane(Qt::Vertical, QStringLiteral("choose")); });
    addTabAction(QStringLiteral("Duplicate Tab"), QStringLiteral("duplicate_tab"), [this] {
        if (m_sessionTabs->currentIndex() >= 0)
            duplicateTab(m_sessionTabs->currentIndex());
    });

    // Looks: quick switching here; every setting is in Preferences → Appearance.
    auto *lookMenu = viewMenu->addMenu(QStringLiteral("Look"));
    connect(lookMenu, &QMenu::aboutToShow, this, [this, lookMenu] {
        lookMenu->clear();
        auto *group = new QActionGroup(lookMenu);
        const QString current = matchingLookId();
        bool separated = false;
        for (const auto &look : looks()) {
            if (!look.isBuiltIn() && !separated) {
                lookMenu->addSeparator();
                separated = true;
            }
            auto *action = lookMenu->addAction(look.name, this, [this, look] { applyLook(look); });
            action->setCheckable(true);
            action->setChecked(look.id == current);
            group->addAction(action);
        }
        lookMenu->addSeparator();
        auto *custom = lookMenu->addAction(QStringLiteral("Custom"));
        custom->setCheckable(true);
        custom->setChecked(current.isEmpty());
        custom->setEnabled(false);
    });
    viewMenu->addAction(QStringLiteral("Appearance…"), this,
                        [this] { showPreferences(QStringLiteral("appearance")); });

    auto *toolsMenu = menuBar()->addMenu(QStringLiteral("T&ools"));
    auto *tunnelsTool = toolsMenu->addAction(QStringLiteral("SSH Tunnels…"));
    setThemedIcon(tunnelsTool, QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    connect(tunnelsTool, &QAction::triggered, this, &MainWindow::showTunnels);
    auto *runCommand = toolsMenu->addAction(QStringLiteral("Run Command on Hosts…"));
    runCommand->setToolTip(QStringLiteral("On the SSH sessions of the selected folder, or on all of them"));
    setThemedIcon(runCommand, QStringLiteral("system-run"), QStyle::SP_MediaPlay);
    connect(runCommand, &QAction::triggered, this, [this] {
        auto *item = selectedItem();
        showRunCommand(item && isFolder(item) ? item : nullptr);
    });
    auto *scriptsTool = toolsMenu->addAction(QStringLiteral("Scripts…"));
    scriptsTool->setToolTip(QStringLiteral("Saved scripts to run in a tab or on many hosts"));
    setThemedIcon(scriptsTool, QStringLiteral("text-x-script"), QStyle::SP_FileIcon);
    connect(scriptsTool, &QAction::triggered, this, [this] { showScripts(); });
    auto *kubeTool = toolsMenu->addAction(QStringLiteral("Kubernetes Credentials…"));
    kubeTool->setToolTip(QStringLiteral("Saved kubeconfigs for Container (Kubernetes) sessions"));
    setThemedIcon(kubeTool, QStringLiteral("dialog-password"), QStyle::SP_DialogApplyButton);
    connect(kubeTool, &QAction::triggered, this, [this] { showKubeCredentials(this); });
    auto *keyManager = toolsMenu->addAction(QStringLiteral("SSH Keys…"));
    setThemedIcon(keyManager, QStringLiteral("dialog-password"), QStyle::SP_DialogApplyButton);
    connect(keyManager, &QAction::triggered, this, &MainWindow::showKeyManager);
    auto *localServers = toolsMenu->addAction(QStringLiteral("Local Servers (TFTP, HTTP)…"));
    setThemedIcon(localServers, QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    connect(localServers, &QAction::triggered, this, [this] {
        // Made when first needed; its servers run only while it is open.
        if (!m_localServersWindow)
            m_localServersWindow = new LocalServersWindow(this);
        m_localServersWindow->show();
        m_localServersWindow->raise();
        m_localServersWindow->activateWindow();
    });
    auto *networkTools = toolsMenu->addAction(QStringLiteral("Network Tools…"));
    setThemedIcon(networkTools, QStringLiteral("network-wired"), QStyle::SP_DriveNetIcon);
    connect(networkTools, &QAction::triggered, this, &MainWindow::showNetworkTools);
    toolsMenu->addSeparator();
    auto *playRecording = toolsMenu->addAction(QStringLiteral("Play Recording…"));
    setThemedIcon(playRecording, QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay);
    connect(playRecording, &QAction::triggered, this, [this] {
        const auto folder = sessionLogFolder(m_logging);
        const auto path = FileDialogs::openFileName(
            this, QStringLiteral("Play Recording"), folder,
            QStringLiteral("asciinema recordings (*.cast);;All files (*)"));
        if (path.isEmpty())
            return;
        auto *player = new CastPlayer(this);
        QString error;
        if (!player->load(path, &error)) {
            delete player;
            QMessageBox::warning(this, QStringLiteral("Play Recording"), error);
            return;
        }
        player->show();
    });

    auto *settingsMenu = menuBar()->addMenu(QStringLiteral("Se&ttings"));
    // One item that locks or unlocks (or creates) the vault, depending on its state.
    auto *vaultAction = settingsMenu->addAction(QString());
    connect(vaultAction, &QAction::triggered, this, [this] {
        if (m_vault.isUnlocked()) {
            lockVaultFor(QString());
        } else {
            ensureVaultUnlocked();
        }
    });
    auto updateVaultAction = [this, vaultAction] {
        const bool unlocked = m_vault.isUnlocked();
        vaultAction->setText(unlocked ? QStringLiteral("Lock Vault")
                             : m_vault.exists() ? QStringLiteral("Unlock Vault…")
                                                : QStringLiteral("Create Credential Vault…"));
        vaultAction->setIcon(unlocked
                                 ? themedIcon(QStringLiteral("object-locked"), QStyle::SP_DialogCancelButton)
                                 : themedIcon(QStringLiteral("object-unlocked"), QStyle::SP_DialogOkButton));
    };
    connect(settingsMenu, &QMenu::aboutToShow, vaultAction, updateVaultAction);
    bindShortcut(vaultAction, QStringLiteral("lock_vault"));
    updateVaultAction();
    settingsMenu->addSeparator();
    auto *preferencesAction = settingsMenu->addAction(QStringLiteral("Preferences…"));
    setThemedIcon(preferencesAction, QStringLiteral("preferences-system"), QStyle::SP_FileDialogDetailedView);
    // QKeySequence::Preferences is only defined on macOS; Ctrl+, everywhere.
    bindShortcut(preferencesAction, QStringLiteral("preferences"));
    connect(preferencesAction, &QAction::triggered, this, [this] { showPreferences(); });

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    auto *aboutAction = helpMenu->addAction(QStringLiteral("About OpsRemote"));
    setThemedIcon(aboutAction, QStringLiteral("help-about"), QStyle::SP_MessageBoxInformation);
    connect(aboutAction, &QAction::triggered, this, [this] {
        showPreferences(QStringLiteral("about"));
    });
}

void MainWindow::createToolbar()
{
    auto *toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);

    auto *newSessionAction = toolbar->addAction(QStringLiteral("New Session"));
    setThemedIcon(newSessionAction, QStringLiteral("document-new"), QStyle::SP_ComputerIcon);
    connect(newSessionAction, &QAction::triggered, this, &MainWindow::createSession);

    auto *newFolderAction = toolbar->addAction(QStringLiteral("New Folder"));
    setThemedIcon(newFolderAction, QStringLiteral("folder-new"), QStyle::SP_FileDialogNewFolder);
    connect(newFolderAction, &QAction::triggered, this, &MainWindow::createFolder);

    toolbar->addSeparator();

    auto *openAction = toolbar->addAction(QStringLiteral("Open Selected"));
    setThemedIcon(openAction, QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay);
    auto *localTerminalAction = toolbar->addAction(QStringLiteral("New Local Terminal"));
    setThemedIcon(localTerminalAction, QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    connect(localTerminalAction, &QAction::triggered, this, &MainWindow::openLocalTerminal);
    disableLocalTerminalWhereUnsupported(localTerminalAction);
    connect(openAction, &QAction::triggered, this, &MainWindow::openSelectedSession);

    toolbar->addSeparator();
    auto *tunnelsAction = toolbar->addAction(QStringLiteral("SSH Tunnels"));
    setThemedIcon(tunnelsAction, QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    connect(tunnelsAction, &QAction::triggered, this, &MainWindow::showTunnels);
    auto *networkToolsAction = toolbar->addAction(QStringLiteral("Network Tools"));
    setThemedIcon(networkToolsAction, QStringLiteral("network-wired"), QStyle::SP_ComputerIcon);
    connect(networkToolsAction, &QAction::triggered, this, &MainWindow::showNetworkTools);

    // Quick connect: [protocol://][user@]host[:port], with a history.
    toolbar->addSeparator();
    auto *quickConnect = new QComboBox(toolbar);
    quickConnect->setObjectName(QStringLiteral("quickConnect"));
    quickConnect->setEditable(true);
    quickConnect->setInsertPolicy(QComboBox::NoInsert);
    quickConnect->setMinimumContentsLength(24);
    quickConnect->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    quickConnect->lineEdit()->setPlaceholderText(QStringLiteral("Quick connect: user@host, rdp://host"));
    quickConnect->lineEdit()->setClearButtonEnabled(true);
    quickConnect->setToolTip(QStringLiteral(
        "Type [ssh|telnet|mosh|rdp|vnc|sftp|scp|ftp|ftps|ftpes://][user@]host[:port], or serial://COM3:9600, and press "
        "Enter. Without a protocol it is SSH."));
    quickConnect->addItems(QuickConnect::history());
    quickConnect->setCurrentIndex(-1);
    toolbar->addWidget(quickConnect);
    connect(quickConnect->lineEdit(), &QLineEdit::returnPressed, this, [this, quickConnect] {
        const QString target = quickConnect->currentText().trimmed();
        QString error;
        if (!openTarget(target, &error)) {
            notify(error, true);
            return;
        }
        const QSignalBlocker blocker(quickConnect);
        quickConnect->clear();
        quickConnect->addItems(QuickConnect::history());
        quickConnect->setCurrentIndex(-1);
        quickConnect->clearEditText();
    });
}

bool MainWindow::openTarget(const QString &target, QString *error)
{
    const auto session = QuickConnect::parse(target, error);
    if (!session)
        return false;
    QuickConnect::remember(target);
    openSession(*session);
    return true;
}

bool MainWindow::openSessionByPath(const QString &path, QString *error)
{
    const QString wanted = path.trimmed();
    const bool byPath = wanted.contains(QLatin1Char('/'));
    QStringList matches;
    std::function<void(QTreeWidgetItem *, const QString &)> walk =
        [&](QTreeWidgetItem *item, const QString &prefix) {
            for (int index = 0; index < item->childCount(); ++index) {
                auto *child = item->child(index);
                if (isFolder(child)) {
                    walk(child, prefix + child->text(0) + QLatin1Char('/'));
                    continue;
                }
                const QString id = child->data(0, SessionIdRole).toString();
                const QString name = m_sessions.value(id).name;
                if (name.compare(byPath ? wanted.section(QLatin1Char('/'), -1) : wanted,
                                 Qt::CaseInsensitive) == 0
                    && (!byPath || (prefix + name).compare(wanted, Qt::CaseInsensitive) == 0))
                    matches.append(id);
            }
        };
    walk(m_sessionTree->invisibleRootItem(), QString());
    if (matches.size() != 1) {
        if (error)
            *error = matches.isEmpty()
                         ? QStringLiteral("No saved session is called \"%1\".").arg(wanted)
                         : QStringLiteral("Several sessions are called \"%1\"; use Folder/Name.")
                               .arg(wanted);
        return false;
    }
    openSession(m_sessions.value(matches.first()));
    return true;
}

// OpsRemote [--session NAME | --session Folder/NAME]... [TARGET]...
// where TARGET is a quick-connect target such as ssh://user@host:22.
void MainWindow::handleCommandLine(const QStringList &arguments)
{
    QStringList problems;
    for (int index = 0; index < arguments.size(); ++index) {
        const QString &argument = arguments.at(index);
        QString error;
        if (argument == QStringLiteral("--session") || argument == QStringLiteral("-s")) {
            if (++index >= arguments.size()) {
                problems.append(QStringLiteral("--session needs a session name."));
                break;
            }
            if (!openSessionByPath(arguments.at(index), &error))
                problems.append(error);
        } else if (argument.startsWith(QStringLiteral("--session="))) {
            if (!openSessionByPath(argument.mid(10), &error))
                problems.append(error);
        } else if (argument.startsWith(QLatin1Char('-'))) {
            // Qt's own options (-style, -platform) are taken out before this.
            problems.append(QStringLiteral("Unknown option %1.").arg(argument));
        } else if (!openTarget(argument, &error)) {
            problems.append(error);
        }
    }
    if (!problems.isEmpty())
        QMessageBox::warning(this, QStringLiteral("OpsRemote"), problems.join(QLatin1Char('\n')));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QStringList connected;
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        if (tabIsConnected(m_sessionTabs->widget(index)))
            connected.append(m_sessionTabs->tabText(index));
    }
    if (m_startup.confirmExit && !connected.isEmpty()) {
        QMessageBox box(QMessageBox::Question, QStringLiteral("Exit OpsRemote"),
                        connected.size() == 1
                            ? QStringLiteral("1 session is still open. Exit anyway?")
                            : QStringLiteral("%1 sessions are still open. Exit anyway?")
                                  .arg(connected.size()),
                        QMessageBox::Yes | QMessageBox::No, this);
        box.setInformativeText(QStringLiteral("• ") + connected.join(QStringLiteral("\n• ")));
        box.setDefaultButton(QMessageBox::No);
        if (box.exec() != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    saveOpenSessions(openSessionIds());
    if (m_startup.rememberWindow)
        saveWindowGeometry(saveGeometry());
    saveFolderExpansion();
    QMainWindow::closeEvent(event);
    // Other top-level windows (Preferences, dialogs) must not keep the
    // application running once the main window is closed.
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

void MainWindow::syncMenuChecks()
{
    if (m_styleGroup) {
        for (auto *action : m_styleGroup->actions())
            action->setChecked(action->data().toString().compare(m_selectedStyleKey, Qt::CaseInsensitive) == 0);
    }
    if (m_colorThemeGroup) {
        for (auto *action : m_colorThemeGroup->actions())
            action->setChecked(action->data().toString() == m_colorThemeKey);
    }
    if (m_iconThemeGroup) {
        for (auto *action : m_iconThemeGroup->actions())
            action->setChecked(action->data().toString() == m_iconTheme);
    }
}

void MainWindow::showPreferences(const QString &page)
{
    if (!m_preferencesWindow)
        m_preferencesWindow = new PreferencesWindow(this);
    m_preferencesWindow->showPage(page);
}

// ---- Tabs, startup, and shortcuts ----

void MainWindow::bindShortcut(QAction *action, const QString &id)
{
    m_shortcutActions.insert(id, action);
    action->setShortcut(m_shortcuts.value(id));
}

void MainWindow::setShortcuts(const QHash<QString, QKeySequence> &shortcuts)
{
    m_shortcuts = shortcuts;
    saveShortcuts(shortcuts);
    RemoteScreenPage::setFullScreenKey(shortcuts.value(QStringLiteral("remote_full_screen")));
    for (auto it = m_shortcutActions.cbegin(); it != m_shortcutActions.cend(); ++it)
        it.value()->setShortcut(shortcuts.value(it.key()));
    for (auto *page : allTerminalPages())
        page->setShortcuts(shortcuts);
}

void MainWindow::setTerminalBehaviour(const TerminalBehaviour &behaviour)
{
    m_behaviour = behaviour;
    saveTerminalBehaviour(behaviour);
    for (auto *page : allTerminalPages())
        page->setBehaviour(behaviour);
    // Every tab takes the new style and colours.
    for (int index = 0; index < m_sessionTabs->count(); ++index)
        showTabActivity(index);
    emit terminalBehaviourChanged();
}

void MainWindow::setMouseClipboardPreferences(const MouseClipboardPreferences &preferences)
{
    saveMouseClipboardPreferences(preferences);
    Clipboard::setPreferences(preferences);
}

void MainWindow::setStartupPreferences(const StartupPreferences &preferences)
{
    m_startup = preferences;
    saveStartupPreferences(preferences);
    m_paneBars->setPlace(m_startup.messageStrip, m_startup.messageBar);
    applyMoveSettings();
    TabPrompt::setInStrip(m_paneBars->shown());
    updatePaneBars();
}

void MainWindow::applyMoveSettings()
{
    m_paneBars->setFixedParts(m_startup.paneBarParts == QStringLiteral("fixed"));
    m_paneBars->setMoveStyle(m_startup.moveDragBy == QStringLiteral("handle"),
                             m_startup.moveHandleClick == QStringLiteral("menu"));
    m_paneDrag->pullTabs = m_startup.moveTerminals && m_startup.movePullTabs;
    m_paneDrag->setOpenTab(m_startup.moveOpenTab ? m_startup.moveOpenTabMs : -1);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
        m_lastInput.restart();
        break;
    default:
        break;
    }
    // The side panel's tab bar, where the tab widget puts it, moved in to
    // start where the session list's box does (and no wider than the room).
    if ((event->type() == QEvent::Move || event->type() == QEvent::Resize) && m_leftTabs
        && watched == m_leftTabs->tabBar() && m_sidePanelInset > 0) {
        auto *bar = m_leftTabs->tabBar();
        if (bar->x() < m_sidePanelInset) {
            const int right = qMin(bar->geometry().right(), m_leftTabs->width() - 1);
            bar->setGeometry(m_sidePanelInset, bar->y(), right + 1 - m_sidePanelInset, bar->height());
        }
    }
    // Its tabs changed size (look, font, a tab shown or hidden): the panel
    // stays at least as wide as all of them, whatever its fit setting.
    if (event->type() == QEvent::LayoutRequest && m_leftTabs && watched == m_leftTabs)
        updateSidePanelMinimum();
    if (event->type() == QEvent::MouseButtonDblClick && m_leftTabs
        && watched == qobject_cast<QSplitter *>(m_leftTabs->parentWidget())->handle(1)) {
        fitSidePanelToContents();
        return true;
    }
    // Drop-down lists and number fields, in every window: the wheel changes
    // them only as Preferences → Workspace → Mouse says; otherwise it goes
    // on to what holds them, so the page scrolls instead.
    if (event->type() == QEvent::Polish || event->type() == QEvent::Wheel) {
        // The field itself, or a part of it (a spin box's text, a marked
        // widget's children such as the terminal preview's display).
        const auto isField = [](const QWidget *widget) {
            return qobject_cast<const QComboBox *>(widget) || qobject_cast<const QAbstractSpinBox *>(widget)
                   || widget->property("opsremoteWheelField").toBool();
        };
        QWidget *field = qobject_cast<QWidget *>(watched);
        // Not across windows: an open drop-down list is a popup of its combo
        // box and scrolls as usual.
        for (int depth = 0; field && !isField(field) && !field->isWindow() && depth < 3; ++depth)
            field = field->parentWidget();
        if (field && !isField(field))
            field = nullptr;
        const auto &wheelOnFields = Clipboard::preferences().wheelOnFields;
        if (field && wheelOnFields != QStringLiteral("always")) {
            // Not focused by the wheel itself, or "after clicking" would never hold.
            if (field->focusPolicy() == Qt::WheelFocus)
                field->setFocusPolicy(Qt::StrongFocus);
            if (event->type() == QEvent::Wheel
                && (wheelOnFields == QStringLiteral("never") || !field->hasFocus())) {
                if (auto *holder = field->parentWidget())
                    QCoreApplication::sendEvent(holder, event);
                return true;
            }
        }
    }
    if (watched == m_sessionSearch && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Down || key == Qt::Key_Return || key == Qt::Key_Enter) {
            if (auto *first = firstVisibleSession()) {
                m_sessionTree->setCurrentItem(first);
                if (key == Qt::Key_Down)
                    m_sessionTree->setFocus();
                else
                    openSelectedSession();
            }
            return true;
        }
        if (key == Qt::Key_Escape) {
            if (m_sessionSearch->text().isEmpty())
                m_sessionTree->setFocus();
            else
                m_sessionSearch->clear();
            return true;
        }
    }
    // Every dialog is wide enough for its title next to the window's own
    // buttons (the vault's prompt showed "Jnlock Credenti…").
    if (event->type() == QEvent::Show) {
        if (auto *dialog = qobject_cast<QDialog *>(watched);
            dialog && dialog->isWindow() && !dialog->property("opsremoteTitleFitted").toBool()) {
            dialog->setProperty("opsremoteTitleFitted", true);
            const QFontMetrics metrics(QApplication::font("QWorkspaceTitleBar"));
            // Icon, minimize/maximize/close, and margins.
            const int wanted = metrics.horizontalAdvance(dialog->windowTitle()) + 190;
            if (dialog->minimumWidth() < wanted)
                dialog->setMinimumWidth(qMin(wanted, 900));
            if (dialog->width() < wanted)
                dialog->resize(qMin(wanted, 900), dialog->height());
        }
    }
    // A search box hidden in the Preferences shows only while it is used.
    if (watched == m_sessionSearch && event->type() == QEvent::FocusOut
        && !m_sessionList.showSearch && m_sessionSearch->text().isEmpty())
        m_sessionSearch->hide();
    // Widgets watched from early in the constructor send events before the
    // tabs exist.
    if (m_sessionTabs && watched == m_sessionTabs->tabBar()
        && event->type() == QEvent::MouseButtonRelease
        && Clipboard::preferences().middleClickClosesTab) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::MiddleButton) {
            const int index = m_sessionTabs->tabBar()->tabAt(mouse->position().toPoint());
            if (index >= 0) {
                closeSessionTab(index);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange && isMinimized() && m_security.lockOnMinimize
        && m_vault.isUnlocked())
        lockVaultFor(QStringLiteral("when the window was minimized"));
    QMainWindow::changeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0, this, &MainWindow::fitSessionPanel);
}

void MainWindow::lockVaultFor(const QString &reason)
{
    m_vault.lock();
    notify(reason.isEmpty()
           ? QStringLiteral("Credential vault locked")
           : QStringLiteral("Credential vault locked %1").arg(reason));
}

void MainWindow::applyTabSettings(TerminalPage *page, const QString &logName)
{
    page->setLogging(m_logging, logName);
    page->setHighlightRules(m_highlightRules);
    if (m_presentationState.active)
        page->setZoomSteps(m_presentation.zoomSteps);
}

void MainWindow::setSecurityPreferences(const SecurityPreferences &preferences)
{
    m_security = preferences;
    saveSecurityPreferences(preferences);
}

void MainWindow::setLoggingPreferences(const LoggingPreferences &preferences)
{
    m_logging = preferences;
    saveLoggingPreferences(preferences);
    // Open tabs start or stop logging now.
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        for (auto *page : terminalPagesIn(m_sessionTabs->widget(index)))
            page->setLogging(preferences, m_sessionTabs->tabText(index));
    }
}

void MainWindow::setPresentationPreferences(const PresentationPreferences &preferences)
{
    m_presentation = preferences;
    savePresentationPreferences(preferences);
}

void MainWindow::setHighlightRules(const QList<HighlightRule> &rules)
{
    m_highlightRules = rules;
    saveHighlightRules(rules);
    for (auto *page : allTerminalPages())
        page->setHighlightRules(rules);
}

void MainWindow::setSnippets(const QList<Snippet> &snippets)
{
    m_snippets = snippets;
    saveSnippets(snippets);
    if (m_commandsPanel)
        m_commandsPanel->setSnippets(snippets);
}

// Resizable, so the whole text can be read before it is sent, and editable
// ("Edit the text"; already on with Preferences → Terminals → Start in
// editing mode): text becomes what was edited. The size is kept for the
// rest of the session.
static bool confirmMultilineSend(QWidget *parent, QString &text, int terminals, bool startEditing)
{
    static QSize lastSize(640, 420);
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Commands"));
    dialog.setSizeGripEnabled(true);
    auto *layout = new QVBoxLayout(&dialog);
    auto *question = new QLabel;
    layout->addWidget(question);
    auto *view = new QPlainTextEdit(text);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(view, 1);
    const auto count = [view, question, terminals] {
        question->setText(QStringLiteral("Send these %1 lines to %2 terminal(s)?")
                              .arg(view->toPlainText().count(QLatin1Char('\n')) + 1)
                              .arg(terminals));
    };
    count();
    QObject::connect(view, &QPlainTextEdit::textChanged, &dialog, count);
    auto *row = new QHBoxLayout;
    auto *editable = new QCheckBox(QStringLiteral("Edit the text"));
    editable->setToolTip(QStringLiteral("Change the text before it is sent"));
    row->addWidget(editable);
    row->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No);
    row->addWidget(buttons);
    layout->addLayout(row);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(editable, &QCheckBox::toggled, &dialog, [view](bool on) {
        view->setReadOnly(!on);
        if (on)
            view->setFocus();
    });
    view->setReadOnly(!startEditing);
    editable->setChecked(startEditing);
    buttons->button(QDialogButtonBox::Yes)->setDefault(true);
    if (startEditing)
        view->setFocus();
    else
        buttons->button(QDialogButtonBox::Yes)->setFocus();
    dialog.resize(lastSize);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    lastSize = dialog.size();
    if (accepted)
        text = view->toPlainText();
    return accepted && !text.isEmpty();
}

void MainWindow::sendSnippet(const Snippet &snippet, bool toMultiExec, bool withoutEnter)
{
    QList<TerminalPage *> targets;
    if (toMultiExec) {
        for (auto *page : allTerminalPages()) {
            if (page->isMultiExecTarget())
                targets << page;
        }
        if (targets.isEmpty()) {
            notify(QStringLiteral("No terminals are chosen for multi-exec (Tabs → Multi-exec…)."));
            return;
        }
    } else if (auto *page = activeTerminal(m_sessionTabs->currentWidget())) {
        targets << page;
    } else {
        notify(QStringLiteral("Open a terminal tab first."));
        return;
    }
    if (const auto script = scriptFromMarker(snippet.text); !script.isEmpty()) {
        for (auto *page : targets)
            runSavedScriptInTab(page, script);
        return;
    }
    auto body = snippet.text;
    if (body.contains(QLatin1Char('\n')) && Clipboard::preferences().confirmMultilinePaste
        && !confirmMultilineSend(this, body, targets.size(), Clipboard::preferences().editMultilinePaste))
        return;
    const auto text = body + (snippet.pressEnter && !withoutEnter ? QStringLiteral("\r") : QString());
    for (auto *page : targets) {
        if (targets.size() == 1)
            page->typeText(text);
        else
            page->sendInput(QString(text).replace(QLatin1Char('\n'), QLatin1Char('\r')).toUtf8());
    }
}

void MainWindow::setBroadcast(bool on)
{
    m_broadcast = on;
    m_broadcastIndicator->setVisible(on);
    if (m_broadcastAction->isChecked() != on)
        m_broadcastAction->setChecked(on);
}

// Larger text and nothing but terminals, for screen sharing and projectors.
void MainWindow::setPresentationMode(bool on)
{
    if (on == m_presentationState.active)
        return;
    auto *toolbar = findChild<QToolBar *>();
    if (on) {
        m_presentationState = {true, windowState(), m_leftTabs->isVisible(),
                               toolbar && toolbar->isVisible(), statusBar()->isVisible()};
        if (m_presentation.hidePanels) {
            m_leftTabs->hide();
            if (toolbar)
                toolbar->hide();
            statusBar()->hide();
        }
        if (m_presentation.fullScreen)
            setWindowState(windowState() | Qt::WindowFullScreen);
    } else {
        m_presentationState.active = false;
        m_leftTabs->setVisible(m_presentationState.sidePanel);
        if (toolbar)
            toolbar->setVisible(m_presentationState.toolbar);
        statusBar()->setVisible(m_presentationState.statusBar);
        setWindowState(m_presentationState.windowState);
    }
    for (auto *page : allTerminalPages())
        page->setZoomSteps(on ? m_presentation.zoomSteps : 0);
    if (m_presentationAction->isChecked() != on)
        m_presentationAction->setChecked(on);
}
