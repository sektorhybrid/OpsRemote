// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "looks.h"
#include "hoststatus.h"
#include "session.h"
#include "sessionimport.h"
#include "credentialvault.h"
#include "terminalappearance.h"
#include "preferences.h"
#include "runcommand.h"
#include "sshsession.h"
#include "tunnels.h"

#include <QMainWindow>
#include <functional>
#include <QHash>
#include <QColor>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QStyle>
#include <QFont>
#include <QElapsedTimer>

class QAction;
class QMenu;
class RemoteScreenPage;
class JumpHostEditor;
class PortForwardListEditor;
class ProxyEditor;
class TunnelsWindow;
class NetworkToolsWindow;
class SerialOptionsEditor;
class SshOptionsEditor;
class VncOptionsEditor;
class FtpOptionsEditor;
struct FtpOptions;
struct VncOptions;
class PreferencesWindow;
class QTreeWidget;
class LocalServersWindow;
class QDialog;
class QDialogButtonBox;
class WakeOnLanEditor;
class QTimer;
class QToolButton;
class QTreeWidgetItem;
class QTabWidget;
class QStackedWidget;
class QLabel;
class QLineEdit;
class QComboBox;
class QFormLayout;
class QActionGroup;
class QCloseEvent;
namespace YAML { class Node; }

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    // Saved SSH sessions that can be a jump host: (id, label).
    QList<QPair<QString, QString>> jumpHostChoices() const;
    // A new SSH session made in the session window over `over` (for "New…"
    // in the lists of sessions); its id, or empty when cancelled.
    QString createSshSession(QWidget *over);
    // Run on Hosts: the connections of these sessions, asking for the vault
    // at most once; a session that cannot be used gets a problem instead.
    QList<RunCommandWindow::Resolved> resolveCommandHosts(const QStringList &ids);
    QString inheritedJumpHost(const QTreeWidgetItem *folder) const;
    QString effectiveJumpHost(const SessionDefinition &session) const;
    QString jumpHostLabel(const QString &jumpHost) const;
    void addProtocolTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                        QWidget *rdpEditor, QWidget *vncEditor, QWidget *page, const QString &title);
    ProxyEditor *addProxyTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                             QWidget *rdpEditor, QWidget *vncEditor, const QString &value,
                             const QTreeWidgetItem *folder);
    QString inheritedProxy(const QTreeWidgetItem *folder) const;
    ProxySettings effectiveProxy(const SessionDefinition &session) const;
    void applyProxy(const SessionDefinition &session, SshConnectionSettings *server,
                    std::vector<SshConnectionSettings> &jumps) const;
    void saveProxyPassword(const QString &value, QString password);
    JumpHostEditor *addJumpHostTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                                   QWidget *rdpEditor, QWidget *vncEditor, const QString &value,
                                   const QTreeWidgetItem *folder, const QString &sessionId);
    void editFolderSettings(QTreeWidgetItem *folder);
    PortForwardListEditor *addPortForwardingTab(TerminalAppearanceEditor *sshTabsMember,
                                                const QList<PortForward> &value);
    bool resolveTunnelServer(const QString &server, SshConnectionSettings *settings);
    bool resolveTunnel(const SavedTunnel &tunnel, SshConnectionSettings *settings);
    TunnelServerInfo describeTunnelServer(const QString &server, const QStringList &via) const;
    SshSession *openTabConnection(const QString &sessionId) const;
    void showTunnels();
    void showKeyManager();
    // Run on Hosts for a folder's SSH sessions (with its subfolders), or
    // for every saved SSH session when folder is null.
    void showRunCommand(QTreeWidgetItem *folder);
    void showNetworkTools();
    bool resolveJumpHosts(const SessionDefinition &session,
                          std::vector<SshConnectionSettings> *jumps);
    explicit MainWindow(QWidget *parent = nullptr);

    // Opens a quick-connect target ([protocol://][user@]host[:port]); false
    // with *error set if it cannot be read.
    bool openTarget(const QString &target, QString *error = nullptr);
    // Opens a saved session by name or by "Folder/Sub/Name"; false if none
    // or several match.
    bool openSessionByPath(const QString &path, QString *error = nullptr);
    // Handles the command line of this or a second start (see main.cpp).
    void handleCommandLine(const QStringList &arguments);
    void openInSplit(Qt::Orientation orientation, const QString &choice);
    void openLocalTerminal();
    void openLocalTerminalAsAdministrator();
    void updateMultiExecIndicator();
    QString copyPasswords(bool toSystem);
    QComboBox *passwordStoreChoice(const QString &value, QWidget *parent) const;
    void validateSessionDialog(QDialog *dialog, QFormLayout *form, QDialogButtonBox *buttons,
                               QComboBox *protocolCombo, QLineEdit *nameEdit, QLineEdit *hostEdit,
                               QLineEdit *portEdit, WakeOnLanEditor *wakeEditor);
    void importResult(const QString &source, const SessionImport::Result &result);

    // Preferences: current values, and setters that apply, save, and keep the
    // View menu in sync. Used by the View menu and the Preferences window.
    QString uiStyle() const { return m_selectedStyleKey; }
    QString uiStyleLabel(const QString &styleKey) const;
    // Looks: built-in and saved combinations of the appearance settings.
    QList<Look> looks() const;
    Look currentLook() const;
    QString matchingLookId() const;
    void applyLook(const Look &look);
    bool saveCurrentLookAs(const QString &name);
    void renameLook(const QString &id, const QString &name);
    void deleteLook(const QString &id);
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
    bool windowBorders() const;
    void setWindowBorders(bool enabled);
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
    LocalShellPreferences localShellPreferences() const { return m_localShell; }
    void setLocalShellPreferences(const LocalShellPreferences &preferences);
    SessionListPreferences sessionListPreferences() const { return m_sessionList; }
    void setSessionListPreferences(const SessionListPreferences &preferences);
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
    void deleteSelectedItem();
    void createSession();
    // The New Session window over `over`, with only `protocol` when given;
    // the saved session's id, or empty when cancelled.
    QString createSessionOver(QWidget *over, const QString &protocol);
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

