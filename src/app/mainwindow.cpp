// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "appdirs.h"
#include "mainwindow.h"
#include "icons.h"
#include "masterpassworddialog.h"
#include "preferenceswindow.h"
#include "rdppage.h"
#include "vncpage.h"
#include "remotescreenpage.h"
#include "sftppanel.h"
#include "terminalpage.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QColorDialog>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QDateTime>
#include <QMenuBar>
#include <QMessageBox>
#include <QFont>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSpinBox>
#include <QCheckBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QGuiApplication>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QMouseEvent>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <yaml-cpp/yaml.h>

namespace {
constexpr int ItemKindRole = Qt::UserRole;
constexpr int SessionIdRole = Qt::UserRole + 1;
constexpr int MaximumFolderDepth = 3;

// User state (sessions, preferences, vault, known hosts) lives in settings/
// next to the executable, so the application folder carries it.
QString stateDirectory()
{
    return portableFilePath(QStringLiteral("settings"));
}

QString stateFilePath(const QString &name)
{
    return QDir(stateDirectory()).filePath(name);
}

// Moves state written by earlier versions from data/ into settings/.
void migrateStateFromDataDirectory()
{
    const QDir legacy(portableFilePath(QStringLiteral("data")));
    for (const auto *name : {"sessions.yaml", "sessions.json", "settings.yaml",
                             "credentials.vault", "known_hosts"}) {
        const auto from = legacy.filePath(QString::fromLatin1(name));
        const auto to = stateFilePath(QString::fromLatin1(name));
        if (QFile::exists(from) && !QFile::exists(to) && QDir().mkpath(stateDirectory()))
            QFile::rename(from, to);
    }
}

// Application color themes beyond System, Light, Dark, and Custom: plain
// palette colors, applied the same way as the built-in Dark theme.
struct PalettePreset
{
    const char *key;
    const char *label;
    QRgb window, base, alternateBase, text, button, highlight, highlightedText, link,
        placeholder;
};

constexpr PalettePreset PalettePresets[] = {
    {"nord", "Nord", 0x2e3440, 0x3b4252, 0x434c5e, 0xeceff4, 0x3b4252, 0x88c0d0, 0x2e3440,
     0x81a1c1, 0x8f9bb3},
    {"solarized-dark", "Solarized Dark", 0x002b36, 0x073642, 0x0a3f4c, 0xeee8d5, 0x073642,
     0x268bd2, 0xfdf6e3, 0x2aa198, 0x839496},
    {"solarized-light", "Solarized Light", 0xeee8d5, 0xfdf6e3, 0xf5efdc, 0x073642, 0xeee8d5,
     0x268bd2, 0xfdf6e3, 0x2aa198, 0x657b83},
    {"dracula", "Dracula", 0x282a36, 0x1e1f29, 0x343746, 0xf8f8f2, 0x343746, 0xbd93f9,
     0x282a36, 0x8be9fd, 0x8b95c9},
    {"gruvbox-dark", "Gruvbox Dark", 0x282828, 0x1d2021, 0x32302f, 0xebdbb2, 0x3c3836,
     0xd79921, 0x282828, 0x83a598, 0xa89984},
    {"high-contrast", "High Contrast", 0x000000, 0x000000, 0x1a1a1a, 0xffffff, 0x000000,
     0xffff00, 0x000000, 0x00ffff, 0xd0d0d0},
};

const PalettePreset *findPalettePreset(const QString &key)
{
    for (const auto &preset : PalettePresets) {
        if (key == QLatin1String(preset.key))
            return &preset;
    }
    return nullptr;
}

QString settingsFilePath()
{
    return stateFilePath(QStringLiteral("settings.yaml"));
}

QString loadUiStylePreference()
{
    try {
        const auto settings = YAML::LoadFile(settingsFilePath().toStdString());
        const auto ui = settings["ui"];
        if (ui && ui["style"])
            return QString::fromStdString(ui["style"].as<std::string>());
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the current platform style.
    }
    return QStringLiteral("system");
}

QString loadColorThemePreference()
{
    try {
        const auto settings = YAML::LoadFile(settingsFilePath().toStdString());
        const auto ui = settings["ui"];
        if (ui && ui["color_theme"])
            return QString::fromStdString(ui["color_theme"].as<std::string>());
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the platform palette.
    }
    return QStringLiteral("system");
}

// Local shells need a pseudo-terminal; on Windows that will be ConPTY, which
// is not implemented yet. SSH sessions do not need one.
#ifdef Q_OS_WIN
constexpr bool LocalTerminalSupported = false;
#else
constexpr bool LocalTerminalSupported = true;
#endif

void disableLocalTerminalWhereUnsupported(QAction *action)
{
#ifdef Q_OS_WIN
    action->setEnabled(false);
    action->setToolTip(QStringLiteral("Local terminals are not available on Windows yet"));
#else
    Q_UNUSED(action);
#endif
}

QString loadIconThemePreference()
{
    try {
        const auto ui = YAML::LoadFile(settingsFilePath().toStdString())["ui"];
        if (ui && ui["icon_theme"])
            return QString::fromStdString(ui["icon_theme"].as<std::string>());
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the built-in icons.
    }
    return QString();
}

void saveIconThemePreference(const QString &themeName)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    settings["ui"]["icon_theme"] = themeName.toStdString();

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

QHash<QString, QColor> loadCustomColors()
{
    QHash<QString, QColor> colors;
    try {
        const auto settings = YAML::LoadFile(settingsFilePath().toStdString());
        const auto values = settings["ui"]["custom_colors"];
        for (const auto *key : {"window", "base", "text", "highlight"}) {
            if (values[key]) {
                const QColor color(QString::fromStdString(values[key].as<std::string>()));
                if (color.isValid())
                    colors.insert(QString::fromLatin1(key), color);
            }
        }
    } catch (const YAML::Exception &) {
        // Invalid custom colors are ignored.
    }
    return colors;
}

void saveColorThemePreference(const QString &themeKey,
                              const QHash<QString, QColor> &customColors)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    settings["ui"]["color_theme"] = themeKey.toStdString();
    for (auto iterator = customColors.cbegin(); iterator != customColors.cend(); ++iterator)
        settings["ui"]["custom_colors"][iterator.key().toStdString()] =
            iterator.value().name(QColor::HexRgb).toStdString();

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

void saveUiStylePreference(const QString &styleKey)
{
    const auto path = settingsFilePath();
    const auto directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if the file does not exist or is invalid.
    }
    settings["ui"]["style"] = styleKey.toStdString();

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

QHash<QString, bool> loadFolderExpansionStates()
{
    QHash<QString, bool> states;
    try {
        const auto settings = YAML::LoadFile(settingsFilePath().toStdString());
        const auto folders = settings["session_tree"]["folder_expansion"];
        if (!folders || !folders.IsMap())
            return states;
        for (const auto &entry : folders)
            states.insert(QString::fromStdString(entry.first.as<std::string>()),
                          entry.second.as<bool>(true));
    } catch (const YAML::Exception &) {
        // Missing or invalid tree state uses the default expanded folders.
    }
    return states;
}

void saveFolderExpansionStates(const QHash<QString, bool> &states)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    YAML::Node folders(YAML::NodeType::Map);
    for (auto iterator = states.cbegin(); iterator != states.cend(); ++iterator)
        folders[iterator.key().toStdString()] = iterator.value();
    settings["session_tree"]["folder_expansion"] = folders;

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

YAML::Node sessionToYaml(const SessionDefinition &session)
{
    YAML::Node node;
    node["id"] = session.id.toStdString();
    node["name"] = session.name.toStdString();
    node["protocol"] = session.protocol.toStdString();
    node["host"] = session.host.toStdString();
    node["port"] = static_cast<int>(session.port);
    node["username"] = session.username.toStdString();
    if (!session.terminal.isEmpty()) {
        YAML::Node terminal(YAML::NodeType::Map);
        if (!session.terminal.colorScheme.isEmpty())
            terminal["color_scheme"] = session.terminal.colorScheme.toStdString();
        if (!session.terminal.fontFamily.isEmpty())
            terminal["font_family"] = session.terminal.fontFamily.toStdString();
        if (session.terminal.fontSize > 0)
            terminal["font_size"] = session.terminal.fontSize;
        node["terminal"] = terminal;
    }
    if (!session.rdp.isEmpty()) {
        YAML::Node rdp(YAML::NodeType::Map);
        rdpOptionsToYaml(session.rdp, rdp);
        node["rdp"] = rdp;
    }
    if (!session.ssh.isEmpty()) {
        YAML::Node ssh(YAML::NodeType::Map);
        sshOptionsToYaml(session.ssh, ssh);
        node["ssh"] = ssh;
    }
    if (!session.vnc.isEmpty()) {
        YAML::Node vnc(YAML::NodeType::Map);
        vncOptionsToYaml(session.vnc, vnc);
        node["vnc"] = vnc;
    }
    return node;
}

TerminalAppearance terminalAppearanceFromYaml(const YAML::Node &node)
{
    TerminalAppearance appearance;
    if (!node || !node.IsMap())
        return appearance;
    appearance.colorScheme = QString::fromStdString(node["color_scheme"].as<std::string>(""));
    appearance.fontFamily = QString::fromStdString(node["font_family"].as<std::string>(""));
    appearance.fontSize = qBound(0, node["font_size"].as<int>(0), 72);
    return appearance;
}

// Interface font for menus, panels, and dialogs (terminals have their own).
// An empty family or a size of 0 keeps the system's choice.
struct InterfaceFont
{
    QString family;
    int pointSize = 0;
};

InterfaceFont loadInterfaceFont()
{
    InterfaceFont font;
    try {
        const auto ui = YAML::LoadFile(settingsFilePath().toStdString())["ui"];
        font.family = QString::fromStdString(ui["font_family"].as<std::string>(""));
        font.pointSize = qBound(0, ui["font_size"].as<int>(0), 48);
    } catch (const YAML::Exception &) {
        // Missing or invalid settings keep the system font.
    }
    return font;
}

void saveInterfaceFont(const InterfaceFont &font)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    settings["ui"]["font_family"] = font.family.toStdString();
    settings["ui"]["font_size"] = font.pointSize;

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

void applyInterfaceFont(const QFont &systemFont, const InterfaceFont &choice)
{
    QFont font = systemFont;
    if (!choice.family.isEmpty())
        font.setFamily(choice.family);
    if (choice.pointSize > 0)
        font.setPointSize(choice.pointSize);
    QApplication::setFont(font);
}

QString loadDarkTerminalColorScheme()
{
    try {
        const auto terminal = YAML::LoadFile(settingsFilePath().toStdString())["terminal"];
        if (terminal && terminal["dark_color_scheme"])
            return QString::fromStdString(terminal["dark_color_scheme"].as<std::string>());
    } catch (const YAML::Exception &) {
        // Missing or invalid settings: no separate dark-mode scheme.
    }
    return QString();
}

TerminalAppearance loadTerminalAppearance()
{
    try {
        return terminalAppearanceFromYaml(
            YAML::LoadFile(settingsFilePath().toStdString())["terminal"]);
    } catch (const YAML::Exception &) {
        // Missing or invalid settings use the built-in terminal appearance.
    }
    return {};
}

void saveTerminalAppearance(const TerminalAppearance &appearance)
{
    const auto path = settingsFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return;

    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
        // Start a fresh settings document if it does not exist or is invalid.
    }
    YAML::Node terminal(YAML::NodeType::Map);
    terminal["color_scheme"] = appearance.colorScheme.toStdString();
    terminal["font_family"] = appearance.fontFamily.toStdString();
    terminal["font_size"] = appearance.fontSize;
    // Keep keys other parts of the application store under terminal:.
    try {
        const auto existing = YAML::LoadFile(path.toStdString())["terminal"];
        if (existing && existing["dark_color_scheme"])
            terminal["dark_color_scheme"] = existing["dark_color_scheme"];
    } catch (const YAML::Exception &) {
    }
    settings["terminal"] = terminal;

    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size()) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

