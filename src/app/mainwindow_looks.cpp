// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

// MainWindow: looks and themes: interface style, colours, fonts, borders, terminal appearance, icons.

#include "mainwindow_p.h"

using namespace mainwindow_p;

namespace {

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

} // namespace

void MainWindow::applyUiStyle(const QString &styleKey)
{
    QString key = styleKey;
    if (key == QStringLiteral("system"))
        key = m_systemStyleKey;

    QStyle *style = key.compare(OpsRemoteStyle::key(), Qt::CaseInsensitive) == 0
                        ? new OpsRemoteStyle
                    : KvantumThemes::isKvantumKey(key) ? KvantumThemes::create(key)
                                                        : QStyleFactory::create(key);
    if (!style && key.compare(m_systemStyleKey, Qt::CaseInsensitive) != 0)
        style = QStyleFactory::create(m_systemStyleKey);
    if (!style)
        return;

    QApplication::setStyle(style);
    applyColorTheme(m_colorThemeKey);
}

void MainWindow::applyColorTheme(const QString &requestedKey)
{
    // A Kvantum theme brings its own colours: "System" uses those.
    if (requestedKey == QStringLiteral("system") && KvantumThemes::isKvantumKey(m_selectedStyleKey)) {
        QApplication::setPalette(withFadedDisabledButtons(QApplication::style()->standardPalette()));
        return;
    }
    QApplication::setPalette(withFadedDisabledButtons(paletteFor(requestedKey, m_customColors)));
}

// An inactive button's text halfway between its text and its background, so
// it looks inactive in every look (some themes barely change it).
QPalette MainWindow::withFadedDisabledButtons(QPalette palette)
{
    const QColor text = palette.color(QPalette::Active, QPalette::ButtonText);
    const QColor button = palette.color(QPalette::Active, QPalette::Button);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     QColor((text.red() + button.red()) / 2, (text.green() + button.green()) / 2,
                            (text.blue() + button.blue()) / 2));
    return palette;
}

// The palette of a colour mode, without applying it (also for previews).
QPalette MainWindow::paletteFor(const QString &requestedKey,
                                const QHash<QString, QColor> &customColors) const
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
        const auto window = customColors.value(QStringLiteral("window"),
                                                 palette.color(QPalette::Window));
        const auto base = customColors.value(QStringLiteral("base"),
                                               palette.color(QPalette::Base));
        const auto text = customColors.value(QStringLiteral("text"),
                                               palette.color(QPalette::Text));
        const auto highlight = customColors.value(QStringLiteral("highlight"),
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
    // Frame, bevel and tab-bar lines use these roles. Taken from the system
    // palette they stay light, which drew a white line in dark themes; derive
    // them from the theme's own button colour, as QPalette(button) does.
    if (palette.color(QPalette::Button) != m_systemPalette.color(QPalette::Button)) {
        const QColor button = palette.color(QPalette::Button);
        const bool dark = button.lightness() < 128;
        const QColor light = dark ? button.lighter(160) : button.lighter(150);
        const QColor shade = dark ? button.darker(160) : button.darker(200);
        for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            palette.setColor(group, QPalette::Light, light);
            palette.setColor(group, QPalette::Midlight,
                             QColor((button.red() + light.red()) / 2,
                                    (button.green() + light.green()) / 2,
                                    (button.blue() + light.blue()) / 2));
            palette.setColor(group, QPalette::Mid, dark ? button.lighter(130) : button.darker(150));
            palette.setColor(group, QPalette::Dark, shade);
            palette.setColor(group, QPalette::Shadow, dark ? button.darker(250) : button.darker(300));
        }
    }
    return palette;
}

