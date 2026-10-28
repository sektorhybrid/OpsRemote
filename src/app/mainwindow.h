// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "session.h"
#include "credentialvault.h"
#include "terminalappearance.h"
#include "preferences.h"
#include "sshsession.h"

#include <QMainWindow>
#include <QHash>
#include <QColor>
#include <QPalette>
#include <QPointer>
#include <QStyle>
#include <QFont>
#include <QElapsedTimer>

class QAction;
class QMenu;
class RemoteScreenPage;
class SshOptionsEditor;
class VncOptionsEditor;
struct VncOptions;
class PreferencesWindow;
class QTreeWidget;
class QTreeWidgetItem;
class QTabWidget;
class QStackedWidget;
class QLabel;
class QComboBox;
class QFormLayout;
class QActionGroup;
class QCloseEvent;
namespace YAML { class Node; }

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    // Saved SSH sessions a VNC connection can tunnel through: (id, label).
    QList<QPair<QString, QString>> sshTunnelChoices() const;
    explicit MainWindow(QWidget *parent = nullptr);

    // Preferences: current values, and setters that apply, save, and keep the
    // View menu in sync. Used by the View menu and the Preferences window.
    QString uiStyle() const { return m_selectedStyleKey; }
    QStringList availableUiStyles() const;
    void setUiStyle(const QString &styleKey);
    QString colorTheme() const { return m_colorThemeKey; }
    QList<QPair<QString, QString>> colorThemeChoices() const;
    void setColorTheme(const QString &themeKey);
    void editCustomColors();
    bool isDarkInterface() const;
    QString iconThemeChoice() const { return m_iconTheme; }
    void setIconThemeChoice(const QString &themeName);
    QFont systemFont() const { return m_systemFont; }
    QString interfaceFontFamily() const;
    int interfaceFontSize() const;
    void setInterfaceFont(const QString &family, int pointSize);
    TerminalAppearance defaultTerminalAppearance() const { return m_terminalAppearance; }
    void setDefaultTerminalAppearance(const TerminalAppearance &appearance);
    QString darkTerminalColorScheme() const { return m_darkTerminalColorScheme; }
    void setDarkTerminalColorScheme(const QString &scheme);
    TerminalBehaviour terminalBehaviour() const { return m_behaviour; }
    void setTerminalBehaviour(const TerminalBehaviour &behaviour);
    StartupPreferences startupPreferences() const { return m_startup; }
    void setStartupPreferences(const StartupPreferences &preferences);
    QHash<QString, QKeySequence> shortcuts() const { return m_shortcuts; }
    void setShortcuts(const QHash<QString, QKeySequence> &shortcuts);
    SshPreferences sshPreferences() const { return loadSshPreferences(); }
    void setSshPreferences(const SshPreferences &preferences) { saveSshPreferences(preferences); }
    SftpPreferences sftpPreferences() const { return loadSftpPreferences(); }
    void setSftpPreferences(const SftpPreferences &preferences) { saveSftpPreferences(preferences); }
    SecurityPreferences securityPreferences() const { return m_security; }
    void setSecurityPreferences(const SecurityPreferences &preferences);
    LoggingPreferences loggingPreferences() const { return m_logging; }
    void setLoggingPreferences(const LoggingPreferences &preferences);
    PresentationPreferences presentationPreferences() const { return m_presentation; }
    void setPresentationPreferences(const PresentationPreferences &preferences);
    QList<HighlightRule> highlightRules() const { return m_highlightRules; }
    void setHighlightRules(const QList<HighlightRule> &rules);
    QList<Snippet> snippets() const { return m_snippets; }
    void setSnippets(const QList<Snippet> &snippets);
    CredentialVault &vault() { return m_vault; }
    // Prompts for the master password as needed; false if the user gave up.
    bool unlockVault() { return ensureVaultUnlocked(); }
    void setPresentationMode(bool on);
    // Opens (or raises) the Preferences window at a page id, e.g. "about".
    void showPreferences(const QString &page = QString());