YAML::Node folderToYaml(const QTreeWidgetItem *folder,
                        const QHash<QString, SessionDefinition> &sessions)
{
    YAML::Node node;
    node["name"] = folder->text(0).toStdString();
    YAML::Node sessionNodes(YAML::NodeType::Sequence);
    YAML::Node folderNodes(YAML::NodeType::Sequence);
    for (int index = 0; index < folder->childCount(); ++index) {
        const auto *child = folder->child(index);
        if (child->data(0, ItemKindRole).toString() == QStringLiteral("folder")) {
            folderNodes.push_back(folderToYaml(child, sessions));
            continue;
        }
        const auto session = sessions.constFind(child->data(0, SessionIdRole).toString());
        if (session != sessions.cend())
            sessionNodes.push_back(sessionToYaml(*session));
    }
    if (sessionNodes.size() > 0)
        node["sessions"] = sessionNodes;
    if (folderNodes.size() > 0)
        node["folder"] = folderNodes;
    return node;
}

class SessionPage final : public QWidget
{
public:
    explicit SessionPage(const SessionDefinition &session, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(32, 28, 32, 28);

        auto *title = new QLabel(session.name, this);
        QFont titleFont = title->font();
        titleFont.setPointSize(titleFont.pointSize() + 5);
        titleFont.setBold(true);
        title->setFont(titleFont);

        auto *details = new QLabel(
            QStringLiteral("%1  ·  %2%3")
                .arg(session.protocol, session.host,
                     session.port == 0 ? QString() : QStringLiteral(":%1").arg(session.port)),
            this);
        details->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto *placeholder = new QLabel(
            QStringLiteral("This tab is reserved for one session.\n"
                           "The %1 connection backend has not been implemented yet.")
                .arg(session.protocol),
            this);
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

        layout->addWidget(title);
        layout->addWidget(details);
        layout->addStretch();
        layout->addWidget(placeholder, 0, Qt::AlignCenter);
        layout->addStretch();
    }
};

// Renaming in the session tree: the editor holds just the name (the list
// may show "name (host)"), and the new name goes through MainWindow.
class SessionNameDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    std::function<QString(const QModelIndex &index)> nameOf;
    std::function<void(const QModelIndex &index, const QString &name)> rename;

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

class ThemeColorDialog final : public QDialog
{
public:
    explicit ThemeColorDialog(const QHash<QString, QColor> &initialColors,
                              const QPalette &fallbackPalette,
                              QWidget *parent = nullptr)
        : QDialog(parent), m_colors(initialColors)
    {
        setWindowTitle(QStringLiteral("Customize Theme Colors"));
        auto *layout = new QFormLayout(this);
        addColorRow(layout, QStringLiteral("window"), QStringLiteral("Window"),
                    fallbackPalette.color(QPalette::Window));
        addColorRow(layout, QStringLiteral("base"), QStringLiteral("Input and panel background"),
                    fallbackPalette.color(QPalette::Base));
        addColorRow(layout, QStringLiteral("text"), QStringLiteral("Text"),
                    fallbackPalette.color(QPalette::Text));
        addColorRow(layout, QStringLiteral("highlight"), QStringLiteral("Selection and accent"),
                    fallbackPalette.color(QPalette::Highlight));

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

    QHash<QString, QColor> colors() const { return m_colors; }

private:
    void addColorRow(QFormLayout *layout, const QString &key, const QString &label,
                     const QColor &fallback)
    {
        auto color = m_colors.value(key, fallback);
        auto *button = new QPushButton(color.name(QColor::HexRgb), this);
        auto updateButton = [button](const QColor &value) {
            button->setText(value.name(QColor::HexRgb));
            button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                                      .arg(value.name(QColor::HexRgb),
                                           value.lightness() < 128 ? QStringLiteral("white")
                                                                   : QStringLiteral("black")));
        };
        updateButton(color);
        connect(button, &QPushButton::clicked, this, [this, key, color, button, updateButton]() mutable {
            const auto selected = QColorDialog::getColor(color, this,
                                                         QStringLiteral("Choose %1 color").arg(key));
            if (selected.isValid()) {
                color = selected;
                m_colors.insert(key, color);
                updateButton(color);
            }
        });
        layout->addRow(label, button);
    }

    QHash<QString, QColor> m_colors;
};
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_vault(stateFilePath(QStringLiteral("credentials.vault")))
{
    migrateStateFromDataDirectory();
    m_systemStyleKey = QApplication::style()->objectName();
    m_systemPalette = QApplication::palette();
    m_systemFont = QApplication::font();
    applyInterfaceFont(m_systemFont, loadInterfaceFont());
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
    m_terminalAppearance = loadTerminalAppearance();
    m_darkTerminalColorScheme = loadDarkTerminalColorScheme();
    m_behaviour = loadTerminalBehaviour();
    m_startup = loadStartupPreferences();
    m_shortcuts = loadShortcuts();
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
        if (auto *item = sessionTree->item(index))
            renameTreeItem(item, name);
    };
    sessionTree->setItemDelegate(nameDelegate);
    sessionTree->canMove = [this](const QTreeWidgetItem *item, const QTreeWidgetItem *newParent) {
        // Sessions always live in a folder; folders keep the nesting limit.
        if (!isFolder(item))
            return newParent && isFolder(newParent);
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
        statusBar()->showMessage(QStringLiteral("Moved %1").arg(item->text(0)), 3000);
    };
    m_sessionTree = sessionTree;
    m_sessionTree->setHeaderLabel(QStringLiteral("Saved sessions"));
    m_sessionTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_leftTabs->addTab(m_sessionTree, QStringLiteral("Sessions"));

    // Each SSH tab owns its own SFTP panel; the stack shows the active one.
    m_sftpStack = new QStackedWidget(m_leftTabs);
    m_sftpPlaceholder = new QLabel(QStringLiteral("Open an SSH session to browse its files here."),
                                   m_sftpStack);
    m_sftpPlaceholder->setAlignment(Qt::AlignCenter);
    m_sftpPlaceholder->setWordWrap(true);
    m_sftpPlaceholder->setMargin(12);
    m_sftpPlaceholder->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    m_sftpStack->addWidget(m_sftpPlaceholder);
    m_leftTabs->addTab(m_sftpStack, QStringLiteral("SFTP"));

    m_sessionTabs = new QTabWidget(splitter);
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
            m_sessionTabs->tabBar()->setTabTextColor(index, QColor());
    });
    m_sessionTabs->tabBar()->installEventFilter(this);
    m_sessionTabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sessionTabs->tabBar(), &QWidget::customContextMenuRequested,
            this, &MainWindow::showTabContextMenu);

    splitter->addWidget(m_leftTabs);
    splitter->addWidget(m_sessionTabs);
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
    restoreFolderExpansion();
    connect(m_sessionTree, &QTreeWidget::itemExpanded,
            this, [this](QTreeWidgetItem *) { saveFolderExpansion(); });
    connect(m_sessionTree, &QTreeWidget::itemCollapsed,
            this, [this](QTreeWidgetItem *) { saveFolderExpansion(); });

    statusBar()->showMessage(QStringLiteral("Ready — double-click a saved session to open its tab"));

    if (m_startup.rememberWindow) {
        const auto geometry = loadWindowGeometry();
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
    }
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

    m_broadcastIndicator = new QLabel(QStringLiteral("  BROADCAST  "), this);
    m_broadcastIndicator->setStyleSheet(QStringLiteral(
        "background: #c0392b; color: white; font-weight: bold; border-radius: 3px;"));
    m_broadcastIndicator->setToolTip(QStringLiteral("Typing goes to every connected SSH tab"));
    m_broadcastIndicator->hide();
    statusBar()->addPermanentWidget(m_broadcastIndicator);
}

