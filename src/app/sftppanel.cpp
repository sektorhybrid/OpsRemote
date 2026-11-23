// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sftppanel.h"
#include "transfers.h"
#include "statusindicators.h"
#include "filedialogs.h"
#include "sftpsync.h"
#include "icons.h"
#include "preferences.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QTimer>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QScrollBar>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QDrag>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>

namespace {
constexpr int PathRole = Qt::UserRole;
constexpr int IsDirectoryRole = Qt::UserRole + 1;
constexpr int SizeRole = Qt::UserRole + 2;
constexpr int ModifiedRole = Qt::UserRole + 3;
constexpr int IsSymlinkRole = Qt::UserRole + 4;
// "." (this folder) and ".." (its parent) at the top: places, not files, so
// they cannot be selected and nothing acts on them but opening and drops.
constexpr int IsPlaceRole = Qt::UserRole + 5;
// Remote paths dragged inside one Files pane (moved on the server).
const QString RemotePathsMime = QStringLiteral("application/x-opsremote-remote-paths");
constexpr int MaximumListedErrors = 15;

enum Column { NameColumn, SizeColumn, ModifiedColumn };

QString joinRemotePath(const QString &directory, const QString &name)
{
    return directory.endsWith(QLatin1Char('/')) ? directory + name
                                                : directory + QLatin1Char('/') + name;
}

QString parentRemotePath(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : path.left(slash);
}

QString formatSize(quint64 bytes)
{
    return QLocale().formattedDataSize(static_cast<qint64>(bytes));
}

QString permissionText(quint32 mode)
{
    QString text;
    for (int shift : {6, 3, 0}) {
        text += (mode >> shift) & 4 ? QLatin1Char('r') : QLatin1Char('-');
        text += (mode >> shift) & 2 ? QLatin1Char('w') : QLatin1Char('-');
        text += (mode >> shift) & 1 ? QLatin1Char('x') : QLatin1Char('-');
    }
    return text;
}

// Local files offered by the clipboard or a drag. GNOME file managers may
// only provide their own list format.
QStringList localPathsFromMime(const QMimeData *mime)
{
    QStringList paths;
    if (!mime)
        return paths;
    QList<QUrl> urls = mime->urls();
    if (urls.isEmpty() && mime->hasFormat(QStringLiteral("x-special/gnome-copied-files"))) {
        const auto lines = mime->data(QStringLiteral("x-special/gnome-copied-files")).split('\n');
        for (qsizetype index = 1; index < lines.size(); ++index) {
            if (!lines[index].trimmed().isEmpty())
                urls.append(QUrl::fromEncoded(lines[index].trimmed()));
        }
    }
    for (const auto &url : urls) {
        if (url.isLocalFile())
            paths.append(QDir::cleanPath(url.toLocalFile()));
    }
    return paths;
}

class SftpItem final : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const bool ascending = treeWidget()->header()->sortIndicatorOrder() == Qt::AscendingOrder;
        // "." then ".." stay on top in either sort order.
        const bool place = data(0, IsPlaceRole).toBool();
        const bool otherPlace = other.data(0, IsPlaceRole).toBool();
        if (place || otherPlace) {
            if (place && otherPlace)
                return (text(NameColumn) < other.text(NameColumn)) == ascending;
            return ascending ? place : otherPlace;
        }
        const bool directory = data(0, IsDirectoryRole).toBool();
        const bool otherDirectory = other.data(0, IsDirectoryRole).toBool();
        // Folders stay above files in either sort order.
        if (directory != otherDirectory) {
            return treeWidget()->header()->sortIndicatorOrder() == Qt::AscendingOrder
                ? directory
                : otherDirectory;
        }
        switch (treeWidget()->sortColumn()) {
        case SizeColumn:
            return data(0, SizeRole).toULongLong() < other.data(0, SizeRole).toULongLong();
        case ModifiedColumn:
            return data(0, ModifiedRole).toDateTime() < other.data(0, ModifiedRole).toDateTime();
        default:
            return text(NameColumn).compare(other.text(NameColumn), Qt::CaseInsensitive) < 0;
        }
    }
};
}

class SftpFileView final : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;
    using QTreeWidget::sizeHintForColumn; // for SftpPanel::contentsWidth()

    // Returns whether the key is handled; only performs it when execute is set.
    std::function<bool(int, Qt::KeyboardModifiers, bool execute)> keyHandler;
    std::function<void(const QStringList &, QTreeWidgetItem *)> dropHandler;
    // Remote paths dragged from this view onto one of its folders.
    std::function<void(const QStringList &, QTreeWidgetItem *)> moveHandler;