private slots:
    void createFolder();
    void deleteSelectedFolder();
    void createSession();
    void openLocalTerminal();
    void editSelectedSession();
    void openSelectedSession();
    void showTreeContextMenu(const QPoint &position);
    void closeSessionTab(int index);
    bool tabIsConnected(QWidget *page) const;
    void renameTreeItem(QTreeWidgetItem *item, const QString &newName);
    void duplicateSession(QTreeWidgetItem *item);
    void addRemoteScreenActions(QMenu *menu, RemoteScreenPage *screen);
    void importSessions();
    void exportSessions();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class ItemKind { Folder, Session };

    void createMenus();
    // Gives action the configurable shortcut id and keeps it up to date.
    void bindShortcut(QAction *action, const QString &id);
    void addSessionTab(QWidget *page, const QString &title, const QString &toolTip);
    void connectTerminalPage(class TerminalPage *page);
    void duplicateTab(int index);
    void showTabContextMenu(const QPoint &position);
    void restoreStartupTabs();
    void setBroadcast(bool on);
    void applyTabSettings(class TerminalPage *page, const QString &logName);
    void lockVaultFor(const QString &reason);
    QStringList openSessionIds() const;
    void createToolbar();
    void applyUiStyle(const QString &styleKey);
    void applyColorTheme(const QString &themeKey);
    void syncMenuChecks();
    TerminalAppearance effectiveTerminalAppearance() const;
    bool ensureVaultUnlocked();
    bool appendSessionYaml(const YAML::Node &roots, QTreeWidgetItem *parent, QString *error);
    bool appendCompactSessionYaml(const YAML::Node &folders, QTreeWidgetItem *parent,
                                  QString *error);
    void restoreFolderExpansion();
    void saveFolderExpansion() const;
    QString folderStateKey(const QTreeWidgetItem *folder) const;
    void loadSessions();
    void saveSessions();
    void addExampleSessions();
    QTreeWidgetItem *selectedFolder() const;
    int folderDepth(const QTreeWidgetItem *folder) const;
    void addFolder(const QString &name, QTreeWidgetItem *parent);
    void addSession(const SessionDefinition &session, QTreeWidgetItem *parent);
    QString sessionLabel(const SessionDefinition &session) const;
    void refreshSessionLabels();
    void openSession(const SessionDefinition &session);
    void setItemKind(QTreeWidgetItem *item, ItemKind kind);
    bool isFolder(const QTreeWidgetItem *item) const;
    void showSftpPanelForCurrentTab();
    void setThemedIcon(QAction *action, const QString &name, QStyle::StandardPixmap fallback);
    void refreshIcons();
    void applyTerminalAppearanceToTabs();
    TerminalAppearanceEditor *addTerminalAppearanceGroup(QFormLayout *form, QComboBox *protocolCombo,
                                                         const TerminalAppearance &value,
                                                         const SshOptions &ssh,
                                                         SshOptionsEditor **sshEditor);
    VncOptionsEditor *addVncOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                         const VncOptions &value);
    SshConnectionSettings sshSettingsFor(const QString &host, quint16 port,
                                         const QString &username,
                                         const SshOptions &options = {}) const;
    RdpOptionsEditor *addRdpOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                         const RdpOptions &value);

    QTabWidget *m_leftTabs = nullptr;
    QTreeWidget *m_sessionTree = nullptr;
    QStackedWidget *m_sftpStack = nullptr;
    QLabel *m_sftpPlaceholder = nullptr;
    QTabWidget *m_sessionTabs = nullptr;
    QHash<QString, SessionDefinition> m_sessions;
    QHash<QString, QColor> m_customColors;
    QPalette m_systemPalette;
    QFont m_systemFont;
    QString m_iconTheme;
    QString m_darkTerminalColorScheme;
    QActionGroup *m_styleGroup = nullptr;
    QActionGroup *m_colorThemeGroup = nullptr;
    QActionGroup *m_iconThemeGroup = nullptr;
    QPointer<PreferencesWindow> m_preferencesWindow;
    struct ThemedAction
    {
        QPointer<QAction> action;
        QString name;
        QStyle::StandardPixmap fallback;
    };
    QList<ThemedAction> m_themedActions;
    QString m_systemStyleKey;
    QString m_selectedStyleKey;
    QString m_colorThemeKey;
    TerminalAppearance m_terminalAppearance;
    CredentialVault m_vault;
    TerminalBehaviour m_behaviour;
    StartupPreferences m_startup;
    QHash<QString, QKeySequence> m_shortcuts;
    QHash<QString, QAction *> m_shortcutActions;
    SecurityPreferences m_security;
    LoggingPreferences m_logging;
    PresentationPreferences m_presentation;
    QList<HighlightRule> m_highlightRules;
    QList<Snippet> m_snippets;
    QElapsedTimer m_lastInput;
    bool m_broadcast = false;
    bool m_broadcasting = false; // re-entrancy guard while forwarding input
    QAction *m_broadcastAction = nullptr;
    QAction *m_presentationAction = nullptr;
    QLabel *m_broadcastIndicator = nullptr;
    struct PresentationState
    {
        bool active = false;
        Qt::WindowStates windowState;
        bool sidePanel = true;
        bool toolbar = true;
        bool statusBar = true;
    } m_presentationState;
};
