// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class MainWindow;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QTabWidget;
class SettingsPage;

// Settings window in the style of Windows 11: pages listed on the left with a
// search box, each page made of cards of setting rows. Changes apply
// immediately; there is no OK or Cancel.
class PreferencesWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit PreferencesWindow(MainWindow *mainWindow);

    // Shows the window at a category or page id ("appearance", "rdp",
    // "about"), or keeps the current page when id is empty.
    void showPage(const QString &id);

protected:
    void changeEvent(QEvent *event) override;

private:
    // Pages added between beginCategory() and endCategory() become tabs of
    // one category in the navigation list; others are categories of their own.
    void beginCategory(const QString &id, const QString &title, const QString &iconName,
                       int fallbackIcon);
    void endCategory();
    SettingsPage *addPage(const QString &id, const QString &title, const QString &iconName,
                          int fallbackIcon);
    void buildAppearancePage();
    void buildTerminalPage();
    void buildLocalShellPage();
    void buildStartupPage();
    void buildShortcutsPage();
    void buildSshPage();
    void buildRdpPage();
    void buildVncPage();
    void buildFtpPage();
    void buildRunCommandPage();
    void buildHttpRequestPage();
    void buildRshPage();
    void buildXdmcpPage();
    void buildProxyPage();
    void buildLooksPage();
    void buildPages();
    // Builds every page again (after a look changed many settings at once).
    void rebuild();
    void buildSftpPage();
    void buildSecurityPage();
    void buildLoggingPage();
    void buildSnippetsPage();
    void buildHighlightsPage();
    void buildPresentationPage();
    void buildSessionListPage();
    void buildDataPage();
    void buildAboutPage();
    void filter(const QString &text);
    void applyStyleSheet();

    MainWindow *m_mainWindow = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_navigation = nullptr;
    QStackedWidget *m_pages = nullptr;
    struct Category
    {
        QString id;
        QTabWidget *tabs = nullptr; // nullptr: a single page
        QList<SettingsPage *> pages;
    };
    QList<Category> m_categories;
    bool m_inCategory = false;
    bool m_applyingStyle = false;
};