void MainWindow::createMenus()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto *localTerminalAction = fileMenu->addAction(QStringLiteral("New Local Terminal"));
    setThemedIcon(localTerminalAction, QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    bindShortcut(localTerminalAction, QStringLiteral("new_local_terminal"));
    connect(localTerminalAction, &QAction::triggered, this, &MainWindow::openLocalTerminal);
    disableLocalTerminalWhereUnsupported(localTerminalAction);
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
    auto *deleteFolderAction = sessionsMenu->addAction(QStringLiteral("Delete Folder…"));
    setThemedIcon(deleteFolderAction, QStringLiteral("edit-delete"), QStyle::SP_TrashIcon);
    bindShortcut(deleteFolderAction, QStringLiteral("delete_folder"));
    connect(deleteFolderAction, &QAction::triggered, this, &MainWindow::deleteSelectedFolder);
    auto *addSessionAction = sessionsMenu->addAction(QStringLiteral("New…"));
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
        if (auto *item = m_sessionTree->currentItem())
            m_sessionTree->editItem(item, 0);
    });
    auto *duplicateAction = sessionsMenu->addAction(QStringLiteral("Duplicate"), this, [this] {
        duplicateSession(m_sessionTree->currentItem());
    });
    connect(sessionsMenu, &QMenu::aboutToShow, this, [this, duplicateAction] {
        auto *item = m_sessionTree->currentItem();
        duplicateAction->setEnabled(item && !isFolder(item));
    });
    auto *snippetsMenu = sessionsMenu->addMenu(QStringLiteral("Send Snippet"));
    snippetsMenu->setIcon(themedIcon(QStringLiteral("insert-text"), QStyle::SP_FileDialogContentsView));
    connect(snippetsMenu, &QMenu::aboutToShow, this, [this, snippetsMenu] {
        snippetsMenu->clear();
        auto *page = qobject_cast<TerminalPage *>(m_sessionTabs->currentWidget());
        for (const auto &snippet : std::as_const(m_snippets)) {
            auto *action = snippetsMenu->addAction(snippet.name, this, [page, snippet] {
                if (page)
                    page->typeText(snippet.text + (snippet.pressEnter ? QStringLiteral("\r") : QString()));
            });
            action->setEnabled(page);
        }
        if (!m_snippets.isEmpty())
            snippetsMenu->addSeparator();
        snippetsMenu->addAction(QStringLiteral("Edit Snippets…"), this,
                                [this] { showPreferences(QStringLiteral("snippets")); });
    });
    sessionsMenu->addSeparator();
    auto *importAction = sessionsMenu->addAction(QStringLiteral("Import from YAML…"));
    setThemedIcon(importAction, QStringLiteral("document-import"), QStyle::SP_DialogOpenButton);
    connect(importAction, &QAction::triggered, this, &MainWindow::importSessions);
    auto *exportAction = sessionsMenu->addAction(QStringLiteral("Export to YAML…"));
    setThemedIcon(exportAction, QStringLiteral("document-export"), QStyle::SP_DialogSaveButton);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportSessions);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    auto *sessionsPanelAction = viewMenu->addAction(QStringLiteral("Sessions and SFTP Panel"));
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
    auto *desktopFullScreen = viewMenu->addAction(QStringLiteral("Remote Desktop Full Screen"));
    desktopFullScreen->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Return));
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
    auto addTabAction = [this](const QString &text, const QString &id, auto slot) {
        auto *action = new QAction(text, this);
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
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
    addTabAction(QStringLiteral("Close Tab"), QStringLiteral("close_tab"), [this] {
        if (m_sessionTabs->currentIndex() >= 0)
            closeSessionTab(m_sessionTabs->currentIndex());
    });
    addTabAction(QStringLiteral("Duplicate Tab"), QStringLiteral("duplicate_tab"), [this] {
        if (m_sessionTabs->currentIndex() >= 0)
            duplicateTab(m_sessionTabs->currentIndex());
    });

    auto *uiFeelMenu = viewMenu->addMenu(QStringLiteral("UI Feel"));
    auto *uiFeelGroup = new QActionGroup(this);
    uiFeelGroup->setExclusive(true);
    m_styleGroup = uiFeelGroup;

    auto addStyleAction = [this, uiFeelMenu, uiFeelGroup](
                              const QString &label, const QString &styleKey) {
        auto *action = uiFeelMenu->addAction(label);
        action->setCheckable(true);
        action->setData(styleKey);
        uiFeelGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, action] {
            setUiStyle(action->data().toString());
        });
        return action;
    };

    auto *systemStyleAction = addStyleAction(QStringLiteral("System Default"),
                                             QStringLiteral("system"));
    systemStyleAction->setChecked(m_selectedStyleKey == QStringLiteral("system")
                                  || !QStyleFactory::keys().contains(m_selectedStyleKey,
                                                                    Qt::CaseInsensitive));

    for (const auto &styleKey : QStyleFactory::keys()) {
        auto *action = addStyleAction(styleKey, styleKey);
        action->setChecked(m_selectedStyleKey.compare(styleKey, Qt::CaseInsensitive) == 0);
    }

    viewMenu->addSeparator();
    auto *iconThemeMenu = viewMenu->addMenu(QStringLiteral("Icon Theme"));
    auto *iconThemeGroup = new QActionGroup(this);
    iconThemeGroup->setExclusive(true);
    m_iconThemeGroup = iconThemeGroup;
    auto addIconThemeAction = [this, iconThemeMenu, iconThemeGroup](const QString &label,
                                                                     const QString &themeName) {
        auto *action = iconThemeMenu->addAction(label);
        action->setCheckable(true);
        action->setData(themeName);
        action->setChecked(themeName == m_iconTheme);
        iconThemeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, themeName] {
            setIconThemeChoice(themeName);
        });
    };
    addIconThemeAction(QStringLiteral("Built-in"), QString());
    iconThemeMenu->addSeparator();
    for (const auto &theme : availableIconThemes())
        addIconThemeAction(theme.second, theme.first);
    viewMenu->addSeparator();

    auto *colorThemeMenu = viewMenu->addMenu(QStringLiteral("Color Theme"));
    auto *colorThemeGroup = new QActionGroup(this);
    colorThemeGroup->setExclusive(true);
    m_colorThemeGroup = colorThemeGroup;
    auto addColorThemeAction = [this, colorThemeMenu, colorThemeGroup](
                                   const QString &label, const QString &themeKey) {
        auto *action = colorThemeMenu->addAction(label);
        action->setCheckable(true);
        action->setData(themeKey);
        colorThemeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, action, colorThemeGroup] {
            const auto themeKey = action->data().toString();
            if (themeKey == QStringLiteral("custom")) {
                editCustomColors();
                syncMenuChecks();
                return;
            }
            setColorTheme(themeKey);
        });
        return action;
    };

    auto *autoThemeAction = addColorThemeAction(QStringLiteral("Automatic (follow OS)"),
                                                QStringLiteral("auto"));
    auto *systemThemeAction = addColorThemeAction(QStringLiteral("System"), QStringLiteral("system"));
    auto *lightThemeAction = addColorThemeAction(QStringLiteral("Light"), QStringLiteral("light"));
    auto *darkThemeAction = addColorThemeAction(QStringLiteral("Dark"), QStringLiteral("dark"));
    colorThemeMenu->addSeparator();
    for (const auto &preset : PalettePresets) {
        auto *action = addColorThemeAction(QString::fromLatin1(preset.label),
                                           QString::fromLatin1(preset.key));
        action->setChecked(m_colorThemeKey == QLatin1String(preset.key));
    }
    colorThemeMenu->addSeparator();
    auto *customThemeAction = addColorThemeAction(QStringLiteral("Customize Colors…"),
                                                 QStringLiteral("custom"));
    autoThemeAction->setChecked(m_colorThemeKey == QStringLiteral("auto"));
    systemThemeAction->setChecked(m_colorThemeKey == QStringLiteral("system"));
    lightThemeAction->setChecked(m_colorThemeKey == QStringLiteral("light"));
    darkThemeAction->setChecked(m_colorThemeKey == QStringLiteral("dark"));
    customThemeAction->setChecked(m_colorThemeKey == QStringLiteral("custom"));

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

void MainWindow::applyUiStyle(const QString &styleKey)
{
    QString key = styleKey;
    if (key == QStringLiteral("system"))
        key = m_systemStyleKey;

    auto *style = QStyleFactory::create(key);
    if (!style && key.compare(m_systemStyleKey, Qt::CaseInsensitive) != 0)
        style = QStyleFactory::create(m_systemStyleKey);
    if (!style)
        return;

    QApplication::setStyle(style);
    applyColorTheme(m_colorThemeKey);
}

void MainWindow::applyColorTheme(const QString &requestedKey)
{
    // Automatic: the light or dark palette, following the OS setting.
    const QString themeKey = requestedKey != QStringLiteral("auto")
        ? requestedKey
        : QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
            ? QStringLiteral("dark")
            : QStringLiteral("light");
    QPalette palette = m_systemPalette;
    if (themeKey == QStringLiteral("dark")) {
        const QColor window(45, 45, 48);
        const QColor base(30, 30, 30);
        const QColor text(235, 235, 235);
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, base);
        palette.setColor(QPalette::AlternateBase, QColor(45, 45, 48));
        palette.setColor(QPalette::ToolTipBase, QColor(245, 245, 245));
        palette.setColor(QPalette::ToolTipText, QColor(25, 25, 25));
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, window);
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Link, QColor(80, 160, 255));
        palette.setColor(QPalette::LinkVisited, QColor(190, 120, 220));
        palette.setColor(QPalette::PlaceholderText, QColor(150, 150, 150));
    } else if (const auto *preset = findPalettePreset(themeKey)) {
        const QColor text(preset->text);
        palette.setColor(QPalette::Window, QColor(preset->window));
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, QColor(preset->base));
        palette.setColor(QPalette::AlternateBase, QColor(preset->alternateBase));
        palette.setColor(QPalette::ToolTipBase, QColor(preset->base));
        palette.setColor(QPalette::ToolTipText, text);
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, QColor(preset->button));
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Highlight, QColor(preset->highlight));
        palette.setColor(QPalette::HighlightedText, QColor(preset->highlightedText));
        palette.setColor(QPalette::Link, QColor(preset->link));
        palette.setColor(QPalette::LinkVisited, QColor(preset->link).darker(120));
        palette.setColor(QPalette::PlaceholderText, QColor(preset->placeholder));
        // Keep disabled controls readable against the preset's background.
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(QPalette::Disabled, role, QColor(preset->placeholder));
    } else if (themeKey == QStringLiteral("light")) {
        palette.setColor(QPalette::Window, QColor(245, 245, 245));
        palette.setColor(QPalette::WindowText, QColor(30, 30, 30));
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::AlternateBase, QColor(238, 238, 238));
        palette.setColor(QPalette::Text, QColor(30, 30, 30));
        palette.setColor(QPalette::Button, QColor(245, 245, 245));
        palette.setColor(QPalette::ButtonText, QColor(30, 30, 30));
        palette.setColor(QPalette::Highlight, QColor(45, 125, 210));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::PlaceholderText, QColor(120, 120, 120));
    } else if (themeKey == QStringLiteral("custom")) {
        const auto window = m_customColors.value(QStringLiteral("window"),
                                                 palette.color(QPalette::Window));
        const auto base = m_customColors.value(QStringLiteral("base"),
                                               palette.color(QPalette::Base));
        const auto text = m_customColors.value(QStringLiteral("text"),
                                               palette.color(QPalette::Text));
        const auto highlight = m_customColors.value(QStringLiteral("highlight"),
                                                    palette.color(QPalette::Highlight));
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, base);
        palette.setColor(QPalette::AlternateBase, window.darker(108));
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, window);
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::Highlight, highlight);
        palette.setColor(QPalette::HighlightedText,
                         highlight.lightness() < 128 ? Qt::white : Qt::black);
        palette.setColor(QPalette::PlaceholderText, text);
    }
    QApplication::setPalette(palette);
}

