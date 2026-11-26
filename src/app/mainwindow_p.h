// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// MainWindow's own helpers, shared by its files (mainwindow.cpp for the
// window, menus and preferences; mainwindow_<area>.cpp for each area): the
// session list's roles and widgets, the look settings, the state files.
// Internal to MainWindow; nothing else includes this.

#pragma once

#include "appdirs.h"
#include "capabilities.h"
#include "panebars.h"
#include "roundedcorners.h"
#include "statusindicators.h"
#include "transfers.h"
#include "filedialogs.h"
#include "mainwindow.h"
#include "kubecredentials.h"
#include "sidepanels.h"
#include <QPlainTextEdit>
#include <QTemporaryFile>
#include "automation.h"
#include "scripts.h"
#include "sshauth.h"
#include "filespage.h"
#include "icons.h"
#include "jumphost.h"
#include "castrecorder.h"
#ifdef Q_OS_WIN
#include "conpty.h"
#endif
#include "localservers.h"
#include "availabilitynote.h"
#include "localresources.h"
#include "looks.h"
#include "quickconnect.h"
#include "serialoptions.h"
#include "sessionimport.h"
#include "systemcredentials.h"
#include "splittab.h"
#include "streamconnection.h"
#include "xdmcp.h"
#include "fieldcheck.h"
#include "formcard.h"
#include "inheritance.h"
#include "ftpoptions.h"
#include "localsessions.h"
#include "rdpoptions.h"
#include "rshoptions.h"
#include "settingsmigrations.h"
#include "vncoptions.h"
#include "opsremotestyle.h"
#include "keymanager.h"
#include "kvantumthemes.h"
#include "networktools.h"
#include "proxy.h"
#include "tunnels.h"
#include "wakeonlan.h"
#include "windowborder.h"
#include "windowfit.h"
#include "masterpassworddialog.h"
#include "preferenceswindow.h"
#include "rdppage.h"
#include "vncpage.h"
#include "remotescreenpage.h"
#include "sftppanel.h"
#include "tabactivity.h"
#include "terminalpage.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QCompleter>
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
#include <QHeaderView>
#include <QTableWidget>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QDateTime>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QScrollBar>
#include <QFont>
#include <QFontDatabase>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSpinBox>
#include <QCheckBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QGuiApplication>
#include <QTabBar>
#include <QTabWidget>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QMouseEvent>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <yaml-cpp/yaml.h>

namespace mainwindow_p {

constexpr int ItemKindRole = Qt::UserRole;

constexpr int SessionIdRole = Qt::UserRole + 1;

// Session list columns; the name is column 0.
constexpr int HostColumn = 1;

constexpr int ProtocolColumn = 2;

constexpr int SummaryColumn = 3; // folder summaries when no other column follows the name
// A folder's jump host for its sessions (see JumpHost); empty: its parent's.
constexpr int FolderJumpHostRole = Qt::UserRole + 2;

// A folder's proxy for its sessions (see ProxySettings); empty: its parent's.
constexpr int FolderProxyRole = Qt::UserRole + 3;

// A folder's Kerberos logons for its SSH sessions: 1 on, 0 off; none: its
// parent's (in the end the preferences).
constexpr int FolderKerberosRole = Qt::UserRole + 40;

// A folder's login script and triggers (automationToText); empty: its
// parent's.
constexpr int FolderAutomationRole = Qt::UserRole + 41;

constexpr int MaximumFolderDepth = 10;

// User state (sessions, preferences, vault, known hosts) lives in settings/
// next to the executable, so the application folder carries it.
QString stateDirectory();

QString stateFilePath(const QString &name);

// Protocols a saved session can use, as shown in the session window.
QStringList sessionProtocols();

// Protocols over SSH: a terminal (SSH), or only files (SFTP, SCP).
bool isSshProtocol(const QString &protocol);

// Protocols shown in a terminal tab.
bool isTerminalProtocol(const QString &protocol);

QStringList loadRecentSessions();

void saveRecentSessions(const QStringList &ids);

// The folder icon with a badge in its lower right corner: a star for Pinned,
// a clock for Recent. Drawn here, so every icon theme has them.
QIcon badgedFolderIcon(const QIcon &folder, bool star);

// Moves state written by earlier versions from data/ into settings/.
void migrateStateFromDataDirectory();

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

const PalettePreset *findPalettePreset(const QString &key);

QString settingsFilePath();

QString loadUiStylePreference();

QString loadColorThemePreference();

// Local shells run in a pseudo-terminal: a PTY on Linux, a pseudo console
// (ConPTY) on Windows 10 1809 and newer. SSH sessions do not need one.
constexpr bool LocalTerminalSupported = true;

void disableLocalTerminalWhereUnsupported(QAction *action);

// The program a local terminal runs, from the Preferences: (program,
// arguments, label). An empty program is the login shell (Linux).

QString loadIconThemePreference();

void saveIconThemePreference(const QString &themeName);

QHash<QString, QColor> loadCustomColors();

void saveColorThemePreference(const QString &themeKey,
                              const QHash<QString, QColor> &customColors);

void saveUiStylePreference(const QString &styleKey);

QHash<QString, bool> loadFolderExpansionStates();

void saveFolderExpansionStates(const QHash<QString, bool> &states);

YAML::Node sessionToYaml(const SessionDefinition &session);

TerminalAppearance terminalAppearanceFromYaml(const YAML::Node &node);

// Interface font for menus, panels, and dialogs (terminals have their own).
// An empty family or a size of 0 keeps the system's choice.
struct InterfaceFont
{
    QString family;
    int pointSize = 0;
};

InterfaceFont loadInterfaceFont();

void saveInterfaceFont(const InterfaceFont &font);

QString loadBaseLookId();

void applyInterfaceFont(const QFont &systemFont, const InterfaceFont &choice);

bool loadWindowBorderPreference();

void saveWindowBorderPreference(bool enabled);

int loadCornerRadiusPreference();

void saveCornerRadiusPreference(int radius);

bool loadSeeThroughListsPreference();

void saveSeeThroughListsPreference(bool seeThrough);

void saveDarkTerminalColorScheme(const QString &scheme);

QString loadDarkTerminalColorScheme();

TerminalAppearance loadTerminalAppearance();

void saveTerminalAppearance(const TerminalAppearance &appearance);

YAML::Node folderToYaml(const QTreeWidgetItem *folder,
                        const QHash<QString, SessionDefinition> &sessions);

} // namespace mainwindow_p