protected:
    bool event(QEvent *event) override
    {
        // Claim our keys before window-wide shortcuts such as Delete do.
        if (event->type() == QEvent::ShortcutOverride) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyHandler && keyHandler(keyEvent->key(), keyEvent->modifiers(), false)) {
                keyEvent->accept();
                return true;
            }
        }
        return QTreeWidget::event(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (keyHandler && keyHandler(event->key(), event->modifiers(), true)) {
            event->accept();
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }

    // Our own drag: the selected remote paths, to move them (never removed
    // from the list here; the folder is listed again once the server moved them).
    void startDrag(Qt::DropActions) override
    {
        QStringList paths;
        for (auto *item : selectedItems())
            paths.append(item->data(0, PathRole).toString());
        if (paths.isEmpty() || !moveHandler)
            return;
        auto *mime = new QMimeData;
        mime->setData(RemotePathsMime, paths.join(QLatin1Char('\n')).toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::MoveAction);
    }

    bool ownDrag(const QDropEvent *event) const
    {
        return event->source() == this && event->mimeData()->hasFormat(RemotePathsMime);
    }

    // A folder under the pointer that is not one of the dragged items.
    QTreeWidgetItem *moveTarget(const QDropEvent *event) const
    {
        auto *item = itemAt(event->position().toPoint());
        return item && item->data(0, IsDirectoryRole).toBool() && !item->isSelected() ? item : nullptr;
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (ownDrag(event) || (dropHandler && !localPathsFromMime(event->mimeData()).isEmpty()))
            event->acceptProposedAction();
        else
            event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (ownDrag(event)) {
            if (moveTarget(event)) {
                event->setDropAction(Qt::MoveAction);
                event->accept();
            } else {
                event->ignore();
            }
            return;
        }
        if (dropHandler && event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
        if (ownDrag(event)) {
            auto *target = moveTarget(event);
            if (!target || !moveHandler) {
                event->ignore();
                return;
            }
            event->setDropAction(Qt::MoveAction);
            event->accept();
            moveHandler(QString::fromUtf8(event->mimeData()->data(RemotePathsMime))
                            .split(QLatin1Char('\n'), Qt::SkipEmptyParts),
                        target);
            return;
        }
        const auto paths = localPathsFromMime(event->mimeData());
        if (!dropHandler || paths.isEmpty()) {
            event->ignore();
            return;
        }
        event->acceptProposedAction();
        dropHandler(paths, itemAt(event->position().toPoint()));
    }
};

SftpPanel::SftpPanel(QWidget *parent)
    : QWidget(parent)
{
    // Transfers waiting for others start once those have ended (queued: not
    // in the middle of the list's own change).
    connect(TransferList::instance(), &TransferList::changed, this, [this] {
        if (!m_waiting.isEmpty())
            startWaiting();
    }, Qt::QueuedConnection);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_titleLabel = new QLabel(this);
    // A style sheet rather than setFont(), so the title follows changes to
    // the application's interface font.
    m_titleLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    // The first row: as tall as the tab strip beside it (see panelRowHeight).
    m_titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_titleLabel->setFixedHeight(panelRowHeight(this));
    layout->addWidget(m_titleLabel);

    auto *navigation = new QHBoxLayout;
    navigation->setSpacing(2);
    auto addButton = [this, navigation](const char *themeIcon, QStyle::StandardPixmap fallback,
                                        const QString &tip) {
        auto *button = new QToolButton(this);
        m_buttonIcons.append({button, QString::fromLatin1(themeIcon), fallback});
        button->setToolTip(tip);
        button->setAutoRaise(true);
        navigation->addWidget(button);
        return button;
    };
    m_upButton = addButton("go-up", QStyle::SP_FileDialogToParent,
                           QStringLiteral("Parent folder (Backspace)"));
    m_homeButton = addButton("go-home", QStyle::SP_DirHomeIcon, QStringLiteral("Home folder"));
    m_refreshButton = addButton("view-refresh", QStyle::SP_BrowserReload,
                                QStringLiteral("Refresh (F5)"));
    m_syncButton = addButton("folder-sync", QStyle::SP_DialogApplyButton,
                                 QStringLiteral("Sync Folders… (like rsync: copy only what changed)"));
    connect(m_syncButton, &QToolButton::clicked, this, &SftpPanel::openSync);
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(QStringLiteral("Remote path"));
    navigation->addWidget(m_pathEdit, 1);
    layout->addLayout(navigation);
    connect(m_upButton, &QToolButton::clicked, this, &SftpPanel::goUp);
    connect(m_homeButton, &QToolButton::clicked, this, [this] { navigate(m_homePath); });
    connect(m_refreshButton, &QToolButton::clicked, this, &SftpPanel::refresh);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this] {
        const auto path = m_pathEdit->text().trimmed();
        if (!path.isEmpty())
            navigate(path);
    });

    m_messageLabel = new QLabel(this);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_messageLabel->hide();
    layout->addWidget(m_messageLabel);

    m_stack = new QStackedWidget(this);

    m_view = new SftpFileView(m_stack);
    m_view->setColumnCount(3);
    m_view->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Size"),
                             QStringLiteral("Modified")});
    // "Connecting…", "Disconnected." and the like show in the middle of the
    // empty list, which keeps its frame and columns: the panel looks the same
    // with or without files.
    auto *statusLayout = new QVBoxLayout(m_view->viewport());
    statusLayout->setContentsMargins(12, 12, 12, 12);
    m_statusLabel = new QLabel(m_view->viewport());
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    statusLayout->addWidget(m_statusLabel);
    m_statusLabel->hide();
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setAcceptDrops(true);
    m_view->viewport()->setAcceptDrops(true);
    m_view->setDragEnabled(true);
    m_view->setDragDropMode(QAbstractItemView::DragDrop);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(NameColumn, Qt::AscendingOrder);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_view->header()->setSectionResizeMode(SizeColumn, QHeaderView::ResizeToContents);
    m_view->header()->setSectionResizeMode(ModifiedColumn, QHeaderView::ResizeToContents);
    m_stack->addWidget(m_view);
    layout->addWidget(m_stack, 1);

    m_view->keyHandler = [this](int key, Qt::KeyboardModifiers modifiers, bool execute) {
        return m_ready && handleKey(key, modifiers, execute);
    };
    m_view->dropHandler = [this](const QStringList &paths, QTreeWidgetItem *target) {
        if (!m_ready)
            return;
        const bool intoFolder = target && target->data(0, IsDirectoryRole).toBool();
        uploadPaths(paths, intoFolder ? target->data(0, PathRole).toString() : m_currentPath);
    };
    m_view->moveHandler = [this](const QStringList &paths, QTreeWidgetItem *target) {
        if (m_ready)
            movePaths(paths, target->data(0, PathRole).toString());
    };
    connect(m_view, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (!item)
            return;
        if (item->data(0, IsPlaceRole).toBool() && item->text(NameColumn) == QStringLiteral("."))
            refresh();
        else if (item->data(0, IsDirectoryRole).toBool())
            navigate(item->data(0, PathRole).toString());
        else
            editFile(item->data(0, PathRole).toString());
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, &SftpPanel::showContextMenu);

    setSession(nullptr);
    // Waiting for its tab's connection: shown, not logged (no connection yet).
    setStatusText(QStringLiteral("Connecting…"));
    refreshIcons();
}

void SftpPanel::refreshIcons()
{
    m_titleLabel->setFixedHeight(panelRowHeight(this)); // the look's font may have changed
    for (const auto &button : std::as_const(m_buttonIcons)) {
        if (button.button)
            button.button->setIcon(themedIcon(button.name, button.fallback));
    }
    for (int index = 0; index < m_view->topLevelItemCount(); ++index) {
        auto *item = m_view->topLevelItem(index);
        item->setIcon(NameColumn, fileTypeIcon(item->text(NameColumn),
                                               item->data(0, IsDirectoryRole).toBool(),
                                               item->data(0, IsSymlinkRole).toBool()));
    }
}

// Without a session: the panel's look with a message in the list, for
// tabs that have no files to show.
int SftpPanel::contentsWidth() const
{
    if (m_view->topLevelItemCount() == 0)
        return 0;
    // Measured, not resized: the user's column widths stay as they are.
    int width = 0;
    for (int column = 0; column < m_view->columnCount(); ++column) {
        if (!m_view->isColumnHidden(column))
            width += qMax(m_view->sizeHintForColumn(column), m_view->header()->sectionSizeHint(column));
    }
    width += 2 * m_view->frameWidth() + 8;
    if (m_view->verticalScrollBar()->isVisible())
        width += m_view->verticalScrollBar()->width();
    return width + this->width() - m_view->width();
}

void SftpPanel::showPlaceholder(const QString &title, const QString &message)
{
    setDisconnectedText(message);
    setSession(nullptr);
    m_titleLabel->setText(title);
    m_titleLabel->setToolTip({});
}

void SftpPanel::setTitle(const QString &title)
{
    m_title = title;
    m_titleLabel->setText(QStringLiteral("%1 · %2").arg(m_protocol, title));
    m_titleLabel->setToolTip(title);
}

void SftpPanel::setDisconnectedText(const QString &text)
{
    m_disconnectedText = text;
}

// Sync Folders, starting at the folder shown.
void SftpPanel::openSync()
{
    if (!m_session || !m_ready) {
        showMessage(QStringLiteral("Connect first, then sync."), true);
        return;
    }
    auto *dialog = new SftpSyncDialog(m_session, m_currentPath, window());
    dialog->show();
}

void SftpPanel::followFolder(const QString &path)
{
    if (m_ready && !path.isEmpty() && path != m_currentPath)
        navigate(path);
}