void MainWindow::editCustomColors()
{
    ThemeColorDialog dialog(m_customColors, QApplication::palette(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_customColors = dialog.colors();
    setColorTheme(QStringLiteral("custom"));
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
}

void MainWindow::addExampleSessions()
{
    auto *examples = new QTreeWidgetItem(m_sessionTree, {QStringLiteral("Examples")});
    setItemKind(examples, ItemKind::Folder);
    auto *linuxFolder = new QTreeWidgetItem(examples, {QStringLiteral("Linux")});
    setItemKind(linuxFolder, ItemKind::Folder);
    auto *servers = new QTreeWidgetItem(linuxFolder, {QStringLiteral("Servers")});
    setItemKind(servers, ItemKind::Folder);

    SessionDefinition sample;
    sample.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sample.name = QStringLiteral("Example SSH Session");
    sample.protocol = QStringLiteral("SSH");
    sample.host = QStringLiteral("server.example.com");
    sample.port = 22;
    addSession(sample, servers);

    examples->setExpanded(true);
    linuxFolder->setExpanded(true);
    servers->setExpanded(true);
}

void MainWindow::loadSessions()
{
    const auto dataDirectory = stateDirectory();
    const auto yamlPath = QDir(dataDirectory).filePath(QStringLiteral("sessions.yaml"));
    const auto legacyJsonPath = QDir(dataDirectory).filePath(QStringLiteral("sessions.json"));
    const bool importingLegacyJson = !QFile::exists(yamlPath) && QFile::exists(legacyJsonPath);
    const auto sourcePath = importingLegacyJson ? legacyJsonPath : yamlPath;
    bool migrateToCompactFormat = importingLegacyJson;

    if (!QFile::exists(sourcePath)) {
        addExampleSessions();
        saveSessions();
        return;
    }

    try {
        const auto document = YAML::LoadFile(sourcePath.toStdString());
        const auto version = document["version"].as<int>(1);
        migrateToCompactFormat = migrateToCompactFormat || version < 2;
        QString error;
        const bool loaded = version == 2
            ? appendCompactSessionYaml(document["folder"], nullptr, &error)
            : appendSessionYaml(document["items"], nullptr, &error);
        if (!loaded)
            throw YAML::Exception(YAML::Mark::null_mark(), error.toStdString());
    } catch (const YAML::Exception &error) {
        QMessageBox::warning(this, QStringLiteral("Invalid sessions file"),
                             QStringLiteral("Could not read %1: %2")
                                 .arg(sourcePath, QString::fromLocal8Bit(error.what())));
        return;
    }

    if (migrateToCompactFormat)
        saveSessions();
}

bool MainWindow::appendSessionYaml(const YAML::Node &roots, QTreeWidgetItem *parent,
                                  QString *error)
{
    if (!roots || !roots.IsSequence()) {
        if (error)
            *error = QStringLiteral("The YAML document must contain an 'items' sequence.");
        return false;
    }

    QSet<QString> importedIds;
    const int startingDepth = parent ? folderDepth(parent) : 0;
    std::function<bool(const YAML::Node &, int)> validate;
    validate = [&](const YAML::Node &node, int depth) {
        if (!node.IsMap()) {
            if (error) *error = QStringLiteral("Each session or folder must be a YAML mapping.");
            return false;
        }
        if (node["password"] || node["credential"] || node["credentials"]) {
            if (error) *error = QStringLiteral("Passwords cannot be imported from session YAML. Use the credential vault.");
            return false;
        }

        const auto kind = QString::fromStdString(node["kind"].as<std::string>(""));
        if (kind == QStringLiteral("folder")) {
            const auto name = QString::fromStdString(node["name"].as<std::string>("" )).trimmed();
            if (name.isEmpty() || depth + 1 > MaximumFolderDepth) {
                if (error) *error = QStringLiteral("Folder names must be set and nesting cannot exceed three levels.");
                return false;
            }
            const auto children = node["children"];
            if (children && !children.IsSequence()) {
                if (error) *error = QStringLiteral("Folder children must be a YAML sequence.");
                return false;
            }
            if (children) {
                for (const auto &child : children) {
                    if (!validate(child, depth + 1))
                        return false;
                }
            }
            return true;
        }
        if (kind != QStringLiteral("session")) {
            if (error) *error = QStringLiteral("Unknown item kind in session YAML.");
            return false;
        }

        const auto value = node["session"];
        if (!value || !value.IsMap() || value["password"] || value["credential"]
            || value["credentials"]) {
            if (error) *error = QStringLiteral("Session data must be a mapping without inline credentials.");
            return false;
        }
        const auto name = QString::fromStdString(value["name"].as<std::string>(
            node["name"].as<std::string>(""))).trimmed();
        const auto protocol = QString::fromStdString(value["protocol"].as<std::string>(""));
        const auto host = QString::fromStdString(value["host"].as<std::string>(""));
        if (name.isEmpty() || protocol.isEmpty() || host.isEmpty()) {
            if (error) *error = QStringLiteral("Every session needs a name, protocol, and host.");
            return false;
        }
        const auto port = value["port"].as<int>(0);
        if (port < 0 || port > 65535) {
            if (error) *error = QStringLiteral("A session port must be between 0 and 65535.");
            return false;
        }
        const auto id = QString::fromStdString(value["id"].as<std::string>(""));
        if (!id.isEmpty()) {
            if (importedIds.contains(id)) {
                if (error) *error = QStringLiteral("Session IDs must be unique in an import file.");
                return false;
            }
            importedIds.insert(id);
        }
        return true;
    };

    for (const auto &root : roots) {
        if (!validate(root, startingDepth))
            return false;
    }

    std::function<void(const YAML::Node &, QTreeWidgetItem *)> add;
    add = [this, &add](const YAML::Node &node, QTreeWidgetItem *destination) {
        const auto kind = QString::fromStdString(node["kind"].as<std::string>());
        if (kind == QStringLiteral("folder")) {
            const auto name = QString::fromStdString(node["name"].as<std::string>());
            auto *folder = destination ? new QTreeWidgetItem(destination, {name})
                                       : new QTreeWidgetItem(m_sessionTree, {name});
            setItemKind(folder, ItemKind::Folder);
            const auto children = node["children"];
            if (children) {
                for (const auto &child : children)
                    add(child, folder);
            }
            folder->setExpanded(true);
            return;
        }

        const auto value = node["session"];
        SessionDefinition session;
        session.id = QString::fromStdString(value["id"].as<std::string>(""));
        if (session.id.isEmpty() || m_sessions.contains(session.id))
            session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        session.name = QString::fromStdString(value["name"].as<std::string>(
            node["name"].as<std::string>("")));
        session.protocol = QString::fromStdString(value["protocol"].as<std::string>());
        session.host = QString::fromStdString(value["host"].as<std::string>());
        session.port = static_cast<quint16>(value["port"].as<int>(0));
        session.username = QString::fromStdString(value["username"].as<std::string>(""));
        addSession(session, destination);
    };

    for (const auto &root : roots)
        add(root, parent);
    return true;
}

bool MainWindow::appendCompactSessionYaml(const YAML::Node &folders, QTreeWidgetItem *parent,
                                          QString *error)
{
    if (!folders || !folders.IsSequence()) {
        if (error)
            *error = QStringLiteral("The YAML document must contain a 'folder' sequence.");
        return false;
    }

    QSet<QString> importedIds;
    const int startingDepth = parent ? folderDepth(parent) : 0;
    std::function<bool(const YAML::Node &, int)> validateFolder;
    validateFolder = [&](const YAML::Node &folder, int depth) {
        if (!folder.IsMap()) {
            if (error) *error = QStringLiteral("Each folder must be a YAML mapping.");
            return false;
        }
        if (folder["password"] || folder["credential"] || folder["credentials"]) {
            if (error) *error = QStringLiteral("Passwords cannot be imported from session YAML.");
            return false;
        }
        const auto name = QString::fromStdString(folder["name"].as<std::string>("")).trimmed();
        if (name.isEmpty() || depth + 1 > MaximumFolderDepth) {
            if (error) *error = QStringLiteral("Folder names must be set and nesting cannot exceed three levels.");
            return false;
        }
        const auto sessions = folder["sessions"];
        if (sessions && !sessions.IsSequence()) {
            if (error) *error = QStringLiteral("Folder sessions must be a YAML sequence.");
            return false;
        }
        if (sessions) {
            for (const auto &session : sessions) {
                if (!session.IsMap() || session["password"] || session["credential"]
                    || session["credentials"]) {
                    if (error) *error = QStringLiteral("Session data must be a mapping without inline credentials.");
                    return false;
                }
                const auto sessionName = QString::fromStdString(
                    session["name"].as<std::string>("")).trimmed();
                const auto protocol = QString::fromStdString(session["protocol"].as<std::string>(""));
                const auto host = QString::fromStdString(session["host"].as<std::string>(""));
                const auto port = session["port"].as<int>(0);
                if (sessionName.isEmpty() || protocol.isEmpty() || host.isEmpty()
                    || port < 0 || port > 65535) {
                    if (error) *error = QStringLiteral("Every session needs a name, protocol, host, and valid port.");
                    return false;
                }
                const auto id = QString::fromStdString(session["id"].as<std::string>(""));
                if (!id.isEmpty()) {
                    if (importedIds.contains(id)) {
                        if (error) *error = QStringLiteral("Session IDs must be unique in an import file.");
                        return false;
                    }
                    importedIds.insert(id);
                }
            }
        }
        const auto children = folder["folder"];
        if (children && !children.IsSequence()) {
            if (error) *error = QStringLiteral("Child folders must be a YAML sequence named 'folder'.");
            return false;
        }
        if (children) {
            for (const auto &child : children) {
                if (!validateFolder(child, depth + 1))
                    return false;
            }
        }
        return true;
    };

    for (const auto &folder : folders) {
        if (!validateFolder(folder, startingDepth))
            return false;
    }

    std::function<void(const YAML::Node &, QTreeWidgetItem *)> addFolder;
    addFolder = [this, &addFolder](const YAML::Node &node, QTreeWidgetItem *destination) {
        const auto name = QString::fromStdString(node["name"].as<std::string>());
        auto *folder = destination ? new QTreeWidgetItem(destination, {name})
                                   : new QTreeWidgetItem(m_sessionTree, {name});
        setItemKind(folder, ItemKind::Folder);
        const auto sessions = node["sessions"];
        if (sessions) {
            for (const auto &value : sessions) {
                SessionDefinition session;
                session.id = QString::fromStdString(value["id"].as<std::string>(""));
                if (session.id.isEmpty() || m_sessions.contains(session.id))
                    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                session.name = QString::fromStdString(value["name"].as<std::string>());
                session.protocol = QString::fromStdString(value["protocol"].as<std::string>());
                session.host = QString::fromStdString(value["host"].as<std::string>());
                session.port = static_cast<quint16>(value["port"].as<int>(0));
                session.username = QString::fromStdString(value["username"].as<std::string>(""));
                session.terminal = terminalAppearanceFromYaml(value["terminal"]);
                session.rdp = rdpOptionsFromYaml(value["rdp"]);
                session.vnc = vncOptionsFromYaml(value["vnc"]);
                session.ssh = sshOptionsFromYaml(value["ssh"]);
                addSession(session, folder);
            }
        }
        const auto children = node["folder"];
        if (children) {
            for (const auto &child : children)
                addFolder(child, folder);
        }
        folder->setExpanded(true);
    };

    for (const auto &folder : folders)
        addFolder(folder, parent);
    return true;
}

void MainWindow::importSessions()
{
    const auto filePath = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import YAML Sessions"), QString(),
        QStringLiteral("YAML files (*.yaml *.yml);;All files (*)"));
    if (filePath.isEmpty())
        return;

    try {
        const auto document = YAML::LoadFile(filePath.toStdString());
        const auto version = document["version"].as<int>(1);
        QString error;
        const bool imported = version == 2
            ? appendCompactSessionYaml(document["folder"], selectedFolder(), &error)
            : appendSessionYaml(document["items"], selectedFolder(), &error);
        if (!imported) {
            QMessageBox::warning(this, QStringLiteral("Could not import sessions"), error);
            return;
        }
        saveSessions();
        statusBar()->showMessage(QStringLiteral("Imported session configuration"), 3000);
    } catch (const YAML::Exception &error) {
        QMessageBox::warning(this, QStringLiteral("Invalid YAML file"),
                             QString::fromLocal8Bit(error.what()));
    }
}

void MainWindow::exportSessions()
{
    const auto filePath = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export YAML Sessions"), QStringLiteral("sessions.yaml"),
        QStringLiteral("YAML files (*.yaml *.yml)"));
    if (filePath.isEmpty())
        return;

    YAML::Node folders(YAML::NodeType::Sequence);
    const auto *selected = m_sessionTree->currentItem();
    if (selected) {
        const auto *folder = isFolder(selected) ? selected : selected->parent();
        if (folder)
            folders.push_back(folderToYaml(folder, m_sessions));
    } else {
        for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
            folders.push_back(folderToYaml(m_sessionTree->topLevelItem(index), m_sessions));
    }
    YAML::Node document;
    document["version"] = 2;
    document["folder"] = folders;
    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << document;

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Could not export sessions"), file.errorString());
        return;
    }
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Could not export sessions"), file.errorString());
        return;
    }
    QMessageBox::information(this, QStringLiteral("Sessions exported"),
                             QStringLiteral("Session configuration was exported. Passwords are never included in YAML; they remain in the encrypted credential vault."));
}

bool MainWindow::ensureVaultUnlocked()
{
    if (m_vault.isUnlocked())
        return true;

    bool accepted = false;
    if (m_vault.exists()) {
        constexpr int MaximumUnlockAttempts = 3;
        QString error;
        for (int attempt = 1; attempt <= MaximumUnlockAttempts; ++attempt) {
            const auto label = error.isEmpty()
                ? QStringLiteral("Master password:")
                : QStringLiteral("%1\nMaster password (attempt %2 of %3):")
                      .arg(error).arg(attempt).arg(MaximumUnlockAttempts);
            auto masterPassword = QInputDialog::getText(
                this, QStringLiteral("Unlock Credential Vault"), label, QLineEdit::Password,
                QString(), &accepted);
            if (!accepted)
                return false;
            const bool unlocked = m_vault.unlock(masterPassword, &error);
            masterPassword.fill(QChar(0));
            if (unlocked) {
                statusBar()->showMessage(QStringLiteral("Credential vault unlocked"), 3000);
                return true;
            }
        }
        QMessageBox::warning(this, QStringLiteral("Could not unlock vault"), error);
        return false;
    }

    MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Create, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    auto masterPassword = dialog.newPassword();
    QString error;
    const bool created = m_vault.initialize(masterPassword, &error);
    masterPassword.fill(QChar(0));
    if (!created) {
        QMessageBox::warning(this, QStringLiteral("Could not create vault"), error);
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Credential vault created and unlocked"), 3000);
    return true;
}

void MainWindow::saveSessions()
{
    const auto dataDirectory = stateDirectory();
    if (!QDir().mkpath(dataDirectory)) {
        QMessageBox::warning(this, QStringLiteral("Could not save sessions"),
                             QStringLiteral("Could not create the settings folder next to the application."));
        return;
    }

    YAML::Node folders(YAML::NodeType::Sequence);
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        folders.push_back(folderToYaml(m_sessionTree->topLevelItem(index), m_sessions));
    YAML::Node document;
    document["version"] = 2;
    document["folder"] = folders;
    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << document;

    QSaveFile file(QDir(dataDirectory).filePath(QStringLiteral("sessions.yaml")));
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Could not save sessions"), file.errorString());
        return;
    }
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) != contents.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Could not save sessions"), file.errorString());
    }
}

