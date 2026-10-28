// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sftppanel.h"
#include "icons.h"
#include "preferences.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
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
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {
constexpr int PathRole = Qt::UserRole;
constexpr int IsDirectoryRole = Qt::UserRole + 1;
constexpr int SizeRole = Qt::UserRole + 2;
constexpr int ModifiedRole = Qt::UserRole + 3;
constexpr int IsSymlinkRole = Qt::UserRole + 4;
constexpr int ProgressScale = 1000;
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

    // Returns whether the key is handled; only performs it when execute is set.
    std::function<bool(int, Qt::KeyboardModifiers, bool execute)> keyHandler;
    std::function<void(const QStringList &, QTreeWidgetItem *)> dropHandler;

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

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (dropHandler && !localPathsFromMime(event->mimeData()).isEmpty())
            event->acceptProposedAction();
        else
            event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (dropHandler && event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            event->ignore();
    }

    void dropEvent(QDropEvent *event) override
    {
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
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_titleLabel = new QLabel(this);
    // A style sheet rather than setFont(), so the title follows changes to
    // the application's interface font.
    m_titleLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
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
    m_statusLabel = new QLabel(m_stack);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    m_stack->addWidget(m_statusLabel);

    m_view = new SftpFileView(m_stack);
    m_view->setColumnCount(3);
    m_view->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Size"),
                             QStringLiteral("Modified")});
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setAcceptDrops(true);
    m_view->viewport()->setAcceptDrops(true);
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
    connect(m_view, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (item && item->data(0, IsDirectoryRole).toBool())
            navigate(item->data(0, PathRole).toString());
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, &SftpPanel::showContextMenu);

    m_progressFooter = new QWidget(this);
    auto *footerLayout = new QVBoxLayout(m_progressFooter);
    footerLayout->setContentsMargins(0, 4, 0, 0);
    footerLayout->setSpacing(2);
    auto *footerTop = new QHBoxLayout;
    m_progressLabel = new QLabel(m_progressFooter);
    m_progressLabel->setMinimumWidth(1);
    footerTop->addWidget(m_progressLabel, 1);
    auto *cancelButton = new QToolButton(m_progressFooter);
    m_buttonIcons.append({cancelButton, QStringLiteral("process-stop"), QStyle::SP_DialogCancelButton});
    cancelButton->setToolTip(QStringLiteral("Cancel the transfer and any queued transfers"));
    cancelButton->setAutoRaise(true);
    footerTop->addWidget(cancelButton);
    footerLayout->addLayout(footerTop);
    m_progressBar = new QProgressBar(m_progressFooter);
    m_progressBar->setRange(0, ProgressScale);
    m_progressBar->setTextVisible(true);
    footerLayout->addWidget(m_progressBar);
    m_progressDetail = new QLabel(m_progressFooter);
    m_progressDetail->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    footerLayout->addWidget(m_progressDetail);
    m_progressFooter->hide();
    layout->addWidget(m_progressFooter);
    connect(cancelButton, &QToolButton::clicked, this, [this] {
        if (m_session)
            m_session->cancelTransfers();
    });

    setSession(nullptr);
    showStatus(QStringLiteral("Connecting…"));
    refreshIcons();
}