void SftpPanel::setSession(RemoteFileSession *session)
{
    const bool hadSession = m_session;
    if (m_session)
        disconnect(m_session, nullptr, this, nullptr);
    m_session = session;
    m_ready = false;
    resetTransfers();
    endWaiting();
    for (auto *widget : std::initializer_list<QWidget *>{m_upButton, m_homeButton, m_syncButton,
                                                         m_refreshButton, m_pathEdit})
        widget->setEnabled(false);
    if (!session) {
        // Only a connection that ended is worth a log line; placeholders
        // (no tab, a tab without files) are not.
        if (hadSession)
            showStatus(m_disconnectedText);
        else
            setStatusText(m_disconnectedText);
        return;
    }

    showStatus(QStringLiteral("Connecting…"));
    const auto preferences = loadSftpPreferences();
    session->setTemporaryUploads(preferences.temporaryUpload == QStringLiteral("off") ? -1
                                 : preferences.temporaryUpload == QStringLiteral("all")
                                     ? 0
                                     : qint64(preferences.temporaryUploadKb) * 1024);
    connect(session, &RemoteFileSession::sftpReady, this, [this](const QString &homePath) {
        m_ready = true;
        m_homePath = homePath;
        if (m_session) {
            m_protocol = m_session->fileProtocol();
            setTitle(m_title);
        }
        for (auto *widget : std::initializer_list<QWidget *>{m_upButton, m_homeButton, m_syncButton,
                                                             m_refreshButton, m_pathEdit})
            widget->setEnabled(true);
        qInfo("SFTP: ready, home %s", qUtf8Printable(homePath));
        // After a reconnect, return to the folder that was open before.
        navigate(m_currentPath.isEmpty() ? homePath : m_currentPath);
    });
    connect(session, &RemoteFileSession::sftpUnavailable, this, &SftpPanel::showStatus);
    connect(session, &RemoteFileSession::directoryListed, this, &SftpPanel::populate);
    connect(session, &RemoteFileSession::fileOperationFinished, this, [this](const QString &error) {
        if (!error.isEmpty())
            showMessage(error, true);
        refresh();
    });
    connect(session, &RemoteFileSession::transferProgress, this, &SftpPanel::updateProgress);
    connect(session, &RemoteFileSession::transferConflict, this, &SftpPanel::askConflict);
    connect(session, &RemoteFileSession::transferFinished, this, &SftpPanel::finishTransfer);
}

void SftpPanel::showStatus(const QString &text)
{
    qInfo("SFTP: %s", qUtf8Printable(text));
    setStatusText(text);
}

void SftpPanel::setStatusText(const QString &text)
{
    m_view->clear();
    m_statusLabel->setText(text);
    m_statusLabel->show();
    m_stack->setCurrentWidget(m_view);
}

void SftpPanel::showMessage(const QString &text, bool error)
{
    if (!text.isEmpty())
        qInfo("SFTP%s: %s", error ? " error" : "", qUtf8Printable(text));
    m_messageLabel->setText(text);
    m_messageLabel->setStyleSheet(error ? QStringLiteral("color: #c62828;")
                                        : QStringLiteral("color: palette(placeholder-text);"));
    m_messageLabel->setVisible(!text.isEmpty());
}

void SftpPanel::navigate(const QString &path)
{
    if (!m_session || !m_ready)
        return;
    m_session->listDirectory(path);
}

void SftpPanel::refresh()
{
    if (!m_currentPath.isEmpty())
        navigate(m_currentPath);
}

void SftpPanel::goUp()
{
    if (!m_currentPath.isEmpty() && m_currentPath != QStringLiteral("/"))
        navigate(parentRemotePath(m_currentPath));
}

void SftpPanel::populate(const QString &path, const QList<SftpEntry> &entries,
                         const QString &error)
{
    if (!error.isEmpty()) {
        // A folder remembered from before a reconnect may be gone; fall back
        // to the home folder.
        if (!m_statusLabel->isHidden() && path != m_homePath) { // nothing listed yet
            m_currentPath.clear();
            navigate(m_homePath);
        }
        showMessage(error, true);
        m_pathEdit->setText(m_currentPath);
        return;
    }

    const bool sameFolder = path == m_currentPath;
    QStringList previousSelection;
    if (sameFolder)
        previousSelection = selectedPaths();
    m_currentPath = path;
    m_pathEdit->setText(path);
    if (!sameFolder)
        showMessage(QString(), false);

    m_view->setSortingEnabled(false);
    m_view->clear();
    const auto preferences = loadSftpPreferences();
    if (preferences.showPlaces) {
        const auto addPlace = [this](const QString &name, const QString &target, const QString &tip) {
            auto *item = new SftpItem(m_view);
            item->setText(NameColumn, name);
            item->setData(0, PathRole, target);
            item->setData(0, IsDirectoryRole, true);
            item->setData(0, IsPlaceRole, true);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
            item->setIcon(NameColumn, fileTypeIcon(name, true, false));
            item->setToolTip(NameColumn, tip);
        };
        addPlace(QStringLiteral("."), path, QStringLiteral("This folder (%1): open to list it again").arg(path));
        if (path != QStringLiteral("/"))
            addPlace(QStringLiteral(".."), parentRemotePath(path),
                     QStringLiteral("The folder above (%1); drop here to move or upload into it")
                         .arg(parentRemotePath(path)));
    }
    for (const auto &entry : entries) {
        if (entry.name == QStringLiteral(".") || entry.name == QStringLiteral(".."))
            continue;
        if (!preferences.showHidden && entry.name.startsWith(QLatin1Char('.')))
            continue;
        auto *item = new SftpItem(m_view);
        const auto fullPath = joinRemotePath(path, entry.name);
        item->setText(NameColumn, entry.name);
        item->setText(SizeColumn, entry.isDirectory ? QString() : formatSize(entry.size));
        item->setText(ModifiedColumn, QLocale().toString(entry.modified, QLocale::ShortFormat));
        item->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
        item->setData(0, PathRole, fullPath);
        item->setData(0, IsDirectoryRole, entry.isDirectory);
        item->setData(0, SizeRole, static_cast<qulonglong>(entry.size));
        item->setData(0, ModifiedRole, entry.modified);
        item->setData(0, IsSymlinkRole, entry.isSymlink);
        item->setIcon(NameColumn, fileTypeIcon(entry.name, entry.isDirectory, entry.isSymlink));
        item->setToolTip(NameColumn,
                         QStringLiteral("%1\n%2  %3\nModified %4")
                             .arg(fullPath, permissionText(entry.permissions),
                                  entry.isDirectory ? QStringLiteral("folder")
                                                    : formatSize(entry.size),
                                  QLocale().toString(entry.modified, QLocale::LongFormat)));
        if (previousSelection.contains(fullPath))
            item->setSelected(true);
    }
    m_view->setSortingEnabled(true);
    m_statusLabel->hide();
    m_stack->setCurrentWidget(m_view);
}

// Returns whether the key is a panel shortcut; performs it only when execute
// is set, so the same check can claim the key from window-wide shortcuts.
bool SftpPanel::handleKey(int key, Qt::KeyboardModifiers modifiers, bool execute)
{
    modifiers &= ~Qt::KeypadModifier;
    std::function<void()> action;
    if (key == Qt::Key_V && modifiers == Qt::ControlModifier)
        action = [this] { pasteInto(m_currentPath); };
    else if (key == Qt::Key_Delete && modifiers == Qt::NoModifier && !selectedPaths().isEmpty())
        action = [this] { deleteSelected(); };
    else if (key == Qt::Key_F2 && modifiers == Qt::NoModifier && selectedPaths().size() == 1)
        action = [this] { renameSelected(); };
    else if (key == Qt::Key_F5 && modifiers == Qt::NoModifier)
        action = [this] { refresh(); };
    else if ((key == Qt::Key_Backspace && modifiers == Qt::NoModifier)
             || (key == Qt::Key_Up && modifiers == Qt::AltModifier))
        action = [this] { goUp(); };
    if (!action)
        return false;
    if (execute)
        action();
    return true;
}