QTreeWidgetItem *MainWindow::selectedFolder() const
{
    auto *item = m_sessionTree->currentItem();
    if (!item)
        return nullptr;
    if (isFolder(item))
        return item;
    return item->parent();
}

int MainWindow::folderDepth(const QTreeWidgetItem *folder) const
{
    int depth = 0;
    for (auto *item = folder; item; item = item->parent()) {
        if (isFolder(item))
            ++depth;
    }
    return depth;
}

void MainWindow::createFolder()
{
    auto *parent = selectedFolder();
    const int newDepth = parent ? folderDepth(parent) + 1 : 1;
    if (newDepth > MaximumFolderDepth) {
        QMessageBox::information(this, QStringLiteral("Folder depth limit"),
                                 QStringLiteral("Folders can be nested up to three levels."));
        return;
    }

    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("New Folder"),
                                            QStringLiteral("Folder name:"), QLineEdit::Normal,
                                            QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;

    addFolder(name, parent);
    saveSessions();
}

void MainWindow::deleteSelectedFolder()
{
    auto *folder = m_sessionTree->currentItem();
    if (!folder || !isFolder(folder)) {
        QMessageBox::information(this, QStringLiteral("Delete Folder"),
                                 QStringLiteral("Select a folder first."));
        return;
    }

    int folderCount = 0;
    int sessionCount = 0;
    QSet<QString> sessionIds;
    std::function<void(QTreeWidgetItem *)> inspect = [this, &folderCount, &sessionCount,
                                                       &sessionIds, &inspect](QTreeWidgetItem *item) {
        if (isFolder(item))
            ++folderCount;
        else {
            ++sessionCount;
            sessionIds.insert(item->data(0, SessionIdRole).toString());
        }
        for (int index = 0; index < item->childCount(); ++index)
            inspect(item->child(index));
    };
    inspect(folder);

    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("Delete Folder"));
    confirmation.setText(QStringLiteral("Delete \"%1\" and its contents?").arg(folder->text(0)));
    confirmation.setInformativeText(
        QStringLiteral("This removes %1 folder(s) and %2 saved session(s) from the session configuration.")
            .arg(folderCount).arg(sessionCount));
    auto *deleteButton = confirmation.addButton(QStringLiteral("Delete Folder"),
                                                QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != deleteButton)
        return;

    const auto folderName = folder->text(0);

    for (int index = m_sessionTabs->count() - 1; index >= 0; --index) {
        auto *page = m_sessionTabs->widget(index);
        if (page && sessionIds.contains(page->property("sessionId").toString()))
            closeSessionTab(index);
    }
    for (const auto &sessionId : sessionIds)
        m_sessions.remove(sessionId);

    auto *parent = folder->parent();
    if (parent)
        delete parent->takeChild(parent->indexOfChild(folder));
    else
        delete m_sessionTree->takeTopLevelItem(m_sessionTree->indexOfTopLevelItem(folder));
    saveSessions();
    saveFolderExpansion();
    statusBar()->showMessage(QStringLiteral("Deleted folder: %1").arg(folderName), 3000);
}

void MainWindow::createSession()
{
    auto *parentFolder = selectedFolder();
    if (!parentFolder) {
        QMessageBox::information(this, QStringLiteral("New Session"),
                                 QStringLiteral("Create or select a folder before adding a session."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New Session"));
    // Wide enough for the terminal preview and the settings tabs.
    dialog.setMinimumWidth(560);
    auto *form = new QFormLayout(&dialog);

    auto *nameEdit = new QLineEdit(&dialog);
    auto *protocolCombo = new QComboBox(&dialog);
    protocolCombo->addItems({QStringLiteral("SSH"), QStringLiteral("RDP"), QStringLiteral("VNC")});
    auto *hostEdit = new QLineEdit(&dialog);
    hostEdit->setPlaceholderText(QStringLiteral("hostname or IP address"));
    auto *portEdit = new QLineEdit(&dialog);
    portEdit->setPlaceholderText(QStringLiteral("Use protocol default"));
    auto *usernameEdit = new QLineEdit(&dialog);
    auto *passwordEdit = new QLineEdit(&dialog);
    passwordEdit->setEchoMode(QLineEdit::Password);

    form->addRow(QStringLiteral("Name:"), nameEdit);
    form->addRow(QStringLiteral("Protocol:"), protocolCombo);
    form->addRow(QStringLiteral("Host:"), hostEdit);
    form->addRow(QStringLiteral("Port:"), portEdit);
    form->addRow(QStringLiteral("Username:"), usernameEdit);
    form->addRow(QStringLiteral("Password (optional):"), passwordEdit);
    SshOptionsEditor *sshEditor = nullptr;
    auto *appearanceEditor = addTerminalAppearanceGroup(form, protocolCombo, {}, {}, &sshEditor);
    auto *rdpEditor = addRdpOptionsGroup(form, protocolCombo, {});
    auto *vncEditor = addVncOptionsGroup(form, protocolCombo, {});

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    bool portOk = true;
    const auto portText = portEdit->text().trimmed();
    const auto portValue = portText.isEmpty() ? 0 : portText.toUShort(&portOk);
    if (nameEdit->text().trimmed().isEmpty() || hostEdit->text().trimmed().isEmpty() || !portOk) {
        QMessageBox::warning(this, QStringLiteral("Invalid session"),
                             QStringLiteral("Enter a session name, host, and a valid port number."));
        return;
    }

    SessionDefinition session;
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.name = nameEdit->text().trimmed();
    session.protocol = protocolCombo->currentText();
    session.host = hostEdit->text().trimmed();
    session.port = portValue;
    session.username = usernameEdit->text().trimmed();
    if (session.protocol == QStringLiteral("SSH")) {
        session.terminal = appearanceEditor->value();
        session.ssh = sshEditor->value();
    }
    if (session.protocol == QStringLiteral("RDP"))
        session.rdp = rdpEditor->value();
    if (session.protocol == QStringLiteral("VNC"))
        session.vnc = vncEditor->value();
    auto sessionPassword = passwordEdit->text();
    if (!sessionPassword.isEmpty()) {
        if (!ensureVaultUnlocked())
            return;
        QString vaultError;
        if (!m_vault.setPassword(session.id, sessionPassword, &vaultError)) {
            QMessageBox::warning(this, QStringLiteral("Could not save password"), vaultError);
            return;
        }
        sessionPassword.fill(QChar(0));
        passwordEdit->clear();
    }
    addSession(session, parentFolder);
    saveSessions();
}

void MainWindow::openLocalTerminal()
{
    auto *page = new TerminalPage(effectiveTerminalAppearance(), m_sessionTabs);
    connectTerminalPage(page);
    applyTabSettings(page, QStringLiteral("local"));
    addSessionTab(page, QStringLiteral("Local Terminal"), QStringLiteral("Local shell"));
}

void MainWindow::openSelectedSession()
{
    auto *item = m_sessionTree->currentItem();
    if (!item || isFolder(item)) {
        statusBar()->showMessage(QStringLiteral("Select a saved session first"), 3000);
        return;
    }

    const auto sessionId = item->data(0, SessionIdRole).toString();
    const auto session = m_sessions.constFind(sessionId);
    if (session != m_sessions.cend())
        openSession(*session);
}

void MainWindow::editSelectedSession()
{
    auto *item = m_sessionTree->currentItem();
    if (!item || isFolder(item)) {
        QMessageBox::information(this, QStringLiteral("Edit Session"),
                                 QStringLiteral("Select a saved session first."));
        return;
    }

    const auto sessionId = item->data(0, SessionIdRole).toString();
    const auto sessionIterator = m_sessions.find(sessionId);
    if (sessionIterator == m_sessions.end())
        return;

    const SessionDefinition original = *sessionIterator;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit Saved Session"));
    // Wide enough for the terminal preview and the settings tabs.
    dialog.setMinimumWidth(560);
    auto *form = new QFormLayout(&dialog);

    auto *nameEdit = new QLineEdit(original.name, &dialog);
    auto *protocolCombo = new QComboBox(&dialog);
    protocolCombo->addItems({QStringLiteral("SSH"), QStringLiteral("RDP"), QStringLiteral("VNC")});
    protocolCombo->setCurrentText(original.protocol);
    auto *hostEdit = new QLineEdit(original.host, &dialog);
    auto *portEdit = new QLineEdit(original.port == 0 ? QString() : QString::number(original.port),
                                   &dialog);
    auto *usernameEdit = new QLineEdit(original.username, &dialog);
    auto *passwordEdit = new QLineEdit(&dialog);
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText(QStringLiteral("Leave blank to keep the stored password"));

    form->addRow(QStringLiteral("Name:"), nameEdit);
    form->addRow(QStringLiteral("Protocol:"), protocolCombo);
    form->addRow(QStringLiteral("Host:"), hostEdit);
    form->addRow(QStringLiteral("Port:"), portEdit);
    form->addRow(QStringLiteral("Username:"), usernameEdit);
    form->addRow(QStringLiteral("New password (optional):"), passwordEdit);
    SshOptionsEditor *sshEditor = nullptr;
    auto *appearanceEditor = addTerminalAppearanceGroup(form, protocolCombo, original.terminal,
                                                        original.ssh, &sshEditor);
    auto *rdpEditor = addRdpOptionsGroup(form, protocolCombo, original.rdp);
    auto *vncEditor = addVncOptionsGroup(form, protocolCombo, original.vnc);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    bool portOk = true;
    const auto portText = portEdit->text().trimmed();
    const auto portValue = portText.isEmpty() ? 0 : portText.toUShort(&portOk);
    if (nameEdit->text().trimmed().isEmpty() || hostEdit->text().trimmed().isEmpty() || !portOk) {
        QMessageBox::warning(this, QStringLiteral("Invalid session"),
                             QStringLiteral("Enter a session name, host, and a valid port number."));
        return;
    }

    auto updated = original;
    updated.name = nameEdit->text().trimmed();
    updated.protocol = protocolCombo->currentText();
    updated.host = hostEdit->text().trimmed();
    updated.port = portValue;
    updated.username = usernameEdit->text().trimmed();
    updated.terminal = updated.protocol == QStringLiteral("SSH") ? appearanceEditor->value()
                                                                : TerminalAppearance();
    updated.ssh = updated.protocol == QStringLiteral("SSH") ? sshEditor->value() : SshOptions();
    updated.rdp = updated.protocol == QStringLiteral("RDP") ? rdpEditor->value() : RdpOptions();
    updated.vnc = updated.protocol == QStringLiteral("VNC") ? vncEditor->value() : VncOptions();

    auto newPassword = passwordEdit->text();
    if (!newPassword.isEmpty()) {
        if (!ensureVaultUnlocked())
            return;
        QString vaultError;
        if (!m_vault.setPassword(updated.id, newPassword, &vaultError)) {
            QMessageBox::warning(this, QStringLiteral("Could not save password"), vaultError);
            return;
        }
        newPassword.fill(QChar(0));
        passwordEdit->clear();
    }

    *sessionIterator = updated;
    item->setText(0, sessionLabel(updated));
    item->setToolTip(0, QStringLiteral("%1 — %2:%3")
                            .arg(updated.protocol, updated.host,
                                 updated.port == 0 ? QStringLiteral("default")
                                                   : QString::number(updated.port)));
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *page = m_sessionTabs->widget(index);
        if (page && page->property("sessionId").toString() == updated.id) {
            m_sessionTabs->setTabText(index, updated.name);
            m_sessionTabs->setTabToolTip(index, QStringLiteral("%1 — %2")
                                                    .arg(updated.protocol, updated.host));
        }
    }
    applyTerminalAppearanceToTabs();
    saveSessions();
    statusBar()->showMessage(QStringLiteral("Saved session: %1").arg(updated.name), 3000);
}

// Renames a session or folder (F2 in the session list).
void MainWindow::renameTreeItem(QTreeWidgetItem *item, const QString &newName)
{
    const QString name = newName.trimmed();
    if (name.isEmpty())
        return;
    if (isFolder(item)) {
        if (item->text(0) == name)
            return;
        item->setText(0, name);
        saveSessions();
        return;
    }
    const auto id = item->data(0, SessionIdRole).toString();
    auto session = m_sessions.find(id);
    if (session == m_sessions.end())
        return;
    if (session->name == name) {
        item->setText(0, sessionLabel(*session));
        return;
    }
    session->name = name;
    item->setText(0, sessionLabel(*session));
    // Open tabs titled by session name follow.
    if (m_startup.tabTitle != QStringLiteral("userhost")) {
        for (int index = 0; index < m_sessionTabs->count(); ++index) {
            if (m_sessionTabs->widget(index)->property("sessionId").toString() == id)
                m_sessionTabs->setTabText(index, name);
        }
    }
    saveSessions();
    statusBar()->showMessage(QStringLiteral("Renamed session to %1").arg(name), 3000);
}

// A copy of the session right below it, named "Name (1)", "Name (2)", ...
void MainWindow::duplicateSession(QTreeWidgetItem *item)
{
    if (!item || isFolder(item))
        return;
    const auto original = m_sessions.constFind(item->data(0, SessionIdRole).toString());
    if (original == m_sessions.cend())
        return;
    QString base = original->name;
    static const QRegularExpression numbered(QStringLiteral("^(.*) \\((\\d+)\\)$"));
    if (const auto match = numbered.match(base); match.hasMatch())
        base = match.captured(1);
    QSet<QString> names;
    for (const auto &session : std::as_const(m_sessions))
        names.insert(session.name);
    int number = 1;
    while (names.contains(QStringLiteral("%1 (%2)").arg(base).arg(number)))
        ++number;

    SessionDefinition copy = *original;
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy.name = QStringLiteral("%1 (%2)").arg(base).arg(number);
    // The saved password comes along when the vault is open.
    bool passwordCopied = false;
    if (m_vault.isUnlocked()) {
        QString password = m_vault.passwordForSession(original->id);
        if (!password.isEmpty())
            passwordCopied = m_vault.setPassword(copy.id, password);
        password.fill(QChar(0));
    }
    auto *parent = item->parent();
    addSession(copy, parent);
    // addSession() appends; place the copy after the original and the
    // copies already below it, so they stay in order.
    auto *added = m_sessionTree->currentItem();
    const auto childAt = [this, parent](int index) {
        return parent ? parent->child(index) : m_sessionTree->topLevelItem(index);
    };
    const auto indexOf = [this, parent](QTreeWidgetItem *child) {
        return parent ? parent->indexOfChild(child) : m_sessionTree->indexOfTopLevelItem(child);
    };
    if (parent)
        parent->removeChild(added);
    else
        m_sessionTree->takeTopLevelItem(indexOf(added));
    int position = indexOf(item) + 1;
    const QString prefix = base + QStringLiteral(" (");
    while (auto *next = childAt(position)) {
        if (isFolder(next)
            || !m_sessions.value(next->data(0, SessionIdRole).toString()).name.startsWith(prefix))
            break;
        ++position;
    }
    if (parent)
        parent->insertChild(position, added);
    else
        m_sessionTree->insertTopLevelItem(position, added);
    m_sessionTree->setCurrentItem(added);
    saveSessions();
    statusBar()->showMessage(
        m_vault.exists() && !m_vault.isUnlocked()
            ? QStringLiteral("Duplicated as %1 (the vault is locked, so its password was not copied)")
                  .arg(copy.name)
            : QStringLiteral("Duplicated as %1%2")
                  .arg(copy.name, passwordCopied ? QStringLiteral(", with its password") : QString()),
        5000);
}

void MainWindow::showTreeContextMenu(const QPoint &position)
{
    auto *clickedItem = m_sessionTree->itemAt(position);
    if (clickedItem)
        m_sessionTree->setCurrentItem(clickedItem);

    QMenu menu(this);
    menu.addAction(QStringLiteral("New Folder…"), this, &MainWindow::createFolder);
    menu.addAction(QStringLiteral("New…"), this, &MainWindow::createSession);
    const auto addRename = [this, &menu, clickedItem] {
        auto *rename = menu.addAction(QStringLiteral("Rename"), this, [this, clickedItem] {
            m_sessionTree->editItem(clickedItem, 0);
        });
        rename->setShortcut(QKeySequence(Qt::Key_F2));
    };
    if (clickedItem && isFolder(clickedItem)) {
        menu.addSeparator();
        addRename();
        menu.addAction(QStringLiteral("Delete Folder…"), this, &MainWindow::deleteSelectedFolder);
    } else if (clickedItem) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("Open"), this, &MainWindow::openSelectedSession);
        menu.addAction(QStringLiteral("Edit…"), this, &MainWindow::editSelectedSession);
        addRename();
        menu.addAction(QStringLiteral("Duplicate"), this, [this, clickedItem] {
            duplicateSession(clickedItem);
        });
    }
    menu.exec(m_sessionTree->viewport()->mapToGlobal(position));
}