void SftpPanel::refreshIcons()
{
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

void SftpPanel::setTitle(const QString &title)
{
    m_titleLabel->setText(QStringLiteral("SFTP · %1").arg(title));
    m_titleLabel->setToolTip(title);
}

void SftpPanel::setSession(SshSession *session)
{
    if (m_session)
        disconnect(m_session, nullptr, this, nullptr);
    m_session = session;
    m_ready = false;
    resetTransfers();
    for (auto *widget : std::initializer_list<QWidget *>{m_upButton, m_homeButton,
                                                         m_refreshButton, m_pathEdit})
        widget->setEnabled(false);
    if (!session) {
        showStatus(QStringLiteral("Disconnected.\nPress Enter in the terminal to reconnect."));
        return;
    }

    showStatus(QStringLiteral("Connecting…"));
    connect(session, &SshSession::sftpReady, this, [this](const QString &homePath) {
        m_ready = true;
        m_homePath = homePath;
        for (auto *widget : std::initializer_list<QWidget *>{m_upButton, m_homeButton,
                                                             m_refreshButton, m_pathEdit})
            widget->setEnabled(true);
        qInfo("SFTP: ready, home %s", qUtf8Printable(homePath));
        // After a reconnect, return to the folder that was open before.
        navigate(m_currentPath.isEmpty() ? homePath : m_currentPath);
    });
    connect(session, &SshSession::sftpUnavailable, this, &SftpPanel::showStatus);
    connect(session, &SshSession::directoryListed, this, &SftpPanel::populate);
    connect(session, &SshSession::fileOperationFinished, this, [this](const QString &error) {
        if (!error.isEmpty())
            showMessage(error, true);
        refresh();
    });
    connect(session, &SshSession::transferProgress, this, &SftpPanel::updateProgress);
    connect(session, &SshSession::transferConflict, this, &SftpPanel::askConflict);
    connect(session, &SshSession::transferFinished, this, &SftpPanel::finishTransfer);
}

void SftpPanel::showStatus(const QString &text)
{
    qInfo("SFTP: %s", qUtf8Printable(text));
    m_statusLabel->setText(text);
    m_stack->setCurrentWidget(m_statusLabel);
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
        if (m_stack->currentWidget() != m_view && path != m_homePath) {
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
    const bool showHidden = loadSftpPreferences().showHidden;
    for (const auto &entry : entries) {
        if (!showHidden && entry.name.startsWith(QLatin1Char('.')))
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
    menu.addSeparator();
    menu.addAction(icon("folder-new", QStyle::SP_FileDialogNewFolder), QStringLiteral("New Folder…"),
                   this, &SftpPanel::createFolder);
    if (selection.size() == 1)
        menu.addAction(icon("edit-rename", QStyle::SP_FileDialogDetailedView),
                       QStringLiteral("Rename…\tF2"), this, &SftpPanel::renameSelected);
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
    menu.exec(m_view->viewport()->mapToGlobal(position));
}

void SftpPanel::uploadPaths(const QStringList &localPaths, const QString &remoteDirectory)
{
    if (!m_session || !m_ready || localPaths.isEmpty() || remoteDirectory.isEmpty())
        return;
    const int jobId = m_session->upload(localPaths, remoteDirectory);
    m_jobUploads.insert(jobId, true);
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
    const auto files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Upload Files to %1").arg(m_currentPath), QDir::homePath());
    uploadPaths(files, m_currentPath);
}

void SftpPanel::uploadFolderDialog()
{
    const auto folder = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Upload Folder to %1").arg(m_currentPath), QDir::homePath());
    if (!folder.isEmpty())
        uploadPaths({folder}, m_currentPath);
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
                               ? QFileDialog::getExistingDirectory(this, QStringLiteral("Download To"),
                                                                   m_lastDownloadDirectory)
                               : defaultFolder;
    if (directory.isEmpty() || !m_session)
        return;
    m_lastDownloadDirectory = directory;
    const int jobId = m_session->download(paths, directory);
    m_jobUploads.insert(jobId, false);
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

void SftpPanel::updateProgress(int jobId, const QString &currentFile, quint64 bytesDone,
                               quint64 bytesTotal, int filesDone, int filesTotal, int queuedJobs)
{
    const bool upload = m_jobUploads.value(jobId, true);
    m_progressFooter->show();
    m_progressLabel->setText(
        currentFile.isEmpty()
            ? (upload ? QStringLiteral("Preparing upload…") : QStringLiteral("Preparing download…"))
            : QStringLiteral("%1 %2").arg(upload ? QStringLiteral("Uploading")
                                                 : QStringLiteral("Downloading"),
                                          currentFile));
    m_progressLabel->setToolTip(currentFile);

    const int value = bytesTotal > 0
        ? static_cast<int>(bytesDone * ProgressScale / bytesTotal)
        : (filesTotal > 0 ? filesDone * ProgressScale / filesTotal : 0);
    m_progressBar->setValue(value);

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

    QStringList details{QStringLiteral("%1 of %2").arg(formatSize(bytesDone),
                                                       formatSize(bytesTotal)),
                        QStringLiteral("%1/%2 files").arg(filesDone).arg(filesTotal)};
    if (m_bytesPerSecond > 0)
        details.append(QStringLiteral("%1/s").arg(formatSize(static_cast<quint64>(m_bytesPerSecond))));
    if (queuedJobs > 0)
        details.append(QStringLiteral("%1 queued").arg(queuedJobs));
    m_progressDetail->setText(details.join(QStringLiteral(" · ")));
}

void SftpPanel::askConflict(const QString &path, quint64 existingSize,
                            const QDateTime &existingModified, quint64 newSize,
                            const QDateTime &newModified, bool upload)
{
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
    if (m_jobUploads.isEmpty())
        resetTransfers();

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

void SftpPanel::resetTransfers()
{
    m_jobUploads.clear();
    m_speedJobId = 0;
    m_bytesPerSecond = 0;
    m_progressFooter->hide();
    m_progressBar->setValue(0);
}