QStringList SftpPanel::selectedPaths() const
{
    QStringList paths;
    for (auto *item : m_view->selectedItems())
        paths.append(item->data(0, PathRole).toString());
    return paths;
}

QTreeWidgetItem *SftpPanel::singleSelectedDirectory() const
{
    const auto items = m_view->selectedItems();
    return items.size() == 1 && items.first()->data(0, IsDirectoryRole).toBool() ? items.first()
                                                                                  : nullptr;
}

void SftpPanel::showContextMenu(const QPoint &position)
{
    if (!m_ready)
        return;
    auto *clicked = m_view->itemAt(position);
    if (clicked && !clicked->isSelected()) {
        m_view->clearSelection();
        clicked->setSelected(true);
        m_view->setCurrentItem(clicked);
    } else if (!clicked) {
        m_view->clearSelection();
    }

    const auto selection = selectedPaths();
    auto *directory = singleSelectedDirectory();
    const bool clipboardHasFiles =
        !localPathsFromMime(QApplication::clipboard()->mimeData()).isEmpty();

    QMenu menu(this);
    const auto icon = [](const char *name, QStyle::StandardPixmap fallback) {
        return themedIcon(QString::fromLatin1(name), fallback);
    };
    if (directory) {
        menu.addAction(icon("document-open", QStyle::SP_DirOpenIcon), QStringLiteral("Open"), this,
                       [this, directory] {
            navigate(directory->data(0, PathRole).toString());
        });
    }
    if (selection.size() == 1 && !directory) {
        menu.addAction(icon("document-edit", QStyle::SP_FileIcon), QStringLiteral("Edit"), this,
                       [this, path = selection.first()] { editFile(path); });
    }
    if (!selection.isEmpty())
        menu.addAction(icon("go-down", QStyle::SP_ArrowDown), QStringLiteral("Download…"), this,
                       &SftpPanel::downloadSelected);
    if (directory) {
        auto *pasteIntoAction = menu.addAction(
            QStringLiteral("Paste into \"%1\"").arg(directory->text(NameColumn)), this,
            [this, path = directory->data(0, PathRole).toString()] { pasteInto(path); });
        pasteIntoAction->setEnabled(clipboardHasFiles);
    }
    if (!menu.isEmpty())
        menu.addSeparator();

    auto *pasteAction = menu.addAction(icon("edit-paste", QStyle::SP_DialogApplyButton),
                                       QStringLiteral("Paste\tCtrl+V"), this,
                                       [this] { pasteInto(m_currentPath); });
    pasteAction->setEnabled(clipboardHasFiles);
    menu.addAction(icon("upload", QStyle::SP_ArrowUp), QStringLiteral("Upload Files…"), this,
                   &SftpPanel::uploadFilesDialog);
    menu.addAction(icon("upload", QStyle::SP_ArrowUp), QStringLiteral("Upload Folder…"), this,
                   &SftpPanel::uploadFolderDialog);
    menu.addAction(icon("folder-sync", QStyle::SP_DialogApplyButton), QStringLiteral("Sync Folders…"),
                   this, &SftpPanel::openSync);
    menu.addSeparator();
    menu.addAction(icon("folder-new", QStyle::SP_FileDialogNewFolder), QStringLiteral("New Folder…"),
                   this, &SftpPanel::createFolder);
    if (selection.size() == 1)
        menu.addAction(icon("edit-rename", QStyle::SP_FileDialogDetailedView),
                       QStringLiteral("Rename…\tF2"), this, &SftpPanel::renameSelected);
    if (!selection.isEmpty())
        menu.addAction(icon("go-jump", QStyle::SP_ArrowForward), QStringLiteral("Move to Folder…"), this,
                       &SftpPanel::moveSelectedDialog);
    if (!selection.isEmpty()) {
        menu.addAction(icon("edit-delete", QStyle::SP_TrashIcon), QStringLiteral("Delete…\tDel"),
                       this, &SftpPanel::deleteSelected);
        menu.addAction(icon("edit-copy", QStyle::SP_FileLinkIcon),
                       selection.size() == 1 ? QStringLiteral("Copy Path")
                                             : QStringLiteral("Copy Paths"),
                       this, &SftpPanel::copySelectedPaths);
    }
    menu.addSeparator();
    menu.addAction(icon("view-refresh", QStyle::SP_BrowserReload), QStringLiteral("Refresh\tF5"),
                   this, &SftpPanel::refresh);
    if (m_following && m_setFollowing) {
        auto *follow = menu.addAction(QStringLiteral("Follow the Terminal's Folder"));
        follow->setCheckable(true);
        follow->setChecked(m_following());
        follow->setToolTip(QStringLiteral("For this tab only; the session's setting stays as it is."));
        connect(follow, &QAction::toggled, this, [this](bool on) { m_setFollowing(on); });
    }
    menu.exec(m_view->viewport()->mapToGlobal(position));
}

void SftpPanel::setFollowControl(std::function<bool()> following, std::function<void(bool)> setFollowing)
{
    m_following = std::move(following);
    m_setFollowing = std::move(setFollowing);
}

void SftpPanel::uploadPaths(const QStringList &localPaths, const QString &remoteDirectory)
{
    if (!m_session || !m_ready || localPaths.isEmpty() || remoteDirectory.isEmpty())
        return;
    startChecked({true, localPaths, remoteDirectory, std::nullopt},
                 [this](int jobId) { m_jobUploads.insert(jobId, true); });
}

QString SftpPanel::uploadInto(const QStringList &localPaths, const QString &remoteDirectory)
{
    if (!m_session)
        return QStringLiteral("This tab is not connected.");
    if (!m_ready)
        return QStringLiteral("The file connection to the server is not ready yet (it opens after "
                              "logging on; a server without SFTP or SCP has none).");
    uploadPaths(localPaths, remoteDirectory);
    return QString();
}

void SftpPanel::pasteInto(const QString &remoteDirectory)
{
    const auto paths = localPathsFromMime(QApplication::clipboard()->mimeData());
    if (paths.isEmpty()) {
        showMessage(QStringLiteral("The clipboard does not contain files. Copy files in your "
                                   "file manager, then paste here."), false);
        return;
    }
    uploadPaths(paths, remoteDirectory);
}

void SftpPanel::uploadFilesDialog()
{
    const auto files = FileDialogs::openFileNames(
        this, QStringLiteral("Upload Files to %1").arg(m_currentPath), QDir::homePath());
    uploadPaths(files, m_currentPath);
}

void SftpPanel::uploadFolderDialog()
{
    const auto folder = FileDialogs::existingDirectory(
        this, QStringLiteral("Upload Folder to %1").arg(m_currentPath), QDir::homePath());
    if (!folder.isEmpty())
        uploadPaths({folder}, m_currentPath);
}