void MainWindow::closeSessionTab(int index)
{
    auto *page = m_sessionTabs->widget(index);
    if (!page)
        return;
    // Remote sessions ask first; local shells close directly.
    auto *terminal = qobject_cast<TerminalPage *>(page);
    auto *screen = qobject_cast<RemoteScreenPage *>(page);
    const QString host = terminal ? terminal->sshSettings().host : screen ? screen->host() : QString();
    if (m_startup.confirmCloseConnected && tabIsConnected(page) && !host.isEmpty()
        && QMessageBox::question(this, QStringLiteral("Close Tab"),
                                 QStringLiteral("Disconnect from %1 and close the tab?").arg(host))
               != QMessageBox::Yes)
        return;
    m_sessionTabs->removeTab(index);
    page->deleteLater();
}

// Whether closing this tab would end a live session: a local shell, or a
// connected SSH or remote desktop session.
bool MainWindow::tabIsConnected(QWidget *page) const
{
    if (auto *terminal = qobject_cast<TerminalPage *>(page))
        return terminal->isConnected();
    if (auto *screen = qobject_cast<RemoteScreenPage *>(page))
        return screen->isConnected();
    return false;
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

QString MainWindow::folderStateKey(const QTreeWidgetItem *folder) const
{
    QStringList segments;
    for (auto *item = folder; item; item = item->parent()) {
        const auto *parent = item->parent();
        const int index = parent ? parent->indexOfChild(const_cast<QTreeWidgetItem *>(item))
                                 : m_sessionTree->indexOfTopLevelItem(
                                       const_cast<QTreeWidgetItem *>(item));
        segments.prepend(QString::number(index));
    }
    return segments.join(QLatin1Char('/'));
}

void MainWindow::restoreFolderExpansion()
{
    const auto states = loadFolderExpansionStates();
    if (states.isEmpty())
        return;

    std::function<void(QTreeWidgetItem *)> restore = [this, &states, &restore](QTreeWidgetItem *item) {
        if (isFolder(item))
            item->setExpanded(states.value(folderStateKey(item), true));
        for (int index = 0; index < item->childCount(); ++index)
            restore(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        restore(m_sessionTree->topLevelItem(index));
}

void MainWindow::saveFolderExpansion() const
{
    QHash<QString, bool> states;
    std::function<void(const QTreeWidgetItem *)> collect = [this, &states, &collect](
                                                             const QTreeWidgetItem *item) {
        if (isFolder(item))
            states.insert(folderStateKey(item), item->isExpanded());
        for (int index = 0; index < item->childCount(); ++index)
            collect(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        collect(m_sessionTree->topLevelItem(index));
    saveFolderExpansionStates(states);
}

void MainWindow::addFolder(const QString &name, QTreeWidgetItem *parent)
{
    auto *folder = parent ? new QTreeWidgetItem(parent, {name})
                          : new QTreeWidgetItem(m_sessionTree, {name});
    setItemKind(folder, ItemKind::Folder);
    folder->setExpanded(true);
    m_sessionTree->setCurrentItem(folder);
}

void MainWindow::addSession(const SessionDefinition &session, QTreeWidgetItem *parent)
{
    auto *item = parent ? new QTreeWidgetItem(parent, {sessionLabel(session)})
                        : new QTreeWidgetItem(m_sessionTree, {sessionLabel(session)});
    setItemKind(item, ItemKind::Session);
    item->setData(0, SessionIdRole, session.id);
    item->setToolTip(0, QStringLiteral("%1 — %2:%3")
                            .arg(session.protocol, session.host,
                                 session.port == 0 ? QStringLiteral("default")
                                                   : QString::number(session.port)));
    m_sessions.insert(session.id, session);
    if (parent)
        parent->setExpanded(true);
    m_sessionTree->setCurrentItem(item);
}

void MainWindow::openSession(const SessionDefinition &session)
{
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *page = m_sessionTabs->widget(index);
        if (page && page->property("sessionId").toString() == session.id) {
            m_sessionTabs->setCurrentIndex(index);
            return;
        }
    }

    QWidget *page = nullptr;
    if (session.protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) == 0) {
        auto settings = sshSettingsFor(session.host, session.port, session.username, session.ssh);
        // A locked vault must be unlocked before a session opens; a wrong
        // master password or cancelling the prompt does not open the session.
        if (m_vault.exists() && !m_vault.isUnlocked() && !ensureVaultUnlocked()) {
            statusBar()->showMessage(
                QStringLiteral("Session not opened: the credential vault is locked"), 5000);
            return;
        }
        settings.password = m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0));
        auto *terminal = new TerminalPage(
            settings, effectiveTerminalAppearance().overriddenBy(session.terminal), m_sessionTabs);
        settings.password.fill(QChar(0));
        connectTerminalPage(terminal);
        applyTabSettings(terminal, session.name);
        page = terminal;
    } else if (session.protocol.compare(QStringLiteral("RDP"), Qt::CaseInsensitive) == 0) {
        RdpConnectionSettings settings;
        settings.host = session.host;
        settings.port = session.port == 0 ? 3389 : session.port;
        // "DOMAIN\user" or "user@domain" both work; FreeRDP needs them apart.
        settings.username = session.username;
        if (const int slash = session.username.indexOf(QLatin1Char('\\')); slash > 0) {
            settings.domain = session.username.left(slash);
            settings.username = session.username.mid(slash + 1);
        }
        settings.configPath = stateFilePath(QStringLiteral("rdp"));
        settings.options = loadRdpDefaults().overriddenBy(session.rdp);
        if (m_vault.exists() && !m_vault.isUnlocked() && !ensureVaultUnlocked()) {
            statusBar()->showMessage(
                QStringLiteral("Session not opened: the credential vault is locked"), 5000);
            return;
        }
        settings.password = m_vault.passwordForSession(session.id.section(QLatin1Char('#'), 0, 0));
        auto *rdp = new RdpPage(settings, m_sessionTabs);
        settings.password.fill(QChar(0));
        connect(rdp, &RdpPage::statusMessage, this, [this](const QString &message) {
            statusBar()->showMessage(message, 5000);
        });
        page = rdp;
    } else if (session.protocol.compare(QStringLiteral("VNC"), Qt::CaseInsensitive) == 0) {
        if (m_vault.exists() && !m_vault.isUnlocked() && !ensureVaultUnlocked()) {
            statusBar()->showMessage(
                QStringLiteral("Session not opened: the credential vault is locked"), 5000);
            return;
        }
        VncPageSettings settings;
        auto &connection = settings.connection;
        connection.host = session.host;
        connection.port = session.port == 0 ? 5900 : session.port;
        // "host:1" is display 1, port 5901 (a bare IPv6 address has more colons).
        if (session.port == 0 && session.host.count(QLatin1Char(':')) == 1) {
            bool ok = false;
            const int display = session.host.section(QLatin1Char(':'), 1).toInt(&ok);
            if (ok && display >= 0 && display < 100) {
                connection.host = session.host.section(QLatin1Char(':'), 0, 0);
                connection.port = quint16(5900 + display);
            }
        }
        connection.username = session.username;
        connection.options = loadVncDefaults().overriddenBy(session.vnc);
        const QString baseId = session.id.section(QLatin1Char('#'), 0, 0);
        connection.password = m_vault.passwordForSession(baseId);
        settings.canSavePassword = m_vault.exists();
        const QString tunnel = connection.options.sshTunnel;
        if (!tunnel.isEmpty() && tunnel != QStringLiteral("none")) {
            if (tunnel.startsWith(QStringLiteral("session:"))) {
                const auto ssh = m_sessions.constFind(tunnel.mid(8));
                if (ssh == m_sessions.cend()) {
                    QMessageBox::warning(this, QStringLiteral("SSH Tunnel"),
                                         QStringLiteral("The saved SSH session for this tunnel no "
                                                        "longer exists. Choose another in the "
                                                        "session's settings."));
                    return;
                }
                settings.tunnel = sshSettingsFor(ssh->host, ssh->port, ssh->username, ssh->ssh);
                settings.tunnel->password = m_vault.passwordForSession(ssh->id);
            } else {
                // user@host[:port]
                QString target = tunnel;
                const QString user = target.contains(QLatin1Char('@')) ? target.section(QLatin1Char('@'), 0, 0)
                                                                        : session.username;
                target = target.section(QLatin1Char('@'), -1);
                quint16 port = 0;
                if (target.count(QLatin1Char(':')) == 1) {
                    port = target.section(QLatin1Char(':'), 1).toUShort();
                    target = target.section(QLatin1Char(':'), 0, 0);
                }
                settings.tunnel = sshSettingsFor(target, port, user);
            }
        }
        auto *vnc = new VncPage(settings, m_sessionTabs);
        connection.password.fill(QChar(0));
        if (settings.tunnel)
            settings.tunnel->password.fill(QChar(0));
        connect(vnc, &VncPage::statusMessage, this, [this](const QString &message) {
            statusBar()->showMessage(message, 5000);
        });
        connect(vnc, &VncPage::passwordSaveRequested, this, [this, baseId](const QString &password) {
            if (!ensureVaultUnlocked())
                return;
            QString error;
            if (!m_vault.setPassword(baseId, password, &error))
                QMessageBox::warning(this, QStringLiteral("Could not save password"), error);
        });
        page = vnc;
    } else {
        page = new SessionPage(session, m_sessionTabs);
    }
    page->setProperty("sessionId", session.id);
    QString title = session.name;
    if (m_startup.tabTitle == QStringLiteral("userhost") && !session.host.isEmpty())
        title = session.username.isEmpty() ? session.host
                                           : QStringLiteral("%1@%2").arg(session.username, session.host);
    addSessionTab(page, title, QStringLiteral("%1 — %2").arg(session.protocol, session.host));
    statusBar()->showMessage(QStringLiteral("Opened session tab: %1").arg(session.name), 3000);
}

// The SSH settings of the session window: appearance and connection tabs.
TerminalAppearanceEditor *MainWindow::addTerminalAppearanceGroup(
    QFormLayout *form, QComboBox *protocolCombo, const TerminalAppearance &value,
    const SshOptions &ssh, SshOptionsEditor **sshEditor)
{
    auto *tabs = new QTabWidget(form->parentWidget());
    auto *editor = new TerminalAppearanceEditor(value, true, m_terminalAppearance, tabs);
    tabs->addTab(editor, QStringLiteral("Appearance"));
    *sshEditor = new SshOptionsEditor(ssh, loadSshPreferences(), tabs);
    tabs->addTab(*sshEditor, QStringLiteral("Connection"));
    form->addRow(tabs);
    // Only the selected protocol's settings are shown; the dialog shrinks to fit.
    auto updateVisible = [protocolCombo, tabs] {
        tabs->setVisible(protocolCombo->currentText() == QStringLiteral("SSH"));
        if (auto *dialog = tabs->window())
            QTimer::singleShot(0, dialog, [dialog] { dialog->adjustSize(); });
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, tabs, updateVisible);
    updateVisible();
    return editor;
}

// The SSH connection settings for a host, with the SSH preferences (no
// password; the caller adds it from the vault).
SshConnectionSettings MainWindow::sshSettingsFor(const QString &host, quint16 port,
                                                 const QString &username,
                                                 const SshOptions &options) const
{
    SshConnectionSettings settings;
    settings.host = host;
    settings.port = port == 0 ? 22 : port;
    settings.username = username;
    settings.knownHostsPath = stateFilePath(QStringLiteral("known_hosts"));
    const auto ssh = options.appliedTo(loadSshPreferences());
    settings.connectTimeout = ssh.connectTimeout;
    settings.keepaliveSeconds = ssh.keepaliveSeconds;
    settings.compression = ssh.compression;
    settings.tryPublicKeys = ssh.tryPublicKeys;
    settings.identityFile = ssh.identityFile;
    settings.terminalType = ssh.terminalType;
    settings.autoReconnect = ssh.autoReconnect;
    settings.reconnectDelay = ssh.reconnectDelay;
    settings.reconnectAttempts = ssh.reconnectAttempts;
    return settings;
}

VncOptionsEditor *MainWindow::addVncOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                                 const VncOptions &value)
{
    // The editor's own tabs; no frame around them.
    auto *group = new QWidget(form->parentWidget());
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *editor = new VncOptionsEditor(value, loadVncDefaults(), sshTunnelChoices(), group);
    layout->addWidget(editor);
    form->addRow(group);
    auto updateVisible = [protocolCombo, group] {
        group->setVisible(protocolCombo->currentText() == QStringLiteral("VNC"));
        if (auto *dialog = group->window())
            QTimer::singleShot(0, dialog, [dialog] { dialog->adjustSize(); });
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, group, updateVisible);
    updateVisible();
    return editor;
}