void MainWindow::editCustomColors()
{
    ThemeColorDialog dialog(m_customColors, QApplication::palette(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_customColors = dialog.colors();
    setColorTheme(QStringLiteral("custom"));
}

// ---- Preferences API (used by the View menu and the Preferences window) ----

// Our own style first; the classic "Windows" (95) style is left out.
QStringList MainWindow::availableUiStyles() const
{
    QStringList styles{OpsRemoteStyle::key()};
    for (const auto &key : QStyleFactory::keys()) {
        if (key.compare(QStringLiteral("Windows"), Qt::CaseInsensitive) != 0)
            styles.append(key);
    }
    for (const auto &theme : KvantumThemes::available())
        styles.append(KvantumThemes::Prefix + theme);
    return styles;
}

// How a style key is shown: "Kvantum: KvArc" for Kvantum themes.
QString MainWindow::uiStyleLabel(const QString &styleKey) const
{
    if (KvantumThemes::isKvantumKey(styleKey))
        return QStringLiteral("Kvantum: %1").arg(styleKey.mid(KvantumThemes::Prefix.size()));
    return styleKey;
}

// ---- Looks ----

QList<Look> MainWindow::looks() const
{
    return builtInLooks() + m_userLooks;
}

Look MainWindow::currentLook() const
{
    Look look;
    look.colorTheme = m_colorThemeKey;
    look.customColors = m_customColors;
    look.style = m_selectedStyleKey;
    look.iconTheme = m_iconTheme;
    look.terminalScheme = m_terminalAppearance.colorScheme;
    look.darkTerminalScheme = m_darkTerminalColorScheme;
    look.terminalFontFamily = m_terminalAppearance.fontFamily;
    look.terminalFontSize = m_terminalAppearance.fontSize;
    const auto font = loadInterfaceFont();
    look.interfaceFontFamily = font.family;
    look.interfaceFontSize = font.pointSize;
    look.windowBorder = windowBordersEnabled();
    look.cornerRadius = RoundedCorners::radius();
    look.seeThroughLists = ::seeThroughLists();
    return look;
}

// The look the current settings match, or empty for "Custom".
QString MainWindow::matchingLookId() const
{
    const Look current = currentLook();
    for (const auto &look : looks()) {
        if (look.sameAppearance(current))
            return look.id;
    }
    return {};
}

void MainWindow::applyLook(const Look &look)
{
    {
        const QSignalBlocker blocker(this);
        if (look.colorTheme == QStringLiteral("custom"))
            m_customColors = look.customColors;
        setColorTheme(look.colorTheme);
        setUiStyle(look.style.isEmpty() ? QStringLiteral("system") : look.style);
        bool knownIcons = look.iconTheme.isEmpty();
        for (const auto &theme : availableIconThemes())
            knownIcons = knownIcons || theme.first == look.iconTheme;
        setIconThemeChoice(knownIcons ? look.iconTheme : QString());
        setInterfaceFont(look.interfaceFontFamily, look.interfaceFontSize);
        setWindowBorders(look.windowBorder);
        setCornerRadius(look.cornerRadius);
        setSeeThroughLists(look.seeThroughLists);
        TerminalAppearance terminal;
        terminal.colorScheme = look.terminalScheme;
        terminal.fontFamily = look.terminalFontFamily;
        terminal.fontSize = look.terminalFontSize;
        setDefaultTerminalAppearance(terminal);
        setDarkTerminalColorScheme(look.darkTerminalScheme);
    }
    setBaseLook(look.id);
    emit appearanceChanged();
    emit looksChanged();
}

// Saves the current settings as a new look; false if the limit is reached.
bool MainWindow::saveCurrentLookAs(const QString &name)
{
    if (m_userLooks.size() >= MaximumUserLooks)
        return false;
    Look look = currentLook();
    look.id = QStringLiteral("user:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    look.name = name;
    m_userLooks.append(look);
    saveUserLooks(m_userLooks);
    setBaseLook(look.id);
    emit appearanceChanged();
    emit looksChanged();
    return true;
}

bool MainWindow::addLook(Look look)
{
    if (m_userLooks.size() >= MaximumUserLooks)
        return false;
    look.id = QStringLiteral("user:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_userLooks.append(look);
    saveUserLooks(m_userLooks);
    emit looksChanged();
    emit appearanceChanged();
    return true;
}

void MainWindow::overwriteLook(const QString &id)
{
    const Look current = currentLook();
    for (auto &look : m_userLooks) {
        if (look.id != id)
            continue;
        Look updated = current;
        updated.id = look.id;
        updated.name = look.name;
        look = updated;
    }
    saveUserLooks(m_userLooks);
    setBaseLook(id);
    emit appearanceChanged();
    emit looksChanged();
}

// The look the appearance came from, kept in the settings so that "Revert
// Unsaved Changes" still knows it after changes by hand and after a restart.
void MainWindow::setBaseLook(const QString &id)
{
    m_baseLookId = id;
    auto ui = loadPreferencesSection("ui");
    ui["base_look"] = id.toStdString();
    savePreferencesSection("ui", ui);
}

// Remembered from the start: the look the appearance matches then, when no
// look was applied yet (older settings).
void MainWindow::rememberStartLook()
{
    if (m_baseLookId.isEmpty())
        if (const auto id = matchingLookId(); !id.isEmpty())
            setBaseLook(id);
}

QString MainWindow::baseLookId() const
{
    for (const auto &look : looks()) {
        if (look.id == m_baseLookId)
            return m_baseLookId;
    }
    return matchingLookId();
}

void MainWindow::renameLook(const QString &id, const QString &name)
{
    for (auto &look : m_userLooks) {
        if (look.id == id)
            look.name = name;
    }
    saveUserLooks(m_userLooks);
    emit appearanceChanged();
    emit looksChanged();
}

void MainWindow::deleteLook(const QString &id)
{
    m_userLooks.removeIf([&id](const Look &look) { return look.id == id; });
    saveUserLooks(m_userLooks);
    if (m_baseLookId == id)
        setBaseLook(QString());
    emit appearanceChanged();
    emit looksChanged();
}

void MainWindow::setUiStyle(const QString &styleKey)
{
    m_selectedStyleKey = styleKey;
    applyUiStyle(styleKey);
    saveUiStylePreference(styleKey);
    syncMenuChecks();    emit appearanceChanged();
}

QList<QPair<QString, QString>> MainWindow::colorThemeChoices() const
{
    QList<QPair<QString, QString>> choices{{QStringLiteral("auto"), QStringLiteral("Automatic (follow OS)")},
                                           {QStringLiteral("system"), QStringLiteral("System (or the style's own)")},
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
    syncMenuChecks();    emit appearanceChanged();
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
    syncMenuChecks();    emit appearanceChanged();
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
    emit appearanceChanged();
}

bool MainWindow::windowBorders() const
{
    return windowBordersEnabled();
}

void MainWindow::setWindowBorders(bool enabled)
{
    saveWindowBorderPreference(enabled);
    setWindowBordersEnabled(enabled);    emit appearanceChanged();
}

int MainWindow::cornerRadius() const
{
    return RoundedCorners::radius();
}

void MainWindow::setCornerRadius(int radius)
{
    radius = validCornerRadius(radius);
    saveCornerRadiusPreference(radius);
    RoundedCorners::setRadius(radius);
    emit appearanceChanged();
}

bool MainWindow::seeThroughLists() const
{
    return ::seeThroughLists();
}

void MainWindow::setSeeThroughLists(bool seeThrough)
{
    saveSeeThroughListsPreference(seeThrough);
    ::setSeeThroughLists(seeThrough);
    emit appearanceChanged();
}

void MainWindow::setDefaultTerminalAppearance(const TerminalAppearance &appearance)
{
    m_terminalAppearance = appearance;
    saveTerminalAppearance(appearance);
    applyTerminalAppearanceToTabs();    emit appearanceChanged();
}

void MainWindow::setDarkTerminalColorScheme(const QString &scheme)
{
    m_darkTerminalColorScheme = scheme;
    saveDarkTerminalColorScheme(scheme);
    applyTerminalAppearanceToTabs();
    emit appearanceChanged();
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

void MainWindow::applyTerminalAppearanceToTabs()
{
    for (auto *page : allTerminalPages()) {
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
        if (isQuickItem(item))
            return; // rebuilt below
        setItemKind(item, isFolder(item) ? ItemKind::Folder : ItemKind::Session);
        for (int index = 0; index < item->childCount(); ++index)
            refreshItem(item->child(index));
    };
    for (int index = 0; index < m_sessionTree->topLevelItemCount(); ++index)
        refreshItem(m_sessionTree->topLevelItem(index));
    refreshSessionList();
    for (auto *page : allTerminalPages()) {
        if (page->sftpPanel())
            page->sftpPanel()->refreshIcons();
    }
    for (auto *page : m_sessionTabs->findChildren<FilesPage *>())
        page->refreshIcons();
}

// Widgets under a style sheet keep the font they were polished with when the
// application font changes (Qt only updates the others): polish them again.
void refreshWidgetFonts()
{
    for (auto *widget : QApplication::allWidgets()) {
        if (widget->testAttribute(Qt::WA_SetFont))
            continue; // a font of its own (headings follow by hand)
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->updateGeometry();
    }
}