void SftpPanel::removeStaleEditFolders()
{
    // Copies left by a crash; a day old, so another running copy of
    // OpsRemote keeps the files it is editing.
    const QDir temp(QDir::tempPath());
    const auto limit = QDateTime::currentDateTime().addDays(-1);
    for (const auto &info : temp.entryInfoList({QStringLiteral("opsremote-edit-*")},
                                               QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (info.isDir() && !info.isSymLink() && info.isWritable() && info.lastModified() < limit)
            QDir(info.absoluteFilePath()).removeRecursively();
    }
}

SftpPanel::~SftpPanel()
{
    // The local copies of edited files go with the panel.
    if (!m_editDirectory.isEmpty())
        QDir(m_editDirectory).removeRecursively();
}

// Downloads a remote file into a private folder and opens it in the editor;
// saving it there uploads it again (see uploadEdited()).
void SftpPanel::editFile(const QString &remotePath)
{
    if (!m_session || !m_ready)
        return;
    if (!m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
            // Editors often save by replacing the file, which ends the watch;
            // a short wait also lets the editor finish writing.
            QTimer::singleShot(500, this, [this, path] {
                if (QFileInfo::exists(path) && !m_watcher->files().contains(path))
                    m_watcher->addPath(path);
                uploadEdited(path);
            });
        });
    }
    for (auto it = m_editedFiles.cbegin(); it != m_editedFiles.cend(); ++it) {
        if (it.value() == remotePath && QFileInfo::exists(it.key())) {
            openEditor(it.key());
            return;
        }
    }
    if (m_editDirectory.isEmpty()) {
        // Created owner-only in one step, so other users never see inside.
        QTemporaryDir directory(QDir(QDir::tempPath()).filePath(QStringLiteral("opsremote-edit-XXXXXX")));
        if (!directory.isValid()) {
            showMessage(QStringLiteral("Could not create a folder for editing."), true);
            return;
        }
        directory.setAutoRemove(false);
        m_editDirectory = directory.path();
    }
    // One folder per file, so files with the same name do not collide.
    const QString folder = QDir(m_editDirectory).filePath(QString::number(m_editedFiles.size() + 1));
    if (!QDir().mkpath(folder)) {
        showMessage(QStringLiteral("Could not create a folder for editing."), true);
        return;
    }
    const QString local = QDir(folder).filePath(remotePath.section(QLatin1Char('/'), -1));
    m_editedFiles.insert(local, remotePath);
    const int jobId = m_session->download({remotePath}, folder);
    m_jobUploads.insert(jobId, false);
    m_editDownloads.insert(jobId, local);
}

// Files the system's default program would run rather than open: a
// server could name a file like this to run code here when it is edited.
static bool couldRun(const QString &localPath)
{
    static const QStringList types{
        // Windows
        QStringLiteral("exe"), QStringLiteral("com"), QStringLiteral("bat"), QStringLiteral("cmd"),
        QStringLiteral("ps1"), QStringLiteral("psm1"), QStringLiteral("vbs"), QStringLiteral("vbe"),
        QStringLiteral("js"), QStringLiteral("jse"), QStringLiteral("wsf"), QStringLiteral("wsh"),
        QStringLiteral("msi"), QStringLiteral("msp"), QStringLiteral("scr"), QStringLiteral("pif"),
        QStringLiteral("lnk"), QStringLiteral("url"), QStringLiteral("hta"), QStringLiteral("cpl"),
        QStringLiteral("jar"), QStringLiteral("reg"), QStringLiteral("inf"), QStringLiteral("msc"),
        QStringLiteral("application"), QStringLiteral("appref-ms"), QStringLiteral("library-ms"),
        QStringLiteral("settingcontent-ms"), QStringLiteral("scf"),
        // Linux and macOS
        QStringLiteral("desktop"), QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"),
        QStringLiteral("run"), QStringLiteral("bin"), QStringLiteral("appimage"), QStringLiteral("py"),
        QStringLiteral("pl"), QStringLiteral("rb"), QStringLiteral("command"), QStringLiteral("app")};
    const QFileInfo info(localPath);
    if (types.contains(info.suffix(), Qt::CaseInsensitive))
        return true;
#ifndef Q_OS_WIN
    // Downloads keep the server's executable bit.
    return info.isExecutable();
#else
    return false;
#endif
}

// A plain text editor for files that must not go to the system's default
// program, or empty if none is found.
static QStringList plainTextEditor()
{
#ifdef Q_OS_WIN
    return {QStringLiteral("notepad.exe")};
#else
    for (const char *name : {"gnome-text-editor", "gedit", "kate", "kwrite", "mousepad", "xed",
                             "pluma", "featherpad", "leafpad", "code"}) {
        const QString program = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!program.isEmpty())
            return {program};
    }
    return {};
#endif
}

void SftpPanel::openEditor(const QString &localPath)
{
    const auto editor = loadSftpPreferences().editor;
    QStringList arguments = QProcess::splitCommand(editor);
    if (editor.isEmpty()) {
        if (!couldRun(localPath)) {
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(localPath)))
                showMessage(QStringLiteral("No program is set to open this kind of file. Choose an "
                                           "editor in Preferences → Files & transfers."), true);
            return;
        }
        arguments = plainTextEditor();
        if (arguments.isEmpty()) {
            showMessage(QStringLiteral("%1 could run if opened with its usual program, so it is "
                                       "only opened in a text editor. Choose one in Preferences "
                                       "→ Files & transfers.").arg(QFileInfo(localPath).fileName()), true);
            return;
        }
    }
    const QString program = arguments.takeFirst();
    arguments.append(localPath);
    if (!QProcess::startDetached(program, arguments))
        showMessage(QStringLiteral("Could not start the editor \"%1\".").arg(program), true);
}

void SftpPanel::uploadEdited(const QString &localPath)
{
    const auto remote = m_editedFiles.value(localPath);
    if (remote.isEmpty() || !QFileInfo::exists(localPath))
        return;
    if (!m_session || !m_ready) {
        showMessage(QStringLiteral("Not connected: %1 was not saved on the server.").arg(remote), true);
        return;
    }
    const int slash = remote.lastIndexOf(QLatin1Char('/'));
    const int jobId = m_session->upload({localPath}, slash > 0 ? remote.left(slash)
                                                                : QStringLiteral("/"));
    m_jobUploads.insert(jobId, true);
    m_editUploads.insert(jobId, remote);
}

void SftpPanel::downloadSelected()
{
    const auto paths = selectedPaths();
    if (paths.isEmpty() || !m_session)
        return;
    const auto preferences = loadSftpPreferences();
    const auto defaultFolder = preferences.downloadFolder.isEmpty()
                                   ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                                   : preferences.downloadFolder;
    if (m_lastDownloadDirectory.isEmpty())
        m_lastDownloadDirectory = defaultFolder;
    const auto directory = preferences.askDownloadFolder
                               ? FileDialogs::existingDirectory(this, QStringLiteral("Download To"),
                                                                   m_lastDownloadDirectory)
                               : defaultFolder;
    if (directory.isEmpty() || !m_session)
        return;
    m_lastDownloadDirectory = directory;
    startChecked({false, paths, directory, std::nullopt},
                 [this](int jobId) { m_jobUploads.insert(jobId, false); });
}

void SftpPanel::createFolder()
{
    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("New Folder"),
                                            QStringLiteral("Folder name in %1:").arg(m_currentPath),
                                            QLineEdit::Normal, QString(), &accepted)
                          .trimmed();
    if (!accepted || name.isEmpty() || !m_session)
        return;
    if (name.contains(QLatin1Char('/')) || name == QStringLiteral(".")
        || name == QStringLiteral("..")) {
        showMessage(QStringLiteral("A folder name cannot contain \"/\"."), true);
        return;
    }
    m_session->makeDirectory(joinRemotePath(m_currentPath, name));
}