// Saved SSH sessions a VNC connection can tunnel through: (id, label).
QList<QPair<QString, QString>> MainWindow::sshTunnelChoices() const
{
    QList<QPair<QString, QString>> choices;
    for (const auto &session : m_sessions) {
        if (session.protocol.compare(QStringLiteral("SSH"), Qt::CaseInsensitive) != 0)
            continue;
        const QString target = session.username.isEmpty()
                                   ? session.host
                                   : QStringLiteral("%1@%2").arg(session.username, session.host);
        choices.append({session.id, QStringLiteral("%1 (%2)").arg(session.name, target)});
    }
    std::sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) {
        return a.second.localeAwareCompare(b.second) < 0;
    });
    return choices;
}

RdpOptionsEditor *MainWindow::addRdpOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                                 const RdpOptions &value)
{
    // The editor's own tabs; no frame around them.
    auto *group = new QWidget(form->parentWidget());
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *editor = new RdpOptionsEditor(value, true, loadRdpDefaults(), group);
    layout->addWidget(editor);
    form->addRow(group);
    auto updateVisible = [protocolCombo, group] {
        group->setVisible(protocolCombo->currentText() == QStringLiteral("RDP"));
        if (auto *dialog = group->window())
            QTimer::singleShot(0, dialog, [dialog] { dialog->adjustSize(); });
    };
    connect(protocolCombo, &QComboBox::currentTextChanged, group, updateVisible);
    updateVisible();
    return editor;
}

// ---- Preferences API (used by the View menu and the Preferences window) ----

QStringList MainWindow::availableUiStyles() const
{
    return QStyleFactory::keys();
}

void MainWindow::setUiStyle(const QString &styleKey)
{
    m_selectedStyleKey = styleKey;
    applyUiStyle(styleKey);
    saveUiStylePreference(styleKey);
    syncMenuChecks();
}

QList<QPair<QString, QString>> MainWindow::colorThemeChoices() const
{
    QList<QPair<QString, QString>> choices{{QStringLiteral("auto"), QStringLiteral("Automatic (follow OS)")},
                                           {QStringLiteral("system"), QStringLiteral("System")},
                                           {QStringLiteral("light"), QStringLiteral("Light")},
                                           {QStringLiteral("dark"), QStringLiteral("Dark")}};
    for (const auto &preset : PalettePresets)
        choices.append({QString::fromLatin1(preset.key), QString::fromLatin1(preset.label)});
    choices.append({QStringLiteral("custom"), QStringLiteral("Custom")});
    return choices;
}

void MainWindow::setColorTheme(const QString &themeKey)
{
    m_colorThemeKey = themeKey;
    applyColorTheme(themeKey);
    saveColorThemePreference(themeKey, m_customColors);
    applyTerminalAppearanceToTabs();
    syncMenuChecks();
}

bool MainWindow::isDarkInterface() const
{
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

void MainWindow::setIconThemeChoice(const QString &themeName)
{
    m_iconTheme = themeName;
    setIconTheme(themeName);
    saveIconThemePreference(themeName);
    refreshIcons();
    syncMenuChecks();
}

QString MainWindow::interfaceFontFamily() const
{
    return loadInterfaceFont().family;
}

int MainWindow::interfaceFontSize() const
{
    return loadInterfaceFont().pointSize;
}

void MainWindow::setInterfaceFont(const QString &family, int pointSize)
{
    const InterfaceFont font{family, pointSize};
    saveInterfaceFont(font);
    applyInterfaceFont(m_systemFont, font);
}

void MainWindow::setDefaultTerminalAppearance(const TerminalAppearance &appearance)
{
    m_terminalAppearance = appearance;
    saveTerminalAppearance(appearance);
    applyTerminalAppearanceToTabs();
}

void MainWindow::setDarkTerminalColorScheme(const QString &scheme)
{
    m_darkTerminalColorScheme = scheme;
    const auto path = settingsFilePath();
    YAML::Node settings;
    try {
        settings = YAML::LoadFile(path.toStdString());
    } catch (const YAML::Exception &) {
    }
    settings["terminal"]["dark_color_scheme"] = scheme.toStdString();
    YAML::Emitter emitter;
    emitter << settings;
    QSaveFile file(path);
    if (QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly)) {
        QByteArray contents(emitter.c_str());
        contents.append('\n');
        if (file.write(contents) == contents.size())
            file.commit();
        else
            file.cancelWriting();
    }
    applyTerminalAppearanceToTabs();
}

// The default appearance, with the dark-mode colour scheme while the
// interface is dark.
TerminalAppearance MainWindow::effectiveTerminalAppearance() const
{
    auto appearance = m_terminalAppearance;
    if (!m_darkTerminalColorScheme.isEmpty() && isDarkInterface())
        appearance.colorScheme = m_darkTerminalColorScheme;
    return appearance;
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

void MainWindow::applyTerminalAppearanceToTabs()
{
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        auto *page = qobject_cast<TerminalPage *>(m_sessionTabs->widget(index));
        if (!page)
            continue;
        const auto session = m_sessions.constFind(
            page->property("sessionId").toString().section(QLatin1Char('#'), 0, 0));
        page->setAppearance(session == m_sessions.cend()
                                ? effectiveTerminalAppearance()
                                : effectiveTerminalAppearance().overriddenBy(session->terminal));
    }
}

void MainWindow::setThemedIcon(QAction *action, const QString &name,
                               QStyle::StandardPixmap fallback)
{
    m_themedActions.append({action, name, fallback});
    action->setIcon(themedIcon(name, fallback));
}