signals:
    // Any appearance setting or the list of looks changed.
    void appearanceChanged();
    // A look was applied, or looks were saved, renamed or deleted: every
    // appearance control may show something else now.
    void looksChanged();

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
public:
    QPalette paletteFor(const QString &themeKey, const QHash<QString, QColor> &customColors) const;
private:
    void syncMenuChecks();
    TerminalAppearance effectiveTerminalAppearance() const;
    bool ensureVaultUnlocked();
    bool appendSessionYaml(const YAML::Node &roots, QTreeWidgetItem *parent, QString *error);
    YAML::Node sessionsDocument(const QTreeWidgetItem *root) const;
    bool appendCompactSessionYaml(const YAML::Node &folders, QTreeWidgetItem *parent,
                                  QString *error, const YAML::Node &rootSessions = YAML::Node());
    void restoreFolderExpansion();
    void saveFolderExpansion() const;
    QString folderStateKey(const QTreeWidgetItem *folder) const;
    void loadSessions();
    void saveSessions();
    void addExampleSessions();
    QTreeWidgetItem *selectedFolder() const;
    int folderDepth(const QTreeWidgetItem *folder) const;
    QTreeWidgetItem *addFolder(const QString &name, QTreeWidgetItem *parent);
    void addSession(const SessionDefinition &session, QTreeWidgetItem *parent);
    QString sessionLabel(const SessionDefinition &session) const;
    void openSession(const SessionDefinition &session);
    void setItemKind(QTreeWidgetItem *item, ItemKind kind);
    bool isFolder(const QTreeWidgetItem *item) const;
    QTreeWidgetItem *selectedItem() const;
    bool isQuickItem(const QTreeWidgetItem *item) const;
    QTreeWidgetItem *realSessionItem(const QString &id) const;
    void recordRecentSession(const QString &id);
    void applySessionListPreferences();
    int summaryColumn() const;
    QPair<int, int> folderStatusCounts(const QTreeWidgetItem *folder) const;
    void refreshSessionList();
    void fitSessionPanel();
    void checkHostStatus();
    void applySessionFilter();
    QList<TerminalPage *> terminalPagesIn(QWidget *tab) const;
    QList<TerminalPage *> allTerminalPages() const;
    TerminalPage *activeTerminal(QWidget *tab) const;
    int tabIndexOf(QWidget *page) const;
    void splitActivePane(Qt::Orientation orientation, const QString &what = QStringLiteral("choose"));
    void addSplitMenus(QMenu *menu, const std::function<bool()> &makeActive);
    std::optional<QString> choosePaneSession(const QString &currentId);
    void insertPane(TerminalPage *beside, TerminalPage *added, Qt::Orientation orientation);
    void closePane(TerminalPage *pane);
    void chooseMultiExecTargets();
    TerminalPage *newLocalProgramPage(const QString &program, const QStringList &arguments,
                                      const QString &label, const TerminalAppearance &appearance);
    void focusSessionSearch();
    QTreeWidgetItem *firstVisibleSession() const;
    void showSftpPanelForCurrentTab();
    void setThemedIcon(QAction *action, const QString &name, QStyle::StandardPixmap fallback);
    void refreshIcons();
    void applyTerminalAppearanceToTabs();
    void connectHostField(QComboBox *protocolCombo, QLineEdit *hostEdit, QLineEdit *portEdit);
    TerminalAppearanceEditor *addTerminalAppearanceGroup(QFormLayout *form, QComboBox *protocolCombo,
                                                         const TerminalAppearance &value,
                                                         const SshOptions &ssh,
                                                         SshOptionsEditor **sshEditor,
                                                         const SerialOptions &serial,
                                                         SerialOptionsEditor **serialEditor);
    FtpOptionsEditor *addFtpOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                         const FtpOptions &value);
    RshOptionsEditor *addRshOptionsTab(QComboBox *protocolCombo, TerminalAppearanceEditor *sshTabsMember,
                                       const RshOptions &value);
    XdmcpOptionsEditor *addXdmcpOptionsGroup(QFormLayout *form, QComboBox *protocolCombo,
                                             const XdmcpOptions &value);
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
    QList<Look> m_userLooks;
    QPalette m_systemPalette;
    QFont m_systemFont;
    QString m_iconTheme;
    QString m_darkTerminalColorScheme;
    QActionGroup *m_styleGroup = nullptr;
    QActionGroup *m_colorThemeGroup = nullptr;
    QActionGroup *m_iconThemeGroup = nullptr;
    QPointer<PreferencesWindow> m_preferencesWindow;
    TunnelsWindow *m_tunnelsWindow = nullptr;
    QPointer<NetworkToolsWindow> m_networkTools;
    // Sessions waiting after a Wake-on-LAN packet before they connect.
    QSet<QString> m_waking;
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
    SessionListPreferences m_sessionList;
    LocalShellPreferences m_localShell;
    QStringList m_recentSessions;
    QTreeWidgetItem *m_pinnedFolder = nullptr;
    QTreeWidgetItem *m_recentFolder = nullptr;
    HostStatusChecker *m_hostStatus = nullptr;
    QTimer *m_statusTimer = nullptr;
    QTimer *m_blinkTimer = nullptr;
    bool m_blinkOn = false;
    QLineEdit *m_sessionSearch = nullptr;
    QLabel *m_noSessionMatches = nullptr;
    // While a search filters the list, expanding folders to show matches is
    // not remembered; the folders' own state comes back afterwards.
    bool m_filtering = false;
    QToolButton *m_multiExecIndicator = nullptr;
    LocalServersWindow *m_localServersWindow = nullptr;
    // Set while Split Right / Split Down opens the new pane's terminal.
    struct PendingSplit
    {
        TerminalPage *beside = nullptr;
        Qt::Orientation orientation = Qt::Horizontal;
    };
    PendingSplit m_pendingSplit;
    bool m_pinnedExpandedBeforeSearch = true;
    bool m_recentExpandedBeforeSearch = true;
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