void SftpPanel::renameSelected()
{
    const auto items = m_view->selectedItems();
    if (items.size() != 1 || !m_session)
        return;
    const auto oldName = items.first()->text(NameColumn);
    const auto oldPath = items.first()->data(0, PathRole).toString();
    bool accepted = false;
    const auto newName = QInputDialog::getText(this, QStringLiteral("Rename"),
                                               QStringLiteral("New name:"), QLineEdit::Normal,
                                               oldName, &accepted)
                             .trimmed();
    if (!accepted || newName.isEmpty() || newName == oldName || !m_session)
        return;
    if (newName.contains(QLatin1Char('/'))) {
        showMessage(QStringLiteral("A name cannot contain \"/\"."), true);
        return;
    }
    m_session->renamePath(oldPath, joinRemotePath(parentRemotePath(oldPath), newName));
}

// Moves remote files and folders into another folder on the same server.
void SftpPanel::movePaths(const QStringList &paths, const QString &folder)
{
    if (!m_session || folder.isEmpty())
        return;
    const QString target = folder.size() > 1 && folder.endsWith(QLatin1Char('/')) ? folder.chopped(1) : folder;
    for (const auto &path : paths) {
        if (parentRemotePath(path) == target)
            continue; // already there
        if (target == path || target.startsWith(path + QLatin1Char('/'))) {
            showMessage(QStringLiteral("\"%1\" cannot be moved into itself.").arg(path), true);
            continue;
        }
        m_session->renamePath(path, joinRemotePath(target, path.section(QLatin1Char('/'), -1)));
    }
}

void SftpPanel::moveSelectedDialog()
{
    const auto paths = selectedPaths();
    if (paths.isEmpty() || !m_session)
        return;
    bool accepted = false;
    const auto folder = QInputDialog::getText(
                            this, QStringLiteral("Move to Folder"),
                            paths.size() == 1
                                ? QStringLiteral("Move \"%1\" to this folder on the server:")
                                      .arg(paths.first().section(QLatin1Char('/'), -1))
                                : QStringLiteral("Move %1 items to this folder on the server:").arg(paths.size()),
                            QLineEdit::Normal, m_currentPath, &accepted)
                            .trimmed();
    if (!accepted || folder.isEmpty())
        return;
    movePaths(paths, folder.startsWith(QLatin1Char('/')) ? folder : joinRemotePath(m_currentPath, folder));
}

void SftpPanel::deleteSelected()
{
    const auto paths = selectedPaths();
    if (paths.isEmpty() || !m_session)
        return;
    if (!loadSftpPreferences().confirmDelete) {
        m_session->removePaths(paths);
        return;
    }
    int folders = 0;
    for (auto *item : m_view->selectedItems())
        folders += item->data(0, IsDirectoryRole).toBool() ? 1 : 0;

    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("Delete Remote Files"));
    confirmation.setText(paths.size() == 1
                             ? QStringLiteral("Delete \"%1\"?").arg(m_view->selectedItems()
                                                                        .first()->text(NameColumn))
                             : QStringLiteral("Delete %1 items?").arg(paths.size()));
    confirmation.setInformativeText(
        folders > 0 ? QStringLiteral("Folders are deleted with everything inside them. "
                                     "This cannot be undone.")
                    : QStringLiteral("This cannot be undone."));
    auto *deleteButton = confirmation.addButton(QStringLiteral("Delete"),
                                                QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() == deleteButton && m_session)
        m_session->removePaths(paths);
}

void SftpPanel::copySelectedPaths()
{
    QApplication::clipboard()->setText(selectedPaths().join(QLatin1Char('\n')));
}

// Progress goes to the transfer list (the tab's strip, Transfers at the
// bottom right), one entry per job, with the speed and files done.
void SftpPanel::updateProgress(int jobId, const QString &currentFile, quint64 bytesDone,
                               quint64 bytesTotal, int filesDone, int filesTotal, int queuedJobs)
{
    // Jobs started elsewhere (Sync Folders) are known to the session.
    const bool upload = m_jobUploads.contains(jobId) ? m_jobUploads.value(jobId)
                                                     : m_session && m_session->isUploadJob(jobId);
    auto *transfers = TransferList::instance();
    if (!m_transferIds.contains(jobId)) {
        const QPointer<SftpPanel> panel(this);
        m_transferIds.insert(jobId, transfers->start(
                                        m_ownerPage ? m_ownerPage.data() : this,
                                        currentFile.isEmpty() ? (upload ? QStringLiteral("Preparing upload…")
                                                                        : QStringLiteral("Preparing download…"))
                                                              : currentFile,
                                        upload,
                                        [panel, jobId] {
                                            if (panel && panel->m_session)
                                                panel->m_session->cancelJob(jobId);
                                        },
                                        QStringLiteral("Cancel this transfer")));
        const int id = m_transferIds.value(jobId);
        transfers->setPause(id, [panel](bool paused) {
            if (panel && panel->m_session)
                panel->m_session->setTransfersPaused(paused);
        });
        // Retry: the same files again from the beginning, on the tab's
        // connection then (also after a reconnect). Not for files opened for
        // editing: those go with their editor.
        const auto job = m_session ? m_session->job(jobId) : std::nullopt;
        if (job && !m_editDownloads.contains(jobId) && !m_editUploads.contains(jobId)) {
            transfers->setRetry(id, [panel, job = *job] {
                if (panel && panel->m_session)
                    panel->startChecked(job, [panel, upload = job.upload](int jobId) {
                        panel->m_jobUploads.insert(jobId, upload);
                    });
            });
        }
        // What it writes, so a transfer of the same file elsewhere asks first.
        if (job)
            transfers->setTargets(id, targetsOf(*job));
    }

    // Smoothed transfer rate, sampled at most twice a second.
    if (jobId != m_speedJobId) {
        m_speedJobId = jobId;
        m_speedBytes = bytesDone;
        m_bytesPerSecond = 0;
        m_speedTimer.start();
    } else if (m_speedTimer.elapsed() >= 500) {
        const double instant = static_cast<double>(bytesDone - m_speedBytes) * 1000.0
                               / static_cast<double>(m_speedTimer.elapsed());
        m_bytesPerSecond = m_bytesPerSecond == 0 ? instant
                                                 : 0.7 * m_bytesPerSecond + 0.3 * instant;
        m_speedBytes = bytesDone;
        m_speedTimer.restart();
    }

    QStringList details{QStringLiteral("%1 of %2").arg(formatSize(bytesDone), formatSize(bytesTotal))};
    if (filesTotal > 1)
        details.append(QStringLiteral("%1/%2 files").arg(filesDone).arg(filesTotal));
    if (m_bytesPerSecond > 0)
        details.append(QStringLiteral("%1/s").arg(formatSize(static_cast<quint64>(m_bytesPerSecond))));
    if (queuedJobs > 0)
        details.append(QStringLiteral("%1 queued").arg(queuedJobs));
    // Without byte counts (folders still being listed), files done stand in.
    const quint64 total = bytesTotal > 0 ? bytesTotal : quint64(qMax(filesTotal, 0));
    const quint64 done = bytesTotal > 0 ? bytesDone : quint64(qMax(filesDone, 0));
    transfers->update(m_transferIds.value(jobId), currentFile, done, total, details.join(QStringLiteral(" · ")));
}