// Re-applies every icon after the icon theme changed.
void MainWindow::refreshIcons()
{
    for (const auto &themed : std::as_const(m_themedActions)) {
        if (themed.action)
            themed.action->setIcon(themedIcon(themed.name, themed.fallback));
    }
    std::function<void(QTreeWidgetItem *)> refreshItem = [this, &refreshItem](QTreeWidgetItem *item) {
        setItemKind(item, isFolder(item) ? ItemKind::Folder : ItemKind::Session);
        for (int index = 0; index < item->childCount(); ++index)
            refreshItem(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        refreshItem(m_sessionTree->topLevelItem(index));
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        if (auto *page = qobject_cast<TerminalPage *>(m_sessionTabs->widget(index));
            page && page->sftpPanel())
            page->sftpPanel()->refreshIcons();
    }
}

void MainWindow::setItemKind(QTreeWidgetItem *item, ItemKind kind)
{
    item->setData(0, ItemKindRole, kind == ItemKind::Folder ? QStringLiteral("folder")
                                                           : QStringLiteral("session"));
    // Both can be renamed with F2.
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    if (kind == ItemKind::Folder) {
        item->setIcon(0, themedIcon(QStringLiteral("folder"), QStyle::SP_DirIcon));
    } else {
        item->setIcon(0, themedIcon(QStringLiteral("network-server"), QStyle::SP_ComputerIcon));
        // Sessions can be dragged, but nothing can be dropped onto them.
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
    }
}

void MainWindow::showSftpPanelForCurrentTab()
{
    auto *page = qobject_cast<TerminalPage *>(m_sessionTabs->currentWidget());
    auto *panel = page ? page->sftpPanel() : nullptr;
    if (!panel) {
        m_sftpPlaceholder->setText(
            m_sessionTabs->count() == 0
                ? QStringLiteral("Open an SSH session to browse its files here.")
                : QStringLiteral("The current tab has no SFTP connection."));
        m_sftpStack->setCurrentWidget(m_sftpPlaceholder);
        return;
    }
    if (m_sftpStack->indexOf(panel) < 0)
        m_sftpStack->addWidget(panel);
    m_sftpStack->setCurrentWidget(panel);
}

bool MainWindow::isFolder(const QTreeWidgetItem *item) const
{
    return item && item->data(0, ItemKindRole).toString() == QStringLiteral("folder");
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
    for (auto it = m_shortcutActions.cbegin(); it != m_shortcutActions.cend(); ++it)
        it.value()->setShortcut(shortcuts.value(it.key()));
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        if (auto *page = qobject_cast<TerminalPage *>(m_sessionTabs->widget(index)))
            page->setShortcuts(shortcuts);
    }
}

void MainWindow::setTerminalBehaviour(const TerminalBehaviour &behaviour)
{
    m_behaviour = behaviour;
    saveTerminalBehaviour(behaviour);
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        if (auto *page = qobject_cast<TerminalPage *>(m_sessionTabs->widget(index)))
            page->setBehaviour(behaviour);
    }
}

void MainWindow::setStartupPreferences(const StartupPreferences &preferences)
{
    const bool labelsChanged = preferences.showHostInList != m_startup.showHostInList;
    m_startup = preferences;
    saveStartupPreferences(preferences);
    if (labelsChanged)
        refreshSessionLabels();
}

QString MainWindow::sessionLabel(const SessionDefinition &session) const
{
    if (!m_startup.showHostInList || session.host.isEmpty())
        return session.name;
    return QStringLiteral("%1 (%2)").arg(session.name, session.host);
}

void MainWindow::refreshSessionLabels()
{
    std::function<void(QTreeWidgetItem *)> refresh = [this, &refresh](QTreeWidgetItem *item) {
        if (!isFolder(item)) {
            const auto session = m_sessions.constFind(item->data(0, SessionIdRole).toString());
            if (session != m_sessions.cend())
                item->setText(0, sessionLabel(*session));
        }
        for (int index = 0; index < item->childCount(); ++index)
            refresh(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        refresh(m_sessionTree->topLevelItem(index));
}

void MainWindow::connectTerminalPage(TerminalPage *page)
{
    connect(page, &TerminalPage::statusMessage, this, [this](const QString &message) {
        statusBar()->showMessage(message, 5000);
    });
    // Background tabs with new output or a bell get the accent colour.
    auto mark = [this, page] {
        const int index = m_sessionTabs->indexOf(page);
        if (index >= 0 && index != m_sessionTabs->currentIndex())
            m_sessionTabs->tabBar()->setTabTextColor(index, palette().color(QPalette::Highlight));
    };
    connect(page, &TerminalPage::activity, this, mark);
    connect(page, &TerminalPage::bell, this, mark);
    connect(page, &TerminalPage::userInput, this, [this, page](const QByteArray &data) {
        if (!m_broadcast || m_broadcasting)
            return;
        m_broadcasting = true;
        for (int index = 0; index < m_sessionTabs->count(); ++index) {
            auto *other = qobject_cast<TerminalPage *>(m_sessionTabs->widget(index));
            if (other && other != page)
                other->sendInput(data);
        }
        m_broadcasting = false;
    });
}

void MainWindow::addSessionTab(QWidget *page, const QString &title, const QString &toolTip)
{
    const int index = m_startup.newTabNextToCurrent && m_sessionTabs->currentIndex() >= 0
                          ? m_sessionTabs->insertTab(m_sessionTabs->currentIndex() + 1, page, title)
                          : m_sessionTabs->addTab(page, title);
    m_sessionTabs->setTabToolTip(index, toolTip);
    m_sessionTabs->setCurrentIndex(index);
    page->setFocus();
    // "Close Tab" on a tab whose session ended.
    const auto close = [this, page] {
        if (const int at = m_sessionTabs->indexOf(page); at >= 0)
            closeSessionTab(at);
    };
    if (auto *terminal = qobject_cast<TerminalPage *>(page))
        connect(terminal, &TerminalPage::closeRequested, this, close);
    else if (auto *screen = qobject_cast<RemoteScreenPage *>(page))
        connect(screen, &RemoteScreenPage::closeRequested, this, close);
}

void MainWindow::duplicateTab(int index)
{
    auto *page = m_sessionTabs->widget(index);
    if (!page)
        return;
    const auto sessionId = page->property("sessionId").toString();
    if (sessionId.isEmpty()) {
        openLocalTerminal();
        return;
    }
    const auto session = m_sessions.constFind(sessionId);
    if (session == m_sessions.cend())
        return;
    // openSession() switches to an open tab of the same session, so open a
    // second copy under a distinct ID.
    auto copy = *session;
    copy.id = sessionId + QStringLiteral("#") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    openSession(copy);
}

// Send Keys and Save Screenshot for a remote desktop (RDP or VNC) tab.
void MainWindow::addRemoteScreenActions(QMenu *menu, RemoteScreenPage *screen)
{
    auto *keys = menu->addMenu(QStringLiteral("Send Keys"));
    keys->setEnabled(screen->isConnected());
    for (const auto &[combination, label] : RemoteScreenPage::keyCombinations()) {
        keys->addAction(label, screen, [screen, combination] {
            screen->sendKeyCombination(combination);
        });
    }
    menu->addAction(QStringLiteral("Save Screenshot…"), this, [this, screen] {
        const QImage image = screen->screenshot();
        if (image.isNull()) {
            statusBar()->showMessage(QStringLiteral("Nothing to save yet: not connected."), 5000);
            return;
        }
        const QString name = QStringLiteral("%1 %2.png")
                                 .arg(screen->host(), QDateTime::currentDateTime().toString(
                                                          QStringLiteral("yyyy-MM-dd HH.mm.ss")));
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Screenshot"),
            QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).filePath(name),
            QStringLiteral("PNG image (*.png);;JPEG image (*.jpg)"));
        if (path.isEmpty())
            return;
        if (image.save(path))
            statusBar()->showMessage(QStringLiteral("Screenshot saved to %1")
                                         .arg(QDir::toNativeSeparators(path)),
                                     5000);
        else
            QMessageBox::warning(this, QStringLiteral("Save Screenshot"),
                                 QStringLiteral("Could not save %1.").arg(QDir::toNativeSeparators(path)));
    });
}

void MainWindow::showTabContextMenu(const QPoint &position)
{
    const int index = m_sessionTabs->tabBar()->tabAt(position);
    if (index < 0)
        return;
    QMenu menu(this);
    menu.addAction(QStringLiteral("Duplicate Tab"), this, [this, index] { duplicateTab(index); });
    if (auto *screen = qobject_cast<RemoteScreenPage *>(m_sessionTabs->widget(index))) {
        menu.addSeparator();
        addRemoteScreenActions(&menu, screen);
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("Close Tab"), this, [this, index] { closeSessionTab(index); });
    menu.addAction(QStringLiteral("Close Other Tabs"), this, [this, index] {
        auto *keep = m_sessionTabs->widget(index);
        for (int other = m_sessionTabs->count() - 1; other >= 0; --other) {
            if (m_sessionTabs->widget(other) != keep)
                closeSessionTab(other);
        }
    });
    menu.exec(m_sessionTabs->tabBar()->mapToGlobal(position));
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
    if (watched == m_sessionTabs->tabBar() && event->type() == QEvent::MouseButtonRelease
        && m_startup.middleClickCloses) {
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

QStringList MainWindow::openSessionIds() const
{
    QStringList ids;
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        // Duplicated tabs are stored under their session's ID.
        const auto id = m_sessionTabs->widget(index)->property("sessionId").toString().section(QLatin1Char('#'), 0, 0);
        ids.append(id.isEmpty() ? QStringLiteral("local") : id);
    }
    return ids;
}

void MainWindow::restoreStartupTabs()
{
    if (m_security.unlockAtStartup && m_vault.exists() && !m_vault.isUnlocked())
        ensureVaultUnlocked();
    if (m_startup.onStartup == QStringLiteral("local")) {
        if (LocalTerminalSupported)
            openLocalTerminal();
    } else if (m_startup.onStartup == QStringLiteral("restore")) {
        for (const auto &id : loadOpenSessions()) {
            if (id == QStringLiteral("local")) {
                if (LocalTerminalSupported)
                    openLocalTerminal();
                continue;
            }
            const auto session = m_sessions.constFind(id);
            if (session != m_sessions.cend())
                openSession(*session);
        }
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange && isMinimized() && m_security.lockOnMinimize
        && m_vault.isUnlocked())
        lockVaultFor(QStringLiteral("when the window was minimized"));
    QMainWindow::changeEvent(event);
}

void MainWindow::lockVaultFor(const QString &reason)
{
    m_vault.lock();
    statusBar()->showMessage(reason.isEmpty()
                                 ? QStringLiteral("Credential vault locked")
                                 : QStringLiteral("Credential vault locked %1").arg(reason),
                             5000);
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
        if (auto *page = qobject_cast<TerminalPage *>(m_sessionTabs->widget(index)))
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
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        if (auto *page = qobject_cast<TerminalPage *>(m_sessionTabs->widget(index)))
            page->setHighlightRules(rules);
    }
}

void MainWindow::setSnippets(const QList<Snippet> &snippets)
{
    m_snippets = snippets;
    saveSnippets(snippets);
}

void MainWindow::setBroadcast(bool on)
{
    m_broadcast = on;
    m_broadcastIndicator->setVisible(on);
    if (m_broadcastAction->isChecked() != on)
        m_broadcastAction->setChecked(on);
    statusBar()->showMessage(on ? QStringLiteral("Typing now goes to every connected SSH tab")
                                : QStringLiteral("Broadcast off"),
                             3000);
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
    for (int index = 0; index < m_sessionTabs->count(); ++index) {
        if (auto *page = qobject_cast<TerminalPage *>(m_sessionTabs->widget(index)))
            page->setZoomSteps(on ? m_presentation.zoomSteps : 0);
    }
    if (m_presentationAction->isChecked() != on)
        m_presentationAction->setChecked(on);
}