void SftpPanel::askConflict(const QString &path, quint64 existingSize,
                            const QDateTime &existingModified, quint64 newSize,
                            const QDateTime &newModified, bool upload)
{
    // Saving an edited file replaces the server's copy, unless that changed
    // since it was downloaded or last saved.
    if (upload && m_session && !m_editUploads.isEmpty()
        && std::find(m_editUploads.cbegin(), m_editUploads.cend(), path) != m_editUploads.cend()) {
        const FileVersion remote{existingSize, existingModified.toSecsSinceEpoch()};
        m_editUploading.insert(path, {newSize, newModified.toSecsSinceEpoch()});
        if (m_editVersions.value(path) == remote) {
            m_session->resolveConflict(SftpConflictResolution::Overwrite, false);
            return;
        }
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("File Changed on the Server"));
        box.setText(QStringLiteral("%1 was changed on the server since you opened it.").arg(path));
        box.setInformativeText(QStringLiteral("Saving now would replace those changes."));
        auto *overwrite = box.addButton(QStringLiteral("Overwrite"), QMessageBox::DestructiveRole);
        auto *copy = box.addButton(QStringLiteral("Save as a Copy"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("Don't Save"), QMessageBox::RejectRole);
        box.setDefaultButton(copy);
        box.exec();
        if (!m_session)
            return;
        m_session->resolveConflict(box.clickedButton() == overwrite ? SftpConflictResolution::Overwrite
                                   : box.clickedButton() == copy    ? SftpConflictResolution::Rename
                                                                    : SftpConflictResolution::Skip,
                                   false);
        if (box.clickedButton() != overwrite)
            m_editUploading.remove(path);
        return;
    }
    // A fixed answer from Preferences applies to the whole transfer.
    const auto policy = loadSftpPreferences().conflict;
    if (policy != QStringLiteral("ask") && m_session) {
        m_session->resolveConflict(policy == QStringLiteral("overwrite") ? SftpConflictResolution::Overwrite
                                   : policy == QStringLiteral("skip")    ? SftpConflictResolution::Skip
                                                                         : SftpConflictResolution::Rename,
                                   true);
        return;
    }
    const QFileInfo info(path);
    QMessageBox dialog(this);
    dialog.setIcon(QMessageBox::Question);
    dialog.setWindowTitle(QStringLiteral("File Already Exists"));
    dialog.setText(QStringLiteral("\"%1\" already exists in %2.")
                       .arg(info.fileName(), upload ? info.path()
                                                    : QDir::toNativeSeparators(info.path())));
    dialog.setInformativeText(
        QStringLiteral("Existing: %1, modified %2\nNew: %3, modified %4")
            .arg(formatSize(existingSize),
                 QLocale().toString(existingModified, QLocale::ShortFormat),
                 formatSize(newSize), QLocale().toString(newModified, QLocale::ShortFormat)));
    auto *applyToAll = new QCheckBox(QStringLiteral("Apply to all remaining conflicts"), &dialog);
    dialog.setCheckBox(applyToAll);
    auto *overwriteButton = dialog.addButton(QStringLiteral("Overwrite"),
                                             QMessageBox::DestructiveRole);
    auto *renameButton = dialog.addButton(QStringLiteral("Keep Both"), QMessageBox::AcceptRole);
    renameButton->setToolTip(QStringLiteral("Save the new file as \"name (1).ext\""));
    auto *skipButton = dialog.addButton(QStringLiteral("Skip"), QMessageBox::RejectRole);
    auto *cancelButton = dialog.addButton(QStringLiteral("Cancel Transfer"),
                                          QMessageBox::RejectRole);
    dialog.setDefaultButton(renameButton);
    dialog.setEscapeButton(cancelButton);
    dialog.exec();

    if (!m_session)
        return;
    auto resolution = SftpConflictResolution::Cancel;
    if (dialog.clickedButton() == overwriteButton)
        resolution = SftpConflictResolution::Overwrite;
    else if (dialog.clickedButton() == renameButton)
        resolution = SftpConflictResolution::Rename;
    else if (dialog.clickedButton() == skipButton)
        resolution = SftpConflictResolution::Skip;
    m_session->resolveConflict(resolution, applyToAll->isChecked());
}

void SftpPanel::finishTransfer(int jobId, bool upload, const QString &destination,
                               int filesTransferred, int skipped, const QStringList &errors,
                               bool cancelled)
{
    m_jobUploads.remove(jobId);
    if (const int id = m_transferIds.take(jobId))
        TransferList::instance()->finish(id, cancelled            ? TransferList::State::Cancelled
                                             : !errors.isEmpty() ? TransferList::State::Failed
                                                                 : TransferList::State::Done);
    if (m_jobUploads.isEmpty())
        resetTransfers();
    if (const auto local = m_editDownloads.take(jobId); !local.isEmpty()) {
        if (errors.isEmpty() && !cancelled && QFileInfo::exists(local)) {
            // Downloads keep the server's modification time.
            const QFileInfo info(local);
            m_editVersions.insert(m_editedFiles.value(local),
                                  {quint64(info.size()), info.lastModified().toSecsSinceEpoch()});
            m_watcher->addPath(local);
            openEditor(local);
            showMessage(QStringLiteral("Editing %1. Each save uploads it again.")
                            .arg(m_editedFiles.value(local)), false);
        } else {
            m_editedFiles.remove(local);
            showMessage(QStringLiteral("Could not open %1 for editing: %2")
                            .arg(QFileInfo(local).fileName(), errors.value(0)), true);
        }
        return;
    }
    if (const auto remote = m_editUploads.take(jobId); !remote.isEmpty()) {
        const bool saved = errors.isEmpty() && !cancelled && skipped == 0;
        // The server now has the uploaded copy (it keeps our modification time).
        if (saved && m_editUploading.contains(remote))
            m_editVersions.insert(remote, m_editUploading.take(remote));
        if (destination == m_currentPath)
            refresh();
        if (skipped > 0 && errors.isEmpty()) {
            showMessage(QStringLiteral("Not saved on the server: %1 changed there.").arg(remote), true);
            return;
        }
        showMessage(saved ? QStringLiteral("Saved %1 on the server.").arg(remote)
                          : QStringLiteral("Could not save %1 on the server: %2")
                                .arg(remote, errors.value(0)),
                    !saved);
        if (!saved && !errors.isEmpty())
            QMessageBox::warning(this, QStringLiteral("Save Failed"), errors.value(0));
        return;
    }

    QString summary = QStringLiteral("%1 %2 %3 to %4")
                          .arg(upload ? QStringLiteral("Uploaded") : QStringLiteral("Downloaded"))
                          .arg(filesTransferred)
                          .arg(filesTransferred == 1 ? QStringLiteral("file")
                                                     : QStringLiteral("files"),
                               upload ? destination : QDir::toNativeSeparators(destination));
    if (skipped > 0)
        summary += QStringLiteral(", skipped %1").arg(skipped);
    if (cancelled)
        summary = QStringLiteral("Transfer cancelled. ") + summary;
    summary += QLatin1Char('.');

    if (!errors.isEmpty()) {
        showMessage(QStringLiteral("%1 %2 failed.").arg(summary).arg(errors.size()), true);
        auto listed = errors.mid(0, MaximumListedErrors);
        if (errors.size() > MaximumListedErrors)
            listed.append(QStringLiteral("…and %1 more.").arg(errors.size() - MaximumListedErrors));
        QMessageBox::warning(this, QStringLiteral("Transfer Problems"),
                             QStringLiteral("%1\n\n%2").arg(summary, listed.join(QLatin1Char('\n'))));
    } else {
        showMessage(summary, false);
    }
    if (upload && destination == m_currentPath)
        refresh();
    if (!upload && !cancelled && filesTransferred > 0 && loadSftpPreferences().openFolderAfterDownload)
        QDesktopServices::openUrl(QUrl::fromLocalFile(destination));
}

QStringList SftpPanel::targetsOf(const RemoteFileSession::Job &job, QStringList *folders) const
{
    QStringList targets;
    const QString server = job.upload && m_session ? m_session->server() : QString();
    for (QString source : job.sources) {
        while (source.size() > 1 && (source.endsWith(QLatin1Char('/')) || source.endsWith(QLatin1Char('\\'))))
            source.chop(1);
        const QString name = QDir::fromNativeSeparators(source).section(QLatin1Char('/'), -1);
        QString path;
        bool folder = false;
        if (job.upload) {
            path = job.destination.endsWith(QLatin1Char('/')) ? job.destination + name
                                                              : job.destination + QLatin1Char('/') + name;
            folder = QFileInfo(source).isDir();
        } else {
            path = QDir(job.destination).filePath(name);
            folder = QFileInfo(path).isDir();
            for (int i = 0; i < m_view->topLevelItemCount() && !folder; ++i) {
                const auto *item = m_view->topLevelItem(i);
                folder = item->data(0, PathRole).toString() == source && item->data(0, IsDirectoryRole).toBool();
            }
        }
        targets.append(TransferList::target(server, path));
        if (folder && folders)
            folders->append(targets.constLast());
    }
    return targets;
}

void SftpPanel::startChecked(const RemoteFileSession::Job &job, std::function<void(int)> started)
{
    if (!m_session || !m_ready)
        return;
    auto *transfers = TransferList::instance();
    QStringList folders;
    const auto targets = targetsOf(job, &folders);
    const auto others = transfers->writingTo(targets);
    if (others.isEmpty()) {
        started(m_session->startJob(job));
        return;
    }

    // A new name helps only where both write the very same file: the paths
    // it must not take. A folder is merged into, it cannot take a new name.
    QStringList avoid;
    bool newNameHelps = folders.isEmpty();
    for (const auto &other : others) {
        for (const auto &theirs : other.targets) {
            if (folders.contains(theirs))
                continue;
            if (targets.contains(theirs))
                avoid.append(theirs.section(QLatin1Char('|'), 1));
            else if (std::any_of(targets.cbegin(), targets.cend(), [&theirs](const QString &mine) {
                         return theirs.startsWith(mine + QLatin1Char('/')) || mine.startsWith(theirs + QLatin1Char('/'));
                     }))
                newNameHelps = false;
        }
    }
    newNameHelps = newNameHelps && !avoid.isEmpty();

    const auto &first = others.constFirst();
    const QString where = TransferList::ownerName && first.owner ? TransferList::ownerName(first.owner) : QString();
    const QString what = job.sources.size() == 1
                             ? QDir::fromNativeSeparators(job.sources.constFirst()).section(QLatin1Char('/'), -1)
                             : QStringLiteral("%1 items").arg(job.sources.size());
    QMessageBox dialog(this);
    dialog.setIcon(QMessageBox::Question);
    dialog.setWindowTitle(QStringLiteral("Same File in Transfer"));
    dialog.setText(QStringLiteral("%1 is being %2 to the same place right now%3.")
                       .arg(what, first.upload ? QStringLiteral("uploaded") : QStringLiteral("downloaded"),
                            where.isEmpty() ? QString() : QStringLiteral(" (in %1)").arg(where)));
    dialog.setInformativeText(QStringLiteral("Both would write the same file: the one that ends last "
                                             "replaces the other. What should this %1 do?")
                                  .arg(job.upload ? QStringLiteral("upload") : QStringLiteral("download")));
    auto *waitButton = dialog.addButton(QStringLiteral("Wait for It"), QMessageBox::AcceptRole);
    waitButton->setToolTip(QStringLiteral("Start when the other transfer has ended; then you are asked "
                                          "whether to overwrite the file it wrote"));
    auto *newNameButton = dialog.addButton(QStringLiteral("Save Under a New Name"), QMessageBox::AcceptRole);
    newNameButton->setEnabled(newNameHelps);
    newNameButton->setToolTip(newNameHelps ? QStringLiteral("Save it as \"name (1).ext\" next to the other")
                                           : QStringLiteral("A folder cannot take a new name: it is merged into"));
    auto *stopButton = dialog.addButton(QStringLiteral("Stop It, Start This"), QMessageBox::DestructiveRole);
    stopButton->setToolTip(QStringLiteral("Cancel the other transfer (only that one) and start this one "
                                          "once it has stopped"));
    auto *dontButton = dialog.addButton(QStringLiteral("Don't Start"), QMessageBox::RejectRole);
    dialog.setDefaultButton(waitButton);
    dialog.setEscapeButton(dontButton);
    dialog.exec();
    if (!m_session || !m_ready || dialog.clickedButton() == dontButton || !dialog.clickedButton())
        return;
    if (dialog.clickedButton() == newNameButton) {
        started(m_session->startJob(job, avoid));
        return;
    }

    QList<int> otherIds;
    for (const auto &other : others)
        otherIds.append(other.id);
    const bool stop = dialog.clickedButton() == stopButton;
    if (stop) {
        for (const int id : std::as_const(otherIds))
            transfers->cancel(id);
    }
    // Waits in the list, as a transfer that can be cancelled (✕).
    auto listId = std::make_shared<int>(0);
    *listId = transfers->start(m_ownerPage ? m_ownerPage.data() : this, what, job.upload, [listId] {
        TransferList::instance()->finish(*listId, TransferList::State::Cancelled);
    }, QStringLiteral("Don't start this transfer"));
    transfers->update(*listId, what, 0, 0,
                      stop ? QStringLiteral("Waiting for the other transfer to stop")
                           : QStringLiteral("Waiting for the other transfer of this file"));
    transfers->setTargets(*listId, targets);
    m_waiting.append({*listId, job, otherIds, std::move(started)});
    startWaiting(); // the other may have ended while asking
}

void SftpPanel::startWaiting()
{
    if (std::exchange(m_startingWaiting, true))
        return;
    auto *transfers = TransferList::instance();
    for (qsizetype i = 0; i < m_waiting.size();) {
        const auto &waiting = m_waiting.at(i);
        if (!transfers->isRunning(waiting.listId)) { // cancelled while waiting
            m_waiting.removeAt(i);
            continue;
        }
        if (std::any_of(waiting.others.cbegin(), waiting.others.cend(),
                        [transfers](int id) { return transfers->isRunning(id); })) {
            ++i;
            continue;
        }
        const auto ready = m_waiting.takeAt(i);
        transfers->remove(ready.listId);
        startChecked(ready.job, ready.started); // may ask again: another one may have started meanwhile
    }
    m_startingWaiting = false;
}

// The connection went: the waiting transfers cannot start on it.
void SftpPanel::endWaiting()
{
    for (const auto &waiting : std::exchange(m_waiting, {}))
        TransferList::instance()->finish(waiting.listId, TransferList::State::Failed);
}

void SftpPanel::resetTransfers()
{
    m_jobUploads.clear();
    m_speedJobId = 0;
    m_bytesPerSecond = 0;
    // Jobs that will not report again (the connection went): ended.
    for (const int id : std::as_const(m_transferIds))
        TransferList::instance()->finish(id, TransferList::State::Failed);
    m_transferIds.clear();
}
