// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "preferenceswindow.h"
#include "availabilitynote.h"
#include "localresources.h"
#include "systemcredentials.h"
#include "appdirs.h"
#include "icons.h"
#include "mainwindow.h"
#include "masterpassworddialog.h"
#include "preferences.h"
#include "rdpoptions.h"
#include "proxy.h"
#include "vncoptions.h"
#include "ftpoptions.h"
#include "rshoptions.h"
#include "xdmcp.h"
#include "terminalappearance.h"

#include <QApplication>
#include <QColorDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QTableWidget>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFontComboBox>
#include <QGuiApplication>
#include <QKeySequenceEdit>
#include <QMap>
#include <QToolButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStandardItemModel>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleHints>
#include <QSysInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTabWidget>
#include <QStyledItemDelegate>
#include <QPainterPath>
#include <QTimer>

#include <optional>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include <libssh/libssh.h>
#include <sodium.h>

#ifndef OPSREMOTE_VERSION
#define OPSREMOTE_VERSION "dev"
#endif

namespace {
QLabel *secondaryLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    return label;
}

// A combo box of (value, label) choices showing current.
QComboBox *choiceBox(const QList<QPair<QString, QString>> &choices, const QString &current)
{
    auto *box = new QComboBox;
    for (const auto &choice : choices)
        box->addItem(choice.second, choice.first);
    box->setCurrentIndex(qMax(0, box->findData(current)));
    return box;
}

// Bold heading text, scale times the interface font. PreferencesWindow
// re-applies it when the interface font changes.
void setHeadingFont(QLabel *label, qreal scale)
{
    label->setProperty("headingScale", scale);
    QFont font = QApplication::font();
    font.setPointSizeF(font.pointSizeF() * scale);
    font.setBold(true);
    label->setFont(font);
}

// An on/off switch in the style of Windows 11, drawn from the palette so it
// is visible in every colour mode. It is a QCheckBox, so it works like one.
class ToggleSwitch final : public QCheckBox
{
public:
    using QCheckBox::QCheckBox;

    QSize sizeHint() const override
    {
        const int textWidth = text().isEmpty() ? 0 : fontMetrics().horizontalAdvance(text()) + 8;
        return {TrackWidth + textWidth, qMax(TrackHeight + 4, fontMetrics().height())};
    }

protected:
    bool hitButton(const QPoint &position) const override { return rect().contains(position); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF track(0.5, (height() - TrackHeight) / 2.0 + 0.5, TrackWidth - 1, TrackHeight - 1);
        const bool on = isChecked();
        const auto accent = palette().color(QPalette::Highlight);
        QColor border = palette().color(QPalette::Text);
        border.setAlphaF(isEnabled() ? 0.6 : 0.3);
        painter.setPen(on ? QPen(accent) : QPen(border, 1));
        QColor fill = accent;
        if (!isEnabled())
            fill.setAlphaF(0.4);
        painter.setBrush(on ? fill : Qt::transparent);
        painter.drawRoundedRect(track, TrackHeight / 2.0, TrackHeight / 2.0);
        const qreal knob = TrackHeight - 8;
        const qreal x = on ? track.right() - 4 - knob : track.left() + 4;
        painter.setPen(Qt::NoPen);
        painter.setBrush(on ? palette().color(QPalette::HighlightedText) : border);
        painter.drawEllipse(QRectF(x, track.center().y() - knob / 2, knob, knob));
        if (hasFocus()) {
            painter.setPen(QPen(accent, 1, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(track.adjusted(-2, -2, 2, 2), TrackHeight / 2.0 + 2, TrackHeight / 2.0 + 2);
        }
        if (!text().isEmpty()) {
            painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                           QPalette::WindowText));
            painter.drawText(QRect(TrackWidth + 8, 0, width() - TrackWidth - 8, height()),
                             Qt::AlignVCenter | Qt::AlignLeft, this->text());
        }
    }

private:
    static constexpr int TrackWidth = 40;
    static constexpr int TrackHeight = 20;
};

QCheckBox *toggle(bool checked)
{
    auto *box = new ToggleSwitch;
    box->setChecked(checked);
    return box;
}
}

// One rounded card of setting rows. Each row remembers the text it can be
// found by.
class SettingsCard final : public QFrame
{
public:
    explicit SettingsCard(const QString &title, QWidget *parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("settingsCard"));
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(16, 12, 16, 12);
        m_layout->setSpacing(0);
        if (!title.isEmpty()) {
            auto *label = new QLabel(title, this);
            setHeadingFont(label, 1.0);
            label->setContentsMargins(0, 0, 0, 6);
            m_layout->addWidget(label);
        }
    }

    // A row: title and description on the left, the control on the right.
    QWidget *addRow(const QString &title, const QString &description, QWidget *control,
                    const QString &keywords = QString())
    {
        auto *row = new QWidget(this);
        row->setMinimumHeight(48);
        row->setObjectName(QStringLiteral("settingsRow"));
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 8, 0, 8);
        auto *texts = new QVBoxLayout;
        texts->setSpacing(2);
        texts->setContentsMargins(0, 0, 16, 0);
        texts->addWidget(new QLabel(title, row));
        if (!description.isEmpty())
            texts->addWidget(secondaryLabel(description, row));
        layout->addLayout(texts, 1);
        if (control) {
            control->setParent(row);
            layout->addWidget(control, 0, Qt::AlignVCenter);
        }
        registerRow(row, title + QLatin1Char(' ') + description + QLatin1Char(' ') + keywords);
        return row;
    }

    // A row whose content spans the card's width.
    QWidget *addWideRow(QWidget *content, const QString &keywords)
    {
        content->setParent(this);
        registerRow(content, keywords);
        return content;
    }

    // Shows the rows matching every word of the search; returns whether any did.
    bool filter(const QStringList &words)
    {
        bool any = false;
        for (auto *row : std::as_const(m_rows)) {
            const auto text = row->property("searchText").toString();
            bool match = true;
            for (const auto &word : words)
                match = match && text.contains(word, Qt::CaseInsensitive);
            row->setVisible(match);
            if (auto *divider = qobject_cast<QWidget *>(row->property("divider").value<QObject *>()))
                divider->setVisible(match && any);
            any = any || match;
        }
        setVisible(any);
        return any;
    }

private:
    void registerRow(QWidget *row, const QString &searchText)
    {
        // A thin divider above every row but the first, hidden with its row.
        if (!m_rows.isEmpty()) {
            auto *divider = new QFrame(this);
            divider->setObjectName(QStringLiteral("settingsDivider"));
            divider->setFixedHeight(1);
            m_layout->addWidget(divider);
            row->setProperty("divider", QVariant::fromValue<QObject *>(divider));
        }
        row->setProperty("searchText", searchText);
        m_layout->addWidget(row);
        m_rows.append(row);
    }

    QVBoxLayout *m_layout = nullptr;
    QList<QWidget *> m_rows;
};

// One page: a large title, then cards, scrolling as a whole.
class SettingsPage final : public QScrollArea
{
public:
    SettingsPage(const QString &id, const QString &title, bool heading, QWidget *parent)
        : QScrollArea(parent), m_id(id)
    {
        setWidgetResizable(true);
        setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget(this);
        content->setObjectName(QStringLiteral("settingsPageContent"));
        m_layout = new QVBoxLayout(content);
        m_layout->setContentsMargins(28, 20, 28, 24);
        m_layout->setSpacing(12);
        // A page shown as a tab has its category's heading above the tabs.
        if (heading) {
            auto *label = new QLabel(title, content);
            setHeadingFont(label, 1.6);
            m_layout->addWidget(label);
        } else {
            m_layout->setContentsMargins(20, 16, 20, 24);
        }
        m_layout->addStretch();
        setWidget(content);
    }

    QString id() const { return m_id; }

    SettingsCard *addCard(const QString &title = QString())
    {
        auto *card = new SettingsCard(title, widget());
        m_layout->insertWidget(m_layout->count() - 1, card);
        m_cards.append(card);
        return card;
    }

    bool filter(const QStringList &words)
    {
        bool any = false;
        for (auto *card : std::as_const(m_cards))
            any = card->filter(words) || any;
        return any;
    }

private:
    QString m_id;
    QVBoxLayout *m_layout = nullptr;
    QList<SettingsCard *> m_cards;
};

// One look in the Looks list: a small preview of its colours, the name
// (bold when in use) and what it sets.
class LookDelegate final : public QStyledItemDelegate
{
public:
    enum Role { NameRole = Qt::UserRole + 1, DescriptionRole, InUseRole, ColorsRole };
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return {200, option.fontMetrics.height() * 2 + 18};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem base(option);
        initStyleOption(&base, index);
        base.text.clear();
        base.icon = QIcon();
        option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &base, painter, option.widget);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect rect = option.rect.adjusted(8, 6, -8, -6);
        // Preview: window colour, a panel in the base colour with a line of
        // text, and the accent.
        const auto colors = index.data(ColorsRole).toList();
        const QRectF tile(rect.left(), rect.top(), 52, rect.height());
        // With two variants (light, dark) the dark one fills the lower right.
        for (qsizetype variant = 0; variant + 3 < colors.size(); variant += 4) {
            painter->save();
            if (variant > 0) {
                QPainterPath half;
                half.moveTo(tile.topRight());
                half.lineTo(tile.bottomRight());
                half.lineTo(tile.bottomLeft());
                half.closeSubpath();
                painter->setClipPath(half);
            }
            painter->setPen(QPen(option.palette.color(QPalette::Mid), 1));
            painter->setBrush(colors[variant].value<QColor>());
            painter->drawRoundedRect(tile, 5, 5);
            painter->setPen(Qt::NoPen);
            painter->setBrush(colors[variant + 1].value<QColor>());
            painter->drawRoundedRect(tile.adjusted(6, 6, -6, -6), 3, 3);
            painter->setBrush(colors[variant + 2].value<QColor>());
            painter->drawRoundedRect(QRectF(tile.left() + 10, tile.top() + 10, 20, 3), 1.5, 1.5);
            painter->setBrush(colors[variant + 3].value<QColor>());
            painter->drawRoundedRect(QRectF(tile.left() + 10, tile.bottom() - 13, 28, 4), 2, 2);
            painter->restore();
        }
        const bool selected = option.state & QStyle::State_Selected;
        const QColor text = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        QFont nameFont = option.font;
        nameFont.setBold(true);
        const QRect textRect = rect.adjusted(int(tile.width()) + 12, 0, 0, 0);
        painter->setFont(nameFont);
        painter->setPen(text);
        QString name = index.data(NameRole).toString();
        if (index.data(InUseRole).toBool())
            name += QStringLiteral("  ✓ in use");
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignTop, name);
        painter->setFont(option.font);
        QColor muted = text;
        muted.setAlphaF(selected ? 0.85 : 0.65);
        painter->setPen(muted);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignBottom,
                          QFontMetrics(option.font).elidedText(index.data(DescriptionRole).toString(),
                                                               Qt::ElideRight, textRect.width()));
        painter->restore();
    }
};

PreferencesWindow::PreferencesWindow(MainWindow *mainWindow)
    : QWidget(mainWindow, Qt::Window), m_mainWindow(mainWindow)
{
    setWindowTitle(QStringLiteral("Preferences"));
    resize(980, 700);
    applyStyleSheet();

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *side = new QWidget(this);
    side->setFixedWidth(215);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(12, 16, 8, 12);
    auto *title = new QLabel(QStringLiteral("Preferences"), side);
    setHeadingFont(title, 1.25);
    sideLayout->addWidget(title);
    m_search = new QLineEdit(side);
    m_search->setPlaceholderText(QStringLiteral("Find a setting"));
    m_search->setClearButtonEnabled(true);
    sideLayout->addWidget(m_search);
    m_navigation = new QListWidget(side);
    m_navigation->setObjectName(QStringLiteral("settingsNavigation"));
    m_navigation->setIconSize(QSize(16, 16));
    m_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideLayout->addWidget(m_navigation, 1);
    layout->addWidget(side);

    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages, 1);

    buildPages();
    // A look changes many settings at once: show them all anew.
    connect(m_mainWindow, &MainWindow::looksChanged, this, [this] {
        QTimer::singleShot(0, this, &PreferencesWindow::rebuild);
    });

    connect(m_navigation, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_search, &QLineEdit::textChanged, this, &PreferencesWindow::filter);
    m_navigation->setCurrentRow(0);
}

void PreferencesWindow::buildPages()
{
    beginCategory(QStringLiteral("appearance"), QStringLiteral("Appearance"),
                  QStringLiteral("preferences-desktop-theme"), QStyle::SP_DesktopIcon);
    buildAppearancePage();
    buildSessionListPage();
    endCategory();
    beginCategory(QStringLiteral("terminal-category"), QStringLiteral("Terminal"),
                  QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    buildTerminalPage();
    buildLocalShellPage();
    buildHighlightsPage();
    buildSnippetsPage();
    buildLoggingPage();
    endCategory();
    beginCategory(QStringLiteral("connections"), QStringLiteral("Connections"),
                  QStringLiteral("network-workgroup"), QStyle::SP_DriveNetIcon);
    buildSshPage();
    buildProxyPage();
    buildRdpPage();
    buildVncPage();
    buildSftpPage();
    buildFtpPage();
    buildRshPage();
    buildXdmcpPage();
    buildRunCommandPage();
    buildHttpRequestPage();
    endCategory();
    beginCategory(QStringLiteral("workspace"), QStringLiteral("Workspace"),
                  QStringLiteral("preferences-system"), QStyle::SP_TitleBarMaxButton);
    buildStartupPage();
    buildShortcutsPage();
    buildPresentationPage();
    endCategory();
    beginCategory(QStringLiteral("security"), QStringLiteral("Security and vault"),
                  QStringLiteral("security-high"), QStyle::SP_MessageBoxWarning);
    buildSecurityPage();
    endCategory();
    beginCategory(QStringLiteral("data"), QStringLiteral("Data and backup"),
                  QStringLiteral("drive-removable-media"), QStyle::SP_DriveFDIcon);
    buildDataPage();
    endCategory();
    beginCategory(QStringLiteral("about"), QStringLiteral("About"),
                  QStringLiteral("help-about"), QStyle::SP_MessageBoxInformation);
    buildAboutPage();
    endCategory();

}

// Keeps the category and tab that were showing.
void PreferencesWindow::rebuild()
{
    const int row = m_navigation->currentRow();
    QList<int> tabs;
    for (const auto &category : std::as_const(m_categories))
        tabs.append(category.tabs ? category.tabs->currentIndex() : 0);
    const QSignalBlocker blocker(m_navigation);
    m_navigation->clear();
    while (m_pages->count() > 0) {
        auto *page = m_pages->widget(0);
        m_pages->removeWidget(page);
        page->deleteLater();
    }
    m_categories.clear();
    buildPages();
    for (int index = 0; index < m_categories.size() && index < tabs.size(); ++index) {
        if (m_categories[index].tabs)
            m_categories[index].tabs->setCurrentIndex(tabs[index]);
    }
    m_navigation->setCurrentRow(qMax(0, row));
    m_pages->setCurrentIndex(qMax(0, row));
    filter(m_search->text());
}

void PreferencesWindow::showPage(const QString &id)
{
    for (int index = 0; index < m_categories.size(); ++index) {
        const auto &category = m_categories[index];
        int tab = category.id == id ? 0 : -1;
        for (int page = 0; page < category.pages.size() && tab < 0; ++page) {
            if (category.pages[page]->id() == id)
                tab = page;
        }
        if (tab < 0)
            continue;
        m_search->clear();
        m_navigation->setCurrentRow(index);
        if (category.tabs)
            category.tabs->setCurrentIndex(tab);
    }
    show();
    raise();
    activateWindow();
}

void PreferencesWindow::beginCategory(const QString &id, const QString &title,
                                      const QString &iconName, int fallbackIcon)
{
    auto *container = new QWidget(m_pages);
    container->setObjectName(QStringLiteral("settingsPageContent"));
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(28, 20, 28, 0);
    layout->setSpacing(10);
    auto *heading = new QLabel(title, container);
    setHeadingFont(heading, 1.6);
    layout->addWidget(heading);
    auto *tabs = new QTabWidget(container);
    tabs->setDocumentMode(true);
    layout->addWidget(tabs, 1);
    m_pages->addWidget(container);
    m_navigation->addItem(new QListWidgetItem(
        fixedSizeIcon(themedIcon(iconName, static_cast<QStyle::StandardPixmap>(fallbackIcon)),
                      m_navigation->iconSize()),
        title));
    m_categories.append({id, tabs, {}});
    m_inCategory = true;
}

void PreferencesWindow::endCategory()
{
    m_inCategory = false;
}

SettingsPage *PreferencesWindow::addPage(const QString &id, const QString &title,
                                         const QString &iconName, int fallbackIcon)
{
    if (m_inCategory) {
        auto &category = m_categories.last();
        auto *page = new SettingsPage(id, title, false, category.tabs);
        category.tabs->addTab(page, title);
        category.pages.append(page);
        return page;
    }
    auto *page = new SettingsPage(id, title, true, m_pages);
    m_pages->addWidget(page);
    m_navigation->addItem(new QListWidgetItem(
        fixedSizeIcon(themedIcon(iconName, static_cast<QStyle::StandardPixmap>(fallbackIcon)),
                      m_navigation->iconSize()),
        title));
    m_categories.append({id, nullptr, {page}});
    return page;
}

// Hides rows, cards, tabs, and categories without a match.
void PreferencesWindow::filter(const QString &text)
{
    const auto words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    int firstVisible = -1;
    for (int index = 0; index < m_categories.size(); ++index) {
        auto &category = m_categories[index];
        bool any = false;
        for (int page = 0; page < category.pages.size(); ++page) {
            const bool visible = category.pages[page]->filter(words);
            if (category.tabs)
                category.tabs->setTabVisible(page, visible);
            any = any || visible;
        }
        if (category.tabs && any && !category.tabs->isTabVisible(category.tabs->currentIndex())) {
            for (int page = 0; page < category.pages.size(); ++page) {
                if (category.tabs->isTabVisible(page)) {
                    category.tabs->setCurrentIndex(page);
                    break;
                }
            }
        }
        m_navigation->item(index)->setHidden(!any);
        if (any && firstVisible < 0)
            firstVisible = index;
    }
    if (firstVisible >= 0 && m_navigation->item(m_navigation->currentRow())->isHidden())
        m_navigation->setCurrentRow(firstVisible);
}

// Looks: ready-made combinations of the settings on the Interface and
// Terminal tabs, and the user's own (up to MaximumUserLooks).
void PreferencesWindow::buildLooksPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("appearance-looks"), QStringLiteral("Looks"),
                         QStringLiteral("preferences-desktop-theme"), QStyle::SP_DesktopIcon);
    auto *card = page->addCard(QStringLiteral("Looks"));
    auto *intro = new QLabel(QStringLiteral(
        "A look sets the colours, style, icons, fonts, terminal colours and window border "
        "together. Changing any of them by hand makes the appearance Custom; save it as a look "
        "of your own to keep it."));
    intro->setWordWrap(true);
    card->addWideRow(intro, QStringLiteral("look theme preset"));

    auto *content = new QWidget;
    auto *layout = new QHBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *list = new QListWidget(content);
    list->setMinimumHeight(300);
    list->setItemDelegate(new LookDelegate(list));
    list->setSpacing(2);
    layout->addWidget(list, 1);
    auto *buttons = new QVBoxLayout;
    auto *apply = new QPushButton(QStringLiteral("Apply"), content);
    auto *save = new QPushButton(QStringLiteral("Save Current as…"), content);
    auto *rename = new QPushButton(QStringLiteral("Rename…"), content);
    auto *remove = new QPushButton(QStringLiteral("Delete"), content);
    for (auto *button : {apply, save, rename, remove})
        buttons->addWidget(button);
    buttons->addStretch();
    layout->addLayout(buttons);
    card->addWideRow(content, QStringLiteral("look apply save rename delete custom"));
    auto *current = new QLabel;
    card->addWideRow(current, QStringLiteral("current look custom"));

    const auto describe = [mainWindow](const Look &look) {
        QString colours;
        for (const auto &choice : mainWindow->colorThemeChoices()) {
            if (choice.first == look.colorTheme)
                colours = choice.second;
        }
        const QHash<QString, QString> styles{{QStringLiteral("opsremote"), QStringLiteral("OpsRemote style")},
                                             {QStringLiteral("fusion"), QStringLiteral("Fusion")},
                                             {QStringLiteral("windows11"), QStringLiteral("Windows 11")},
                                             {QStringLiteral("windowsvista"), QStringLiteral("Windows Vista")},
                                             {QStringLiteral("system"), QStringLiteral("System style")}};
        const QString styleKey = look.style.isEmpty() ? QStringLiteral("system") : look.style.toLower();
        QStringList parts{colours.isEmpty() ? look.colorTheme : colours,
                          styles.value(styleKey, mainWindow->uiStyleLabel(look.style)),
                          look.iconTheme.isEmpty() ? QStringLiteral("built-in icons") : look.iconTheme};
        if (!look.terminalScheme.isEmpty())
            parts.append(look.darkTerminalScheme.isEmpty()
                             ? look.terminalScheme
                             : QStringLiteral("%1 / %2").arg(look.terminalScheme, look.darkTerminalScheme));
        return parts.join(QStringLiteral(" · "));
    };
    // Fills the list; the look in use is marked and selected.
    const auto refresh = [mainWindow, list, current, describe] {
        const QString selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString()
                                                     : QString();
        const QString active = mainWindow->matchingLookId();
        list->clear();
        for (const auto &look : mainWindow->looks()) {
            const bool inUse = look.id == active;
            auto *item = new QListWidgetItem(look.name, list);
            item->setData(Qt::UserRole, look.id);
            item->setData(LookDelegate::NameRole,
                          look.isBuiltIn() ? look.name : look.name + QStringLiteral("  (yours)"));
            item->setData(LookDelegate::DescriptionRole, describe(look));
            item->setData(LookDelegate::InUseRole, inUse);
            // A look that follows the OS shows its light and dark variants.
            QVariantList colors;
            const QStringList variants = look.colorTheme == QStringLiteral("auto")
                                             ? QStringList{QStringLiteral("light"), QStringLiteral("dark")}
                                             : QStringList{look.colorTheme};
            for (const auto &variant : variants) {
                const QPalette colours = mainWindow->paletteFor(variant, look.customColors);
                colors << colours.color(QPalette::Window) << colours.color(QPalette::Base)
                       << colours.color(QPalette::Text) << colours.color(QPalette::Highlight);
            }
            item->setData(LookDelegate::ColorsRole, colors);
            if (look.id == (selected.isEmpty() ? active : selected))
                list->setCurrentItem(item);
        }
        QString name = QStringLiteral("Custom");
        for (const auto &look : mainWindow->looks()) {
            if (look.id == active)
                name = look.name;
        }
        current->setText(QStringLiteral("In use: <b>%1</b>").arg(name.toHtmlEscaped()));
    };
    const auto selectedLook = [mainWindow, list]() -> std::optional<Look> {
        if (!list->currentItem())
            return std::nullopt;
        const auto id = list->currentItem()->data(Qt::UserRole).toString();
        for (const auto &look : mainWindow->looks()) {
            if (look.id == id)
                return look;
        }
        return std::nullopt;
    };
    const auto updateButtons = [list, apply, rename, remove, save, selectedLook, mainWindow] {
        const auto look = selectedLook();
        apply->setEnabled(look.has_value());
        rename->setEnabled(look && !look->isBuiltIn());
        remove->setEnabled(look && !look->isBuiltIn());
        int own = 0;
        for (const auto &each : mainWindow->looks())
            own += each.isBuiltIn() ? 0 : 1;
        save->setEnabled(own < MaximumUserLooks);
        save->setToolTip(own < MaximumUserLooks
                             ? QString()
                             : QStringLiteral("Up to %1 looks of your own; delete one first.")
                                   .arg(MaximumUserLooks));
        Q_UNUSED(list);
    };
    refresh();
    updateButtons();
    connect(list, &QListWidget::currentItemChanged, this, updateButtons);
    connect(list, &QListWidget::itemDoubleClicked, apply, &QPushButton::click);
    connect(apply, &QPushButton::clicked, this, [mainWindow, selectedLook] {
        if (const auto look = selectedLook())
            mainWindow->applyLook(*look);
    });
    connect(save, &QPushButton::clicked, this, [this, mainWindow] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Save Look"),
                                                   QStringLiteral("Name of the look:"),
                                                   QLineEdit::Normal, QString(), &ok)
                                 .trimmed();
        if (ok && !name.isEmpty() && !mainWindow->saveCurrentLookAs(name))
            QMessageBox::information(this, QStringLiteral("Save Look"),
                                     QStringLiteral("You can keep up to %1 looks of your own. "
                                                    "Delete one first.").arg(MaximumUserLooks));
    });
    connect(rename, &QPushButton::clicked, this, [this, mainWindow, selectedLook] {
        const auto look = selectedLook();
        if (!look || look->isBuiltIn())
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Rename Look"),
                                                   QStringLiteral("Name of the look:"),
                                                   QLineEdit::Normal, look->name, &ok)
                                 .trimmed();
        if (ok && !name.isEmpty())
            mainWindow->renameLook(look->id, name);
    });
    connect(remove, &QPushButton::clicked, this, [this, mainWindow, selectedLook] {
        const auto look = selectedLook();
        if (look && !look->isBuiltIn()
            && QMessageBox::question(this, QStringLiteral("Delete Look"),
                                     QStringLiteral("Delete the look \"%1\"?").arg(look->name))
                   == QMessageBox::Yes)
            mainWindow->deleteLook(look->id);
    });
    // Settings changed one by one only move the "in use" mark.
    connect(mainWindow, &MainWindow::appearanceChanged, list, [refresh, updateButtons] {
        refresh();
        updateButtons();
    });
}

void PreferencesWindow::buildAppearancePage()
{
    auto *mainWindow = m_mainWindow;
    buildLooksPage();
    auto *page = addPage(QStringLiteral("appearance-interface"), QStringLiteral("Interface"),
                         QStringLiteral("preferences-desktop-theme"), QStyle::SP_DesktopIcon);

    // Colours and style
    auto *colours = page->addCard(QStringLiteral("Colours and style"));
    auto *colourMode = new QComboBox;
    for (const auto &choice : mainWindow->colorThemeChoices())
        colourMode->addItem(choice.second, choice.first);
    colourMode->setCurrentIndex(qMax(0, colourMode->findData(mainWindow->colorTheme())));
    colours->addRow(QStringLiteral("Colour mode"),
                    QStringLiteral("Automatic follows your operating system's light or dark "
                                   "setting and switches with it. With a Kvantum style, System "
                                   "uses the theme's own colours."),
                    colourMode, QStringLiteral("theme dark light palette automatic os"));
    auto *customButton = new QPushButton(QStringLiteral("Edit…"));
    colours->addRow(QStringLiteral("Custom colours"),
                    QStringLiteral("Window, panel, text, and accent colours for the Custom mode."),
                    customButton, QStringLiteral("accent palette"));
    connect(colourMode, &QComboBox::activated, this, [mainWindow, colourMode] {
        const auto key = colourMode->currentData().toString();
        if (key == QStringLiteral("custom"))
            mainWindow->editCustomColors();
        else
            mainWindow->setColorTheme(key);
        colourMode->setCurrentIndex(qMax(0, colourMode->findData(mainWindow->colorTheme())));
    });
    connect(customButton, &QPushButton::clicked, this, [mainWindow, colourMode] {
        mainWindow->editCustomColors();
        colourMode->setCurrentIndex(qMax(0, colourMode->findData(mainWindow->colorTheme())));
    });

    auto *style = new QComboBox;
    style->addItem(QStringLiteral("System default"), QStringLiteral("system"));
    for (const auto &key : mainWindow->availableUiStyles())
        style->addItem(mainWindow->uiStyleLabel(key), key);
    style->setCurrentIndex(qMax(0, style->findData(mainWindow->uiStyle(), Qt::UserRole,
                                                   Qt::MatchFixedString)));
    colours->addRow(QStringLiteral("Style"),
                    QStringLiteral("The look of buttons, menus, and other controls."), style,
                    QStringLiteral("ui feel widget style fusion"));
    connect(style, &QComboBox::activated, this, [mainWindow, style] {
        mainWindow->setUiStyle(style->currentData().toString());
    });
    auto *windowBorder = new ToggleSwitch;
    windowBorder->setChecked(mainWindow->windowBorders());
    colours->addRow(QStringLiteral("Window border"),
                         QStringLiteral("A thin outline around every window, so overlapping "
                                        "windows of the same colour stand apart."),
                         windowBorder, QStringLiteral("frame edge outline shadow separate"));
    connect(windowBorder, &QCheckBox::toggled, this,
            [mainWindow](bool enabled) { mainWindow->setWindowBorders(enabled); });


    // Icons and text
    auto *iconsAndText = page->addCard(QStringLiteral("Icons and text"));
    auto *iconTheme = new QComboBox;
    iconTheme->addItem(QStringLiteral("Built-in"), QString());
    for (const auto &theme : availableIconThemes())
        iconTheme->addItem(theme.second, theme.first);
    iconTheme->setCurrentIndex(qMax(0, iconTheme->findData(mainWindow->iconThemeChoice())));
    iconsAndText->addRow(QStringLiteral("Icon theme"),
                         QStringLiteral("Icons for the toolbar, menus, session tree, and SFTP file "
                                        "types."),
                         iconTheme, QStringLiteral("icons papirus breeze adwaita"));
    connect(iconTheme, &QComboBox::activated, this, [mainWindow, iconTheme] {
        mainWindow->setIconThemeChoice(iconTheme->currentData().toString());
    });

    auto *fontControls = new QWidget;
    auto *fontLayout = new QHBoxLayout(fontControls);
    fontLayout->setContentsMargins(0, 0, 0, 0);
    auto *useSystemFont = new ToggleSwitch(QStringLiteral("System"), fontControls);
    auto *fontFamily = new QFontComboBox(fontControls);
    fontFamily->setMaximumWidth(200);
    auto *fontSize = new QSpinBox(fontControls);
    fontSize->setRange(6, 48);
    fontSize->setSuffix(QStringLiteral(" pt"));
    const auto family = mainWindow->interfaceFontFamily();
    const int size = mainWindow->interfaceFontSize();
    useSystemFont->setChecked(family.isEmpty() && size == 0);
    fontFamily->setCurrentFont(QFont(family.isEmpty() ? mainWindow->systemFont().family() : family));
    fontSize->setValue(size > 0 ? size : mainWindow->systemFont().pointSize());
    fontLayout->addWidget(useSystemFont);
    fontLayout->addWidget(fontFamily);
    fontLayout->addWidget(fontSize);
    iconsAndText->addRow(QStringLiteral("Interface font"),
                         QStringLiteral("Menus, panels, and dialogs. Terminals have their own "
                                        "font on the Terminal tab."),
                         fontControls, QStringLiteral("text size font family"));
    auto applyFont = [mainWindow, useSystemFont, fontFamily, fontSize] {
        fontFamily->setEnabled(!useSystemFont->isChecked());
        fontSize->setEnabled(!useSystemFont->isChecked());
        if (useSystemFont->isChecked())
            mainWindow->setInterfaceFont(QString(), 0);
        else
            mainWindow->setInterfaceFont(fontFamily->currentFont().family(), fontSize->value());
    };
    fontFamily->setEnabled(!useSystemFont->isChecked());
    fontSize->setEnabled(!useSystemFont->isChecked());
    connect(useSystemFont, &QCheckBox::toggled, this, applyFont);
    connect(fontFamily, &QFontComboBox::currentFontChanged, this, applyFont);
    connect(fontSize, &QSpinBox::valueChanged, this, applyFont);

    // Terminal
    page = addPage(QStringLiteral("appearance-terminal"), QStringLiteral("Terminal"),
                   QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    auto *terminal = page->addCard(QStringLiteral("Terminal"));
    auto *editor = new TerminalAppearanceEditor(mainWindow->defaultTerminalAppearance(), false);
    editor->changed = [mainWindow, editor] {
        mainWindow->setDefaultTerminalAppearance(editor->value());
    };
    terminal->addWideRow(editor, QStringLiteral("terminal colour scheme font size preview "
                                                "import"));
    auto *darkScheme = new QComboBox;
    darkScheme->addItem(QStringLiteral("Same as above"), QString());
    for (const auto &scheme : availableColorSchemes())
        darkScheme->addItem(scheme, scheme);
    darkScheme->setCurrentIndex(qMax(0, darkScheme->findData(mainWindow->darkTerminalColorScheme())));
    terminal->addRow(QStringLiteral("Colour scheme in dark mode"),
                     QStringLiteral("Used while the interface is dark; with the Automatic colour "
                                    "mode it switches with your operating system."),
                     darkScheme, QStringLiteral("terminal dark night automatic"));
    connect(darkScheme, &QComboBox::activated, this, [mainWindow, darkScheme] {
        mainWindow->setDarkTerminalColorScheme(darkScheme->currentData().toString());
    });
}

void PreferencesWindow::buildTerminalPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("terminal"), QStringLiteral("Behaviour"),
                         QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    // Every control edits one field and applies it to all open terminals.
    auto update = [mainWindow](auto change) {
        auto behaviour = mainWindow->terminalBehaviour();
        change(behaviour);
        mainWindow->setTerminalBehaviour(behaviour);
    };
    auto connectChoice = [this, update](QComboBox *box, QString TerminalBehaviour::*field) {
        connect(box, &QComboBox::activated, this, [update, box, field] {
            update([&](TerminalBehaviour &b) { b.*field = box->currentData().toString(); });
        });
    };
    auto connectToggle = [this, update](QCheckBox *box, bool TerminalBehaviour::*field) {
        connect(box, &QCheckBox::toggled, this, [update, field](bool on) {
            update([&](TerminalBehaviour &b) { b.*field = on; });
        });
    };
    const auto current = mainWindow->terminalBehaviour();

    auto *display = page->addCard(QStringLiteral("Cursor and display"));
    auto *cursor = choiceBox({{QStringLiteral("block"), QStringLiteral("Block")},
                              {QStringLiteral("underline"), QStringLiteral("Underline")},
                              {QStringLiteral("ibeam"), QStringLiteral("Bar")}},
                             current.cursorShape);
    display->addRow(QStringLiteral("Cursor shape"), QString(), cursor, QStringLiteral("caret"));
    connectChoice(cursor, &TerminalBehaviour::cursorShape);
    auto *blink = toggle(current.cursorBlinks);
    display->addRow(QStringLiteral("Blinking cursor"), QString(), blink, QStringLiteral("caret"));
    connectToggle(blink, &TerminalBehaviour::cursorBlinks);
    auto *scrollBar = choiceBox({{QStringLiteral("right"), QStringLiteral("Right")},
                                 {QStringLiteral("left"), QStringLiteral("Left")},
                                 {QStringLiteral("hidden"), QStringLiteral("Hidden")}},
                                current.scrollBar);
    display->addRow(QStringLiteral("Scroll bar"), QString(), scrollBar);
    connectChoice(scrollBar, &TerminalBehaviour::scrollBar);
    auto *padding = new QSpinBox;
    padding->setRange(0, 32);
    padding->setSuffix(QStringLiteral(" px"));
    padding->setValue(current.padding);
    display->addRow(QStringLiteral("Padding"), QStringLiteral("Space between the text and the "
                                                              "terminal's edges."),
                    padding, QStringLiteral("margin border"));
    connect(padding, &QSpinBox::valueChanged, this, [update](int value) {
        update([&](TerminalBehaviour &b) { b.padding = value; });
    });

    auto *scrollback = page->addCard(QStringLiteral("Scrollback"));
    auto *lines = new QSpinBox;
    lines->setRange(0, 1000000);
    lines->setSingleStep(1000);
    lines->setSpecialValueText(QStringLiteral("Unlimited"));
    lines->setSuffix(QStringLiteral(" lines"));
    lines->setValue(current.scrollbackLines);
    scrollback->addRow(QStringLiteral("Lines kept"),
                       QStringLiteral("How much output you can scroll back to. 0 keeps "
                                      "everything (memory use grows with output)."),
                       lines, QStringLiteral("history buffer"));
    connect(lines, &QSpinBox::valueChanged, this, [update](int value) {
        update([&](TerminalBehaviour &b) { b.scrollbackLines = value; });
    });

    auto *mouse = page->addCard(QStringLiteral("Mouse, copy, and paste"));
    auto *copyOnSelect = toggle(current.copyOnSelect);
    mouse->addRow(QStringLiteral("Copy on select"),
                  QStringLiteral("Selected text goes to the clipboard at once, like PuTTY."),
                  copyOnSelect, QStringLiteral("clipboard selection"));
    connectToggle(copyOnSelect, &TerminalBehaviour::copyOnSelect);
    auto *rightClick = choiceBox({{QStringLiteral("menu"), QStringLiteral("Show menu")},
                                  {QStringLiteral("paste"), QStringLiteral("Paste")}},
                                 current.rightClick);
    mouse->addRow(QStringLiteral("Right-click"),
                  QStringLiteral("With Paste, Shift+right-click still opens the menu."),
                  rightClick, QStringLiteral("context menu mouse button putty mobaxterm"));
    connectChoice(rightClick, &TerminalBehaviour::rightClick);
    auto *confirmPaste = toggle(current.confirmMultilinePaste);
    mouse->addRow(QStringLiteral("Confirm multi-line paste"),
                  QStringLiteral("Asks first, so a pasted script does not run line by line by "
                                 "accident."),
                  confirmPaste, QStringLiteral("safety clipboard"));
    connectToggle(confirmPaste, &TerminalBehaviour::confirmMultilinePaste);
    auto *trim = toggle(current.trimPastedNewlines);
    mouse->addRow(QStringLiteral("Remove trailing newlines when pasting"),
                  QStringLiteral("A pasted command waits for Enter instead of running."), trim,
                  QStringLiteral("clipboard"));
    connectToggle(trim, &TerminalBehaviour::trimPastedNewlines);
    auto *wordChars = new QLineEdit(current.wordCharacters);
    wordChars->setMaximumWidth(200);
    mouse->addRow(QStringLiteral("Word characters"),
                  QStringLiteral("Characters that double-click selects as part of a word, besides "
                                 "letters and digits; for example paths and URLs."),
                  wordChars, QStringLiteral("double click selection"));
    connect(wordChars, &QLineEdit::editingFinished, this, [update, wordChars] {
        update([&](TerminalBehaviour &b) { b.wordCharacters = wordChars->text(); });
    });
    auto *links = toggle(current.ctrlClickOpensLinks);
    mouse->addRow(QStringLiteral("Ctrl+click opens links"),
                  QStringLiteral("Web addresses in the output open in your browser."), links,
                  QStringLiteral("url hyperlink browser"));
    connectToggle(links, &TerminalBehaviour::ctrlClickOpensLinks);

    auto *notifications = page->addCard(QStringLiteral("Notifications"));
    auto *bell = choiceBox({{QStringLiteral("visual"), QStringLiteral("Flash")},
                            {QStringLiteral("sound"), QStringLiteral("Sound")},
                            {QStringLiteral("none"), QStringLiteral("Off")}},
                           current.bell);
    notifications->addRow(QStringLiteral("Terminal bell"),
                          QStringLiteral("What happens when a program rings the bell."), bell,
                          QStringLiteral("beep alert"));
    connectChoice(bell, &TerminalBehaviour::bell);
    auto *activity = toggle(current.markActivity);
    notifications->addRow(QStringLiteral("Highlight tabs with new output"),
                          QStringLiteral("A background tab's title turns the accent colour when "
                                         "it prints something, for example when a long job ends."),
                          activity, QStringLiteral("activity monitor tab"));
    connectToggle(activity, &TerminalBehaviour::markActivity);
}

void PreferencesWindow::buildStartupPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("startup"), QStringLiteral("Startup and tabs"),
                         QStringLiteral("system-run"), QStyle::SP_MediaPlay);
    auto update = [mainWindow](auto change) {
        auto preferences = mainWindow->startupPreferences();
        change(preferences);
        mainWindow->setStartupPreferences(preferences);
    };
    auto connectToggle = [this, update](QCheckBox *box, bool StartupPreferences::*field) {
        connect(box, &QCheckBox::toggled, this, [update, field](bool on) {
            update([&](StartupPreferences &p) { p.*field = on; });
        });
    };
    const auto current = mainWindow->startupPreferences();

    auto *startup = page->addCard(QStringLiteral("When OpsRemote starts"));
    QList<QPair<QString, QString>> startChoices{
        {QStringLiteral("nothing"), QStringLiteral("Show the session list")},
        {QStringLiteral("restore"), QStringLiteral("Reopen the last tabs")}};
#ifndef Q_OS_WIN
    startChoices.insert(1, {QStringLiteral("local"), QStringLiteral("Open a local terminal")});
#endif
    auto *onStartup = choiceBox(startChoices, current.onStartup);
    startup->addRow(QStringLiteral("Open"),
                    QStringLiteral("Reopening asks for the vault password first when sessions "
                                   "need it."),
                    onStartup, QStringLiteral("restore session launch"));
    connect(onStartup, &QComboBox::activated, this, [update, onStartup] {
        update([&](StartupPreferences &p) { p.onStartup = onStartup->currentData().toString(); });
    });
    auto *remember = toggle(current.rememberWindow);
    startup->addRow(QStringLiteral("Remember window size and position"), QString(), remember,
                    QStringLiteral("geometry maximized"));
    connectToggle(remember, &StartupPreferences::rememberWindow);
    auto *single = toggle(current.singleInstance);
    startup->addRow(QStringLiteral("Run only one copy of OpsRemote"),
#ifdef Q_OS_LINUX
                    QStringLiteral("Starting it again brings the open window to the front (on Wayland "
                                   "only with the next setting on)."),
#else
                    QStringLiteral("Starting it again brings the open window to the front."),
#endif
                    single, QStringLiteral("instance single multiple second window"));
    connectToggle(single, &StartupPreferences::singleInstance);
#ifdef Q_OS_LINUX
    auto *activation = toggle(current.waylandActivation);
    startup->addRow(QStringLiteral("Bring windows to the front (Wayland)"),
                    QStringLiteral("Lets OpsRemote raise its windows, such as the open window when it "
                                   "is started again. On GNOME each time shows the busy pointer for "
                                   "several seconds, after most menus and dialogs. Applies after a "
                                   "restart."),
                    activation, QStringLiteral("activation raise focus busy cursor pointer wayland gnome"));
    connectToggle(activation, &StartupPreferences::waylandActivation);
#endif

    auto *tabs = page->addCard(QStringLiteral("Tabs"));
    auto *title = choiceBox({{QStringLiteral("name"), QStringLiteral("Session name")},
                             {QStringLiteral("userhost"), QStringLiteral("user@host")}},
                            current.tabTitle);
    tabs->addRow(QStringLiteral("Tab title"), QStringLiteral("Applies to newly opened tabs."),
                 title, QStringLiteral("label name"));
    connect(title, &QComboBox::activated, this, [update, title] {
        update([&](StartupPreferences &p) { p.tabTitle = title->currentData().toString(); });
    });
    auto *nextTo = toggle(current.newTabNextToCurrent);
    tabs->addRow(QStringLiteral("Open new tabs next to the current one"),
                 QStringLiteral("Otherwise new tabs open at the end."), nextTo,
                 QStringLiteral("position order"));
    connectToggle(nextTo, &StartupPreferences::newTabNextToCurrent);
    auto *middle = toggle(current.middleClickCloses);
    tabs->addRow(QStringLiteral("Middle-click closes a tab"), QString(), middle,
                 QStringLiteral("mouse wheel button"));
    connectToggle(middle, &StartupPreferences::middleClickCloses);

    auto *confirm = page->addCard(QStringLiteral("Confirmations"));
    auto *closeConnected = toggle(current.confirmCloseConnected);
    confirm->addRow(QStringLiteral("Ask before closing a connected SSH tab"), QString(),
                    closeConnected, QStringLiteral("confirm disconnect"));
    connectToggle(closeConnected, &StartupPreferences::confirmCloseConnected);
    auto *exit = toggle(current.confirmExit);
    confirm->addRow(QStringLiteral("Ask before exiting with open terminals"), QString(), exit,
                    QStringLiteral("confirm quit close"));
    connectToggle(exit, &StartupPreferences::confirmExit);
}

void PreferencesWindow::buildShortcutsPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("shortcuts"), QStringLiteral("Keyboard shortcuts"),
                         QStringLiteral("preferences-desktop-keyboard"), QStyle::SP_DialogHelpButton);

    auto *general = page->addCard();
    auto *conflicts = secondaryLabel(QString(), nullptr);
    conflicts->setStyleSheet(QStringLiteral("color: palette(highlight); font-weight: bold;"));
    auto *resetAll = new QPushButton(QStringLiteral("Reset all"));
    general->addRow(QStringLiteral("Shortcuts"),
                    QStringLiteral("Click a shortcut and press the new keys. Terminal shortcuts "
                                   "use Shift so that Ctrl+C and the like still reach the remote "
                                   "program."),
                    resetAll, QStringLiteral("keys keyboard bindings hotkeys"));
    general->addWideRow(conflicts, QStringLiteral("keys keyboard bindings hotkeys conflict"));
    conflicts->hide();

    QHash<QString, QKeySequenceEdit *> editors;
    QMap<QString, SettingsCard *> cards;
    for (const auto &definition : shortcutDefinitions()) {
        auto *&card = cards[definition.category];
        if (!card)
            card = page->addCard(definition.category);
        auto *controls = new QWidget;
        auto *layout = new QHBoxLayout(controls);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *editor = new QKeySequenceEdit(mainWindow->shortcuts().value(definition.id), controls);
        editor->setMaximumSequenceLength(1);
        editor->setClearButtonEnabled(true);
        editor->setMinimumWidth(170);
        auto *reset = new QToolButton(controls);
        reset->setIcon(themedIcon(QStringLiteral("edit-undo"), QStyle::SP_BrowserReload));
        reset->setToolTip(QStringLiteral("Default: %1").arg(
            definition.defaultKey.toString(QKeySequence::NativeText)));
        layout->addWidget(editor);
        layout->addWidget(reset);
        card->addRow(definition.label, QString(), controls,
                     definition.category + QStringLiteral(" shortcut keys ")
                         + definition.defaultKey.toString(QKeySequence::NativeText));
        editors.insert(definition.id, editor);
        connect(reset, &QToolButton::clicked, editor, [editor, key = definition.defaultKey] {
            editor->setKeySequence(key);
            emit editor->editingFinished();
        });
    }

    // Saves every editor and lists keys used twice.
    auto apply = [mainWindow, editors, conflicts] {
        QHash<QString, QKeySequence> shortcuts;
        QHash<QKeySequence, QStringList> users;
        for (const auto &definition : shortcutDefinitions()) {
            const auto key = editors.value(definition.id)->keySequence();
            shortcuts.insert(definition.id, key);
            if (!key.isEmpty())
                users[key].append(definition.label);
        }
        QStringList clashes;
        for (auto it = users.cbegin(); it != users.cend(); ++it) {
            if (it.value().size() > 1)
                clashes.append(QStringLiteral("%1 is used by %2")
                                   .arg(it.key().toString(QKeySequence::NativeText),
                                        it.value().join(QStringLiteral(" and "))));
        }
        conflicts->setText(clashes.join(QLatin1Char('\n')));
        conflicts->setVisible(!clashes.isEmpty());
        mainWindow->setShortcuts(shortcuts);
    };
    for (auto *editor : std::as_const(editors)) {
        connect(editor, &QKeySequenceEdit::editingFinished, this, apply);
        connect(editor, &QKeySequenceEdit::keySequenceChanged, this, [editor, apply] {
            if (editor->keySequence().isEmpty())
                apply();
        });
    }
    connect(resetAll, &QPushButton::clicked, this, [editors, apply] {
        for (const auto &definition : shortcutDefinitions())
            editors.value(definition.id)->setKeySequence(definition.defaultKey);
        apply();
    });
}

// Edits one preferences struct of MainWindow: get, change a copy, set.
#define PREFERENCE_UPDATER(Type, getter, setter)                                    \
    [mainWindow = m_mainWindow](auto change) {                                      \
        Type value = mainWindow->getter();                                          \
        change(value);                                                              \
        mainWindow->setter(value);                                                  \
    }

namespace {
// Connects common controls to one field through an updater.
template <typename Type, typename Updater>
void bindToggle(QObject *context, QCheckBox *box, Updater update, bool Type::*field)
{
    QObject::connect(box, &QCheckBox::toggled, context,
                     [update, field](bool on) { update([&](Type &v) { v.*field = on; }); });
}

template <typename Type, typename Updater>
void bindSpin(QObject *context, QSpinBox *box, Updater update, int Type::*field)
{
    QObject::connect(box, &QSpinBox::valueChanged, context,
                     [update, field](int value) { update([&](Type &v) { v.*field = value; }); });
}

template <typename Type, typename Updater>
void bindChoice(QObject *context, QComboBox *box, Updater update, QString Type::*field)
{
    QObject::connect(box, &QComboBox::activated, context, [update, field, box] {
        update([&](Type &v) { v.*field = box->currentData().toString(); });
    });
}

QSpinBox *spinBox(int minimum, int maximum, int value, const QString &suffix,
                  const QString &zeroText = QString())
{
    auto *box = new QSpinBox;
    box->setRange(minimum, maximum);
    box->setSuffix(suffix);
    if (!zeroText.isEmpty())
        box->setSpecialValueText(zeroText);
    box->setValue(value);
    return box;
}

// A path field with a Browse… button; folder or file.
QWidget *pathPicker(const QString &value, const QString &placeholder, bool folder,
                    std::function<void(const QString &)> changed)
{
    auto *widget = new QWidget;
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *edit = new QLineEdit(QDir::toNativeSeparators(value), widget);
    edit->setPlaceholderText(placeholder);
    edit->setClearButtonEnabled(true);
    edit->setMinimumWidth(220);
    auto *browse = new QPushButton(QStringLiteral("Browse…"), widget);
    layout->addWidget(edit);
    layout->addWidget(browse);
    QObject::connect(edit, &QLineEdit::editingFinished, widget,
                     [edit, changed] { changed(QDir::fromNativeSeparators(edit->text().trimmed())); });
    QObject::connect(browse, &QPushButton::clicked, widget, [widget, edit, folder, changed] {
        const auto start = edit->text().isEmpty() ? QDir::homePath() : edit->text();
        const auto chosen = folder ? QFileDialog::getExistingDirectory(widget, QString(), start)
                                   : QFileDialog::getOpenFileName(widget, QString(), start);
        if (chosen.isEmpty())
            return;
        edit->setText(QDir::toNativeSeparators(chosen));
        changed(chosen);
    });
    return widget;
}

// Trusted host keys in settings/known_hosts: host, key type, and the line.
struct KnownHost
{
    QString hosts;
    QString keyType;
    QString line;
};

QString knownHostsPath()
{
    return portableFilePath(QStringLiteral("settings/known_hosts"));
}

QList<KnownHost> readKnownHosts()
{
    QList<KnownHost> hosts;
    QFile file(knownHostsPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return hosts;
    while (!file.atEnd()) {
        const auto line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const auto fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() < 3)
            continue;
        const bool hashed = fields[0].startsWith(QStringLiteral("|1|"));
        hosts.append({hashed ? QStringLiteral("(hashed host name)") : fields[0], fields[1], line});
    }
    return hosts;
}

bool writeKnownHosts(const QList<KnownHost> &hosts)
{
    QSaveFile file(knownHostsPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    for (const auto &host : hosts)
        file.write(host.line.toUtf8() + '\n');
    return file.commit();
}

QTableWidget *editorTable(const QStringList &headers)
{
    auto *table = new QTableWidget(0, headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setMinimumHeight(220);
    return table;
}
}

void PreferencesWindow::buildSshPage()
{
    auto *page = addPage(QStringLiteral("ssh"), QStringLiteral("SSH"),
                         QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    auto update = PREFERENCE_UPDATER(SshPreferences, sshPreferences, setSshPreferences);
    const auto current = m_mainWindow->sshPreferences();

    auto *connection = page->addCard(QStringLiteral("Connecting"));
    auto *timeout = spinBox(3, 300, current.connectTimeout, QStringLiteral(" s"));
    connection->addRow(QStringLiteral("Connection timeout"),
                       QStringLiteral("How long to wait for a server before giving up."), timeout);
    bindSpin(this, timeout, update, &SshPreferences::connectTimeout);
    auto *keepalive = spinBox(0, 3600, current.keepaliveSeconds, QStringLiteral(" s"),
                              QStringLiteral("Off"));
    connection->addRow(QStringLiteral("Keepalive interval"),
                       QStringLiteral("Sends a small packet while idle, so routers and firewalls "
                                      "do not drop the connection."),
                       keepalive, QStringLiteral("idle timeout nat"));
    bindSpin(this, keepalive, update, &SshPreferences::keepaliveSeconds);
    auto *compression = toggle(current.compression);
    connection->addRow(QStringLiteral("Compression"),
                       QStringLiteral("Helps on slow links; costs a little CPU on fast ones."),
                       compression, QStringLiteral("zlib slow"));
    bindToggle(this, compression, update, &SshPreferences::compression);
    auto *terminalType = choiceBox({{QStringLiteral("xterm-256color"), QStringLiteral("xterm-256color")},
                                    {QStringLiteral("xterm"), QStringLiteral("xterm")},
                                    {QStringLiteral("linux"), QStringLiteral("linux")},
                                    {QStringLiteral("vt100"), QStringLiteral("vt100")}},
                                   current.terminalType);
    connection->addRow(QStringLiteral("Terminal type"),
                       QStringLiteral("Reported to the server as TERM. Try xterm on very old "
                                      "systems."),
                       terminalType, QStringLiteral("TERM emulation"));
    bindChoice(this, terminalType, update, &SshPreferences::terminalType);

    auto *reconnect = page->addCard(QStringLiteral("Dropped connections"));
    auto *autoReconnect = toggle(current.autoReconnect);
    reconnect->addRow(QStringLiteral("Reconnect automatically"),
                      QStringLiteral("When a connection drops (not when you log out)."),
                      autoReconnect, QStringLiteral("retry"));
    bindToggle(this, autoReconnect, update, &SshPreferences::autoReconnect);
    auto *delay = spinBox(1, 600, current.reconnectDelay, QStringLiteral(" s"));
    reconnect->addRow(QStringLiteral("Wait before reconnecting"), QString(), delay,
                      QStringLiteral("retry delay"));
    bindSpin(this, delay, update, &SshPreferences::reconnectDelay);
    auto *attempts = spinBox(1, 100, current.reconnectAttempts, QString());
    reconnect->addRow(QStringLiteral("Attempts"), QString(), attempts, QStringLiteral("retry"));
    bindSpin(this, attempts, update, &SshPreferences::reconnectAttempts);

    auto *browser = page->addCard(QStringLiteral("File browser"));
    auto *follow = toggle(current.followTerminalFolder);
    browser->addRow(QStringLiteral("Follow the terminal's folder"),
                    QStringLiteral("After each command, the Files pane shows the folder the "
                                   "remote shell is in. Sessions can turn it on or off, and the "
                                   "right-click menus of a terminal and its Files pane switch it "
                                   "for that tab only."),
                    follow, QStringLiteral("sftp cd directory cwd sync follow"));
    bindToggle(this, follow, update, &SshPreferences::followTerminalFolder);
    auto *fileTransfer = choiceBox(sshFileTransferChoices(), current.fileTransfer);
    browser->addRow(QStringLiteral("File transfer"),
                    QStringLiteral("How the file browser reaches a server's files. SCP suits devices "
                                   "without SFTP (routers, switches, appliances). Sessions can "
                                   "choose their own."),
                    fileTransfer, QStringLiteral("sftp scp protocol files router switch"));
    connect(fileTransfer, &QComboBox::activated, this, [update, fileTransfer] {
        update([&](SshPreferences &preferences) {
            preferences.fileTransfer = fileTransfer->currentData().toString();
        });
    });

    auto *keys = page->addCard(QStringLiteral("Keys"));
    auto *publicKeys = toggle(current.tryPublicKeys);
    keys->addRow(QStringLiteral("Use my SSH keys and agent"),
                 QStringLiteral("Tries the keys in your .ssh folder and the SSH agent before "
                                "asking for a password."),
                 publicKeys, QStringLiteral("public key authentication agent id_ed25519 id_rsa"));
    bindToggle(this, publicKeys, update, &SshPreferences::tryPublicKeys);
    auto *forwardAgent = toggle(current.forwardAgent);
    keys->addRow(QStringLiteral("Forward my SSH agent"),
                 QStringLiteral("Servers can use your local keys to log on further (ssh -A). "
                                "Only for servers you trust; sessions can turn it on or off."),
                 forwardAgent, QStringLiteral("agent forwarding -A"));
    bindToggle(this, forwardAgent, update, &SshPreferences::forwardAgent);
    auto *forwardX11 = toggle(current.forwardX11);
    keys->addRow(QStringLiteral("Forward X11"),
                 QStringLiteral("Graphical programs started on servers show on this computer "
                                "(ssh -Y). Needs a local X server; on Windows VcXsrv or Xming. "
                                "Only for servers you trust; sessions can turn it on or off."),
                 forwardX11, QStringLiteral("x11 xorg display gui -X -Y vcxsrv xming"));
    bindToggle(this, forwardX11, update, &SshPreferences::forwardX11);
    keys->addWideRow(new AvailabilityNote(&LocalResources::x11Problem, [forwardX11](bool available) {
        forwardX11->setEnabled(available || forwardX11->isChecked());
    }), QStringLiteral("x11 xorg display"));
#ifdef Q_OS_WIN
    auto *agent = choiceBox({{QStringLiteral("auto"), QStringLiteral("Automatic")},
                             {QStringLiteral("openssh"), QStringLiteral("OpenSSH agent")},
                             {QStringLiteral("pageant"), QStringLiteral("Pageant (PuTTY)")}},
                            current.agent);
    keys->addRow(QStringLiteral("SSH agent"),
                 QStringLiteral("Which agent forwarded requests go to. Automatic uses the Windows "
                                "OpenSSH agent when it runs, otherwise Pageant."),
                 agent, QStringLiteral("pageant putty agent openssh"));
    bindChoice(this, agent, update, &SshPreferences::agent);
#endif
    keys->addRow(QStringLiteral("Extra private key"),
                 QStringLiteral("Tried first. Keep it next to the program (for example in "
                                "settings/) to carry it with you. Keys with a passphrase are not "
                                "supported yet."),
                 pathPicker(current.identityFile, QStringLiteral("None"), false,
                            [update](const QString &path) {
                                update([&](SshPreferences &v) { v.identityFile = path; });
                            }),
                 QStringLiteral("identity file key pem"));

    // Known hosts manager
    auto *knownHosts = page->addCard(QStringLiteral("Trusted host keys"));
    auto *table = editorTable({QStringLiteral("Host"), QStringLiteral("Key type")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto reload = [table] {
        const auto hosts = readKnownHosts();
        table->setRowCount(0);
        for (const auto &host : hosts) {
            const int row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(host.hosts));
            table->setItem(row, 1, new QTableWidgetItem(host.keyType));
            table->item(row, 0)->setData(Qt::UserRole, host.line);
            table->item(row, 0)->setToolTip(host.line.left(200));
        }
        table->resizeColumnToContents(0);
    };
    reload();
    auto *buttons = new QWidget;
    auto *buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    auto *remove = new QPushButton(QStringLiteral("Forget selected"), buttons);
    auto *refresh = new QPushButton(QStringLiteral("Refresh"), buttons);
    buttonLayout->addWidget(remove);
    buttonLayout->addWidget(refresh);
    knownHosts->addRow(QStringLiteral("Servers you trusted"),
                       QStringLiteral("Forget a server after it was reinstalled, so its new key is "
                                      "offered again instead of the \"key changed\" warning."),
                       buttons, QStringLiteral("known_hosts fingerprint host key"));
    knownHosts->addWideRow(table, QStringLiteral("known_hosts fingerprint host key trusted"));
    connect(refresh, &QPushButton::clicked, this, reload);
    connect(remove, &QPushButton::clicked, this, [this, table, reload] {
        QSet<QString> lines;
        for (const auto *item : table->selectedItems())
            lines.insert(table->item(item->row(), 0)->data(Qt::UserRole).toString());
        if (lines.isEmpty())
            return;
        if (QMessageBox::question(this, QStringLiteral("Forget Host Keys"),
                                  QStringLiteral("Forget %1 trusted host key(s)? You will be asked "
                                                 "to confirm these servers again.")
                                      .arg(lines.size()))
            != QMessageBox::Yes)
            return;
        auto hosts = readKnownHosts();
        hosts.removeIf([&](const KnownHost &host) { return lines.contains(host.line); });
        if (!writeKnownHosts(hosts))
            QMessageBox::warning(this, QStringLiteral("Forget Host Keys"),
                                 QStringLiteral("Could not write %1.")
                                     .arg(QDir::toNativeSeparators(knownHostsPath())));
        reload();
    });
}

// The proxy for every SSH connection, unless a folder or session sets one.
void PreferencesWindow::buildProxyPage()
{
    auto *page = addPage(QStringLiteral("proxy"), QStringLiteral("Proxy"),
                         QStringLiteral("network-wired"), QStyle::SP_DriveNetIcon);
    auto update = PREFERENCE_UPDATER(SshPreferences, sshPreferences, setSshPreferences);
    auto *card = page->addCard(QStringLiteral("Proxy for SSH connections"));
    auto *proxy = new ProxyEditor(m_mainWindow->sshPreferences().proxy, QString(), QString());
    card->addRow(QString(), QString(), proxy, QStringLiteral("proxy http socks corporate firewall"));
    connect(proxy, &ProxyEditor::changed, this, [update, proxy] {
        update([&](SshPreferences &preferences) { preferences.proxy = proxy->value(); });
    });
    connect(proxy, &ProxyEditor::passwordEntered, this, [this, proxy] {
        m_mainWindow->saveProxyPassword(proxy->value(), proxy->password());
    });
    card->addRow(QStringLiteral("Per folder and session"),
                 QStringLiteral("Folders and sessions can choose their own proxy, or none, on "
                                "their Proxy tab."),
                 nullptr, QStringLiteral("override"));
}

void PreferencesWindow::buildVncPage()
{
    auto *page = addPage(QStringLiteral("vnc"), QStringLiteral("VNC"),
                         QStringLiteral("preferences-desktop-remote-desktop"), QStyle::SP_DesktopIcon);
    const auto current = loadVncDefaults();
    auto update = [](auto change) {
        auto options = loadVncDefaults();
        change(options);
        saveVncDefaults(options);
    };
    // The same three sections as the session editor.
    auto *display = page->addCard(QStringLiteral("Display"));
    auto *resources = page->addCard(QStringLiteral("Local resources"));
    auto *connection = page->addCard(QStringLiteral("Connection"));
    auto addChoice = [this, update](auto *card, const QString &title, const QString &description,
                                    const RdpChoices &choices, const QString &value,
                                    const QString &keywords, auto apply) {
        auto *box = choiceBox(choices, value);
        card->addRow(title, description, box, keywords);
        connect(box, &QComboBox::activated, this, [box, update, apply] {
            const auto chosen = box->currentData().toString();
            update([&](VncOptions &options) { apply(options, chosen); });
        });
    };
    auto addSwitch = [this, update](auto *card, const QString &title, const QString &description,
                                    int value, const QString &keywords, auto apply) {
        auto *box = toggle(value == 1);
        card->addRow(title, description, box, keywords);
        connect(box, &QCheckBox::toggled, this, [update, apply](bool on) {
            update([&](VncOptions &options) { apply(options, on ? 1 : 0); });
        });
    };

    addChoice(display, QStringLiteral("Resolution"),
              QStringLiteral("Follow the tab size resizes the remote desktop with the window when "
                             "the server allows it (TigerVNC, recent x11vnc); otherwise it is "
                             "scaled to fit."),
              vncResolutionChoices(), current.resolution, QStringLiteral("size resize scale vnc"),
              [](VncOptions &o, const QString &v) { o.resolution = v; });
    addChoice(display, QStringLiteral("Colours"),
              QStringLiteral("Fewer colours need less bandwidth."), vncColorDepthChoices(),
              QString::number(current.colorDepth), QStringLiteral("colors depth bits vnc"),
              [](VncOptions &o, const QString &v) { o.colorDepth = v.toInt(); });
    addChoice(display, QStringLiteral("Quality"),
              QStringLiteral("Best sends the picture without loss; the others use JPEG and more "
                             "compression for slower links."),
              vncQualityChoices(), current.quality, QStringLiteral("performance jpeg compression vnc"),
              [](VncOptions &o, const QString &v) { o.quality = v; });
    addSwitch(display, QStringLiteral("Sharp on high-DPI screens"),
              QStringLiteral("One remote pixel per physical screen pixel, instead of an enlarged, "
                             "blurry desktop."),
              current.highDpi, QStringLiteral("hidpi scaling vnc"),
              [](VncOptions &o, int v) { o.highDpi = v; });
    addSwitch(display, QStringLiteral("Full screen uses all monitors"),
              QStringLiteral("In full screen (Ctrl+Alt+Enter), the window spans every monitor (not "
                             "on Wayland)."),
              current.allMonitors, QStringLiteral("multi monitor vnc"),
              [](VncOptions &o, int v) { o.allMonitors = v; });

    addSwitch(resources, QStringLiteral("Show the remote pointer shape"),
              QStringLiteral("Draws the server's pointer locally, so it moves without delay. Turn "
                             "off for servers that also draw it into the picture (two pointers)."),
              current.remoteCursor, QStringLiteral("cursor pointer mouse vnc"),
              [](VncOptions &o, int v) { o.remoteCursor = v; });
    addSwitch(resources, QStringLiteral("Share clipboard text"),
              QStringLiteral("Copy text here and paste it on the remote computer, and the other "
                             "way round."),
              current.clipboard, QStringLiteral("copy paste clipboard vnc"),
              [](VncOptions &o, int v) { o.clipboard = v; });

    addChoice(connection, QStringLiteral("Encryption"),
              QStringLiteral("Plain VNC is not encrypted. Require refuses servers without TLS; an "
                             "jump host encrypts any server."),
              vncEncryptionChoices(), current.encryption, QStringLiteral("tls vencrypt security vnc"),
              [](VncOptions &o, const QString &v) { o.encryption = v; });
    addSwitch(connection, QStringLiteral("Reconnect automatically"),
              QStringLiteral("If the network drops, tries again for about half a minute."),
              current.autoReconnect, QStringLiteral("reconnect network drop vnc"),
              [](VncOptions &o, int v) { o.autoReconnect = v; });
    connection->addRow(QStringLiteral("Per session"),
                       QStringLiteral("Every VNC session can override these in its own settings "
                                      "(Sessions → Edit…), and can also listen for "
                                      "a reverse connection or use an UltraVNC repeater."),
                       nullptr, QStringLiteral("override listen reverse repeater"));
}

void PreferencesWindow::buildRshPage()
{
    auto *page = addPage(QStringLiteral("rlogin"), QStringLiteral("Rlogin and RSH"),
                         QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    const auto current = loadRshDefaults();
    auto update = [](auto change) {
        auto options = loadRshDefaults();
        change(options);
        saveRshDefaults(options);
    };
    auto *card = page->addCard(QStringLiteral("Connecting"));
    auto *localUser = new QLineEdit(current.localUser);
    localUser->setPlaceholderText(currentUserName());
    card->addRow(QStringLiteral("Local user name"),
                 QStringLiteral("Sent as your name on this computer; the server's .rhosts or hosts.equiv "
                                "decide whether it lets you in without a password. Empty: this account's."),
                 localUser, QStringLiteral("rhosts hosts.equiv trust"));
    connect(localUser, &QLineEdit::editingFinished, this, [update, localUser] {
        update([&](RshOptions &o) { o.localUser = localUser->text().trimmed(); });
    });
    auto *sourcePort = choiceBox(rshSourcePortChoices(), current.sourcePort);
    card->addRow(QStringLiteral("Source port"),
                 QStringLiteral("Most servers accept only a reserved port (below 1024). Windows lets "
                                "anyone use one; Linux and macOS only an administrator."),
                 sourcePort, QStringLiteral("privileged reserved 1023"));
    connect(sourcePort, &QComboBox::activated, this, [update, sourcePort] {
        update([&](RshOptions &o) { o.sourcePort = sourcePort->currentData().toString(); });
    });
    auto *note = page->addCard(QStringLiteral("Security"));
    auto *text = new QLabel(QStringLiteral(
        "Rlogin and RSH send everything, passwords included, unencrypted, and trust the source "
        "address. Use them only on networks you trust, for equipment that has nothing better."));
    text->setWordWrap(true);
    note->addRow(QStringLiteral("Not encrypted"), QString(), text, QStringLiteral("insecure plain"));
}

void PreferencesWindow::buildXdmcpPage()
{
    auto *page = addPage(QStringLiteral("xdmcp"), QStringLiteral("XDMCP"),
                         QStringLiteral("video-display"), QStyle::SP_DesktopIcon);
    const auto current = loadXdmcpDefaults();
    auto update = [](auto change) {
        auto options = loadXdmcpDefaults();
        change(options);
        saveXdmcpDefaults(options);
    };
    auto *card = page->addCard(QStringLiteral("Display"));
    auto *mode = choiceBox(xdmcpModeChoices(), current.mode);
    card->addRow(QStringLiteral("Show"), QStringLiteral("What the X server asks the host for."), mode,
                 QStringLiteral("query indirect broadcast chooser"));
    connect(mode, &QComboBox::activated, this, [update, mode] {
        update([&](XdmcpOptions &o) { o.mode = mode->currentData().toString(); });
    });
    const bool customPath = !current.program.isEmpty()
                            && std::none_of(xdmcpProgramChoices().cbegin(), xdmcpProgramChoices().cend(),
                                            [&](const auto &choice) { return choice.first == current.program; });
    auto *program = choiceBox(xdmcpProgramChoices(), customPath ? QStringLiteral("custom") : current.program);
    auto *path = new QLineEdit(customPath ? current.program : QString());
    path->setPlaceholderText(QStringLiteral("Path of the X server program"));
    auto *programBox = new QWidget;
    auto *programLayout = new QVBoxLayout(programBox);
    programLayout->setContentsMargins(0, 0, 0, 0);
    programLayout->addWidget(program);
    programLayout->addWidget(path);
    auto *availability = new AvailabilityNote(
        [program, path] {
            QString choice = program->currentData().toString();
            if (choice == QStringLiteral("custom"))
                choice = path->text().trimmed().isEmpty() ? QStringLiteral("custom") : path->text().trimmed();
            return findXServer(choice).problem;
        },
        {});
    programLayout->addWidget(availability);
    card->addRow(QStringLiteral("X server program"),
                 QStringLiteral("XDMCP shows the login screen in an X server of this computer, in a "
                                "window of its own."),
                 programBox, QStringLiteral("xephyr xnest vcxsrv xming"));
    const auto storeProgram = [update, program, path, availability] {
        path->setVisible(program->currentData().toString() == QStringLiteral("custom"));
        QString value = program->currentData().toString();
        if (value == QStringLiteral("custom") && !path->text().trimmed().isEmpty())
            value = path->text().trimmed();
        update([&](XdmcpOptions &o) { o.program = value; });
        availability->check();
    };
    connect(program, &QComboBox::activated, this, storeProgram);
    connect(path, &QLineEdit::editingFinished, this, storeProgram);
    path->setVisible(program->currentData().toString() == QStringLiteral("custom"));
    availability->check();
    auto *size = choiceBox(xdmcpSizeChoices(), current.size);
    card->addRow(QStringLiteral("Size"), QString(), size, QStringLiteral("resolution geometry fullscreen"));
    connect(size, &QComboBox::activated, this, [update, size] {
        update([&](XdmcpOptions &o) { o.size = size->currentData().toString(); });
    });
}

void PreferencesWindow::buildHttpRequestPage()
{
    auto *page = addPage(QStringLiteral("http-request"), QStringLiteral("HTTP requests"),
                         QStringLiteral("internet-web-browser"), QStyle::SP_DriveNetIcon);
    const auto current = loadHttpRequestPreferences();
    auto update = [](auto change) {
        auto preferences = loadHttpRequestPreferences();
        change(preferences);
        saveHttpRequestPreferences(preferences);
    };
    auto *card = page->addCard(QStringLiteral("Network Tools → HTTP Request starts with"));
    auto *timeout = spinBox(1, 3600, current.timeoutSeconds, QStringLiteral(" s"));
    card->addRow(QStringLiteral("Time limit"), QStringLiteral("For the whole request, redirects included."), timeout,
                 QStringLiteral("timeout"));
    connect(timeout, &QSpinBox::valueChanged, this,
            [update](int value) { update([&](HttpRequestPreferences &p) { p.timeoutSeconds = value; }); });
    auto *verify = toggle(current.verifyCertificates);
    card->addRow(QStringLiteral("Check certificates"),
                 QStringLiteral("Off: answers come even from untrusted certificates; what is wrong is still shown."),
                 verify, QStringLiteral("tls ssl insecure"));
    connect(verify, &QCheckBox::toggled, this,
            [update](bool on) { update([&](HttpRequestPreferences &p) { p.verifyCertificates = on; }); });
    auto *follow = toggle(current.followRedirects);
    card->addRow(QStringLiteral("Follow redirects"), QString(), follow, QStringLiteral("location 301 302"));
    connect(follow, &QCheckBox::toggled, this,
            [update](bool on) { update([&](HttpRequestPreferences &p) { p.followRedirects = on; }); });
    auto *redirects = spinBox(0, 50, current.maxRedirects, QStringLiteral(" redirects"));
    card->addRow(QStringLiteral("At most"), QString(), redirects, QStringLiteral("redirect limit"));
    connect(redirects, &QSpinBox::valueChanged, this,
            [update](int value) { update([&](HttpRequestPreferences &p) { p.maxRedirects = value; }); });
    auto *agent = new QLineEdit(current.userAgent);
    card->addRow(QStringLiteral("User-Agent"), QStringLiteral("Sent unless the request sets its own."), agent,
                 QStringLiteral("browser identification"));
    connect(agent, &QLineEdit::editingFinished, this, [update, agent] {
        update([&](HttpRequestPreferences &p) { p.userAgent = agent->text().trimmed(); });
    });
    auto *size = spinBox(1, 1024, current.maxBodyMb, QStringLiteral(" MB"));
    card->addRow(QStringLiteral("Largest answer kept"),
                 QStringLiteral("Beyond this the body is cut (the tab says so)."), size, QStringLiteral("limit"));
    connect(size, &QSpinBox::valueChanged, this,
            [update](int value) { update([&](HttpRequestPreferences &p) { p.maxBodyMb = value; }); });
}

void PreferencesWindow::buildRunCommandPage()
{
    auto *page = addPage(QStringLiteral("run-command"), QStringLiteral("Run on hosts"),
                         QStringLiteral("system-run"), QStyle::SP_MediaPlay);
    const auto current = loadRunCommandPreferences();
    auto update = [](auto change) {
        auto preferences = loadRunCommandPreferences();
        change(preferences);
        saveRunCommandPreferences(preferences);
    };
    auto *defaults = page->addCard(QStringLiteral("Each Run on Hosts window starts with"));
    auto *parallel = spinBox(1, 50, current.parallel, QStringLiteral(" hosts"));
    defaults->addRow(QStringLiteral("At once"),
                     QStringLiteral("How many hosts run the command at the same time. Each window can "
                                    "change it for itself."),
                     parallel, QStringLiteral("parallel concurrent simultaneous"));
    connect(parallel, &QSpinBox::valueChanged, this,
            [update](int value) { update([&](RunCommandPreferences &p) { p.parallel = value; }); });
    auto *timeout = spinBox(1, 24 * 3600, current.timeoutSeconds, QStringLiteral(" s"));
    defaults->addRow(QStringLiteral("Time limit per host"),
                     QStringLiteral("A command still running after this is stopped on that host."),
                     timeout, QStringLiteral("timeout"));
    connect(timeout, &QSpinBox::valueChanged, this,
            [update](int value) { update([&](RunCommandPreferences &p) { p.timeoutSeconds = value; }); });
    auto *terminal = toggle(current.terminal);
    defaults->addRow(QStringLiteral("Use a terminal (pty)"),
                     QStringLiteral("For commands that need one. Output then comes as a terminal shows "
                                    "it, and a command waiting for input waits until the time limit."),
                     terminal, QStringLiteral("pty tty sudo"));
    connect(terminal, &QCheckBox::toggled, this,
            [update](bool on) { update([&](RunCommandPreferences &p) { p.terminal = on; }); });

    auto *kept = page->addCard(QStringLiteral("Results and history"));
    auto *output = spinBox(16, 64 * 1024, current.outputKb, QStringLiteral(" KB"));
    kept->addRow(QStringLiteral("Output kept per host"),
                 QStringLiteral("Beyond this, a host's output is read and left out."), output,
                 QStringLiteral("limit size"));
    connect(output, &QSpinBox::valueChanged, this,
            [update](int value) { update([&](RunCommandPreferences &p) { p.outputKb = value; }); });
    auto *recent = spinBox(0, 100, current.keepRecent, QStringLiteral(" commands"), QStringLiteral("None"));
    kept->addRow(QStringLiteral("Remember recent commands"),
                 QStringLiteral("Offered under Recent in the window. Commands are kept in settings.yaml "
                                "as typed, so do not put passwords in them."),
                 recent, QStringLiteral("history"));
    connect(recent, &QSpinBox::valueChanged, this, [update](int value) {
        update([&](RunCommandPreferences &p) {
            p.keepRecent = value;
            p.recent = p.recent.mid(0, value);
        });
    });
    auto *forget = new QPushButton(QStringLiteral("Forget Recent Commands"));
    kept->addRow(QStringLiteral("Recent commands"), QString(), forget, QStringLiteral("clear history"));
    connect(forget, &QPushButton::clicked, this,
            [update] { update([](RunCommandPreferences &p) { p.recent.clear(); }); });
}

void PreferencesWindow::buildFtpPage()
{
    auto *page = addPage(QStringLiteral("ftp"), QStringLiteral("FTP"),
                         QStringLiteral("folder-remote"), QStyle::SP_DriveNetIcon);
    const auto current = loadFtpDefaults();
    auto update = [](auto change) {
        auto options = loadFtpDefaults();
        change(options);
        saveFtpDefaults(options);
    };
    auto *connection = page->addCard(QStringLiteral("Connection"));
    auto addChoice = [this, update, connection](const QString &title, const QString &description,
                                                const RdpChoices &choices, const QString &value,
                                                const QString &keywords, auto apply) {
        auto *box = choiceBox(choices, value);
        connection->addRow(title, description, box, keywords);
        connect(box, &QComboBox::activated, this, [box, update, apply] {
            const auto chosen = box->currentData().toString();
            update([&](FtpOptions &options) { apply(options, chosen); });
        });
    };
    addChoice(QStringLiteral("Security"),
              QStringLiteral("Plain FTP sends passwords and files unencrypted. Use TLS when "
                             "offered works with every server; Require refuses servers without "
                             "it; implicit FTPS is the older kind, usually on port 990."),
              ftpSecurityChoices(), current.security, QStringLiteral("ftps tls ssl encryption"),
              [](FtpOptions &o, const QString &v) { o.security = v; });
    addChoice(QStringLiteral("Data connections"),
              QStringLiteral("Passive works through most firewalls and NAT. Active needs the "
                             "server to reach this computer; through a proxy, passive is used."),
              ftpModeChoices(), current.mode, QStringLiteral("pasv port active passive firewall"),
              [](FtpOptions &o, const QString &v) { o.mode = v; });
    addChoice(QStringLiteral("Folder listings"),
              QStringLiteral("MLSD gives exact sizes and times; LIST works with every server."),
              ftpListingChoices(), current.listing, QStringLiteral("mlsd list directory"),
              [](FtpOptions &o, const QString &v) { o.listing = v; });
    auto *timeout = spinBox(1, 600, current.connectTimeout, QStringLiteral(" s"));
    connection->addRow(QStringLiteral("Connect timeout"), QString(), timeout,
                       QStringLiteral("timeout wait"));
    connect(timeout, &QSpinBox::valueChanged, this, [update](int value) {
        update([&](FtpOptions &options) { options.connectTimeout = value; });
    });
    auto *keepalive = spinBox(0, 3600, current.keepaliveSeconds, QStringLiteral(" s"),
                              QStringLiteral("Off"));
    connection->addRow(QStringLiteral("Keepalive"),
                       QStringLiteral("Sends NOOP while idle, so servers do not close the connection."),
                       keepalive, QStringLiteral("noop idle timeout"));
    connect(keepalive, &QSpinBox::valueChanged, this, [update](int value) {
        update([&](FtpOptions &options) { options.keepaliveSeconds = value; });
    });
    connection->addRow(QStringLiteral("Per session"),
                       QStringLiteral("Every FTP session can override these in its own settings "
                                      "(Sessions → Edit…). The file browser's own settings are on "
                                      "the Files and transfers page; proxies on the Proxy page."),
                       nullptr, QStringLiteral("override"));
}

void PreferencesWindow::buildRdpPage()
{
    auto *page = addPage(QStringLiteral("rdp"), QStringLiteral("Remote desktop (RDP)"),
                         QStringLiteral("preferences-desktop-remote-desktop"),
                         QStyle::SP_DesktopIcon);
    const auto current = loadRdpDefaults();
    auto update = [](auto change) {
        auto options = loadRdpDefaults();
        change(options);
        saveRdpDefaults(options);
    };

    // The same three sections as the session editor; sessions override any
    // of these in their own settings.
    auto *display = page->addCard(QStringLiteral("Display"));
    auto *resources = page->addCard(QStringLiteral("Local resources"));
    auto *connection = page->addCard(QStringLiteral("Connection"));
    auto addChoice = [this, update](auto *card, const QString &title, const QString &description,
                                    const RdpChoices &choices, const QString &value,
                                    const QString &keywords, auto apply) {
        auto *box = choiceBox(choices, value);
        card->addRow(title, description, box, keywords);
        connect(box, &QComboBox::activated, this, [box, update, apply] {
            const auto chosen = box->currentData().toString();
            update([&](RdpOptions &options) { apply(options, chosen); });
        });
    };
    auto addSwitch = [this, update](auto *card, const QString &title, const QString &description,
                                    int value, const QString &keywords, auto apply) {
        auto *box = toggle(value == 1);
        card->addRow(title, description, box, keywords);
        connect(box, &QCheckBox::toggled, this, [update, apply](bool on) {
            update([&](RdpOptions &options) { apply(options, on ? 1 : 0); });
        });
    };
    auto addText = [this, update](auto *card, const QString &title, const QString &description,
                                  const QString &value, const QString &placeholder,
                                  const QString &keywords, auto apply) {
        auto *edit = new QLineEdit(value);
        edit->setPlaceholderText(placeholder);
        edit->setMaxLength(255);
        edit->setMinimumWidth(220);
        card->addRow(title, description, edit, keywords);
        connect(edit, &QLineEdit::editingFinished, this, [edit, update, apply] {
            const auto text = edit->text().trimmed();
            update([&](RdpOptions &options) { apply(options, text); });
        });
    };

    addChoice(display, QStringLiteral("Resolution"),
              QStringLiteral("Follow the tab size resizes the remote desktop with the window "
                             "(Windows 8.1 / Server 2012 R2 or newer). The other sizes are scaled "
                             "down to fit the tab."),
              rdpResolutionChoices(), current.resolution,
              QStringLiteral("size dynamic resize screen display"),
              [](RdpOptions &o, const QString &v) { o.resolution = v; });
    addChoice(display, QStringLiteral("Colour depth"), QString(), rdpColorDepthChoices(),
              QString::number(current.colorDepth), QStringLiteral("colors bits"),
              [](RdpOptions &o, const QString &v) { o.colorDepth = v.toInt(); });
    addChoice(display, QStringLiteral("Connection quality"),
              QStringLiteral("Lower quality turns off wallpaper, themes, animations, and font "
                             "smoothing on the remote side, which helps on slow links."),
              rdpQualityChoices(), current.quality,
              QStringLiteral("performance experience bandwidth wallpaper"),
              [](RdpOptions &o, const QString &v) { o.quality = v; });
    addSwitch(display, QStringLiteral("Modern graphics"),
              QStringLiteral("The RDP 8 graphics pipeline: smoother and lighter on the network, "
                             "with H.264 video coding on Windows. Turn off for old servers that "
                             "draw incorrectly. Needs 32-bit colour."),
              current.graphicsPipeline, QStringLiteral("gfx h264 avc remotefx codec performance"),
              [](RdpOptions &o, int v) { o.graphicsPipeline = v; });
    addSwitch(display, QStringLiteral("Sharp on high-DPI screens"),
              QStringLiteral("Uses the screen's full resolution and asks Windows to scale text and "
                             "icons to match, instead of enlarging a smaller, blurry desktop."),
              current.highDpi, QStringLiteral("hidpi retina scaling 4k zoom"),
              [](RdpOptions &o, int v) { o.highDpi = v; });
    addSwitch(display, QStringLiteral("Full screen uses all monitors"),
              QStringLiteral("In full screen (Ctrl+Alt+Enter), the remote desktop gets one monitor "
                             "per local monitor. Needs a server that allows several monitors; "
                             "not available on Wayland."),
              current.allMonitors, QStringLiteral("multi monitor multimon screens span"),
              [](RdpOptions &o, int v) { o.allMonitors = v; });

    addChoice(resources, QStringLiteral("Keyboard layout"),
              QStringLiteral("The remote session's layout. Keys are sent by position, so this "
                             "decides which characters they type there."),
              rdpKeyboardChoices(), current.keyboardLayout,
              QStringLiteral("keyboard language layout locale"),
              [](RdpOptions &o, const QString &v) { o.keyboardLayout = v; });
    addSwitch(resources, QStringLiteral("Share clipboard text"),
              QStringLiteral("Copy text here and paste it on the remote computer, and the other "
                             "way round."),
              current.clipboardText, QStringLiteral("copy paste clipboard text"),
              [](RdpOptions &o, int v) { o.clipboardText = v; });
    addSwitch(resources, QStringLiteral("Share files through the clipboard"),
              QStringLiteral("Copy files here (or drop them onto the remote screen) and paste "
                             "them on the remote computer with Ctrl+V. Files copied there can be "
                             "saved here from a bar in the tab."),
              current.clipboardFiles, QStringLiteral("copy paste files drag drop transfer"),
              [](RdpOptions &o, int v) { o.clipboardFiles = v; });

    addChoice(connection, QStringLiteral("Security"),
              QStringLiteral("Automatic picks the strongest the server offers. Legacy RDP "
                             "encryption is only for very old servers."),
              rdpSecurityChoices(), current.security,
              QStringLiteral("nla tls encryption credssp"),
              [](RdpOptions &o, const QString &v) { o.security = v; });
    addSwitch(connection, QStringLiteral("Ask for the password before connecting"),
              QStringLiteral("Off: when no password is saved, connect first and log on at the "
                             "remote computer's own screen (for Linux xrdp and older servers; "
                             "Windows usually requires NLA, which needs it up front)."),
              current.askCredentials, QStringLiteral("credentials login logon prompt password"),
              [](RdpOptions &o, int v) { o.askCredentials = v; });
    addSwitch(connection, QStringLiteral("Reconnect automatically"),
              QStringLiteral("If the network drops, tries again for about half a minute and "
                             "resumes the same remote session."),
              current.autoReconnect, QStringLiteral("reconnect network drop resume"),
              [](RdpOptions &o, int v) { o.autoReconnect = v; });
    addSwitch(connection, QStringLiteral("Administrator session"),
              QStringLiteral("Connects to the server's console session (mstsc /admin)."),
              current.adminSession, QStringLiteral("console admin"),
              [](RdpOptions &o, int v) { o.adminSession = v; });
    addText(connection, QStringLiteral("RD Gateway"),
            QStringLiteral("For company networks that reach computers through a Remote Desktop "
                           "Gateway: host or host:port (443 if not given)."),
            current.gateway, QStringLiteral("None (connect directly)"),
            QStringLiteral("gateway rdg tsgateway proxy"),
            [](RdpOptions &o, const QString &v) { o.gateway = v; });
    addText(connection, QStringLiteral("Gateway user"),
            QStringLiteral("DOMAIN\\user or user@domain. Empty logs on to the gateway with each "
                           "session's own credentials."),
            current.gatewayUser, QStringLiteral("Same as the session"),
            QStringLiteral("gateway user credentials"),
            [](RdpOptions &o, const QString &v) { o.gatewayUser = v; });
    connection->addRow(QStringLiteral("Per session"),
                       QStringLiteral("Every RDP session can override these in its own settings "
                                      "(Sessions → Edit…), and can also start a "
                                      "program instead of the desktop."),
                       nullptr, QStringLiteral("override start program"));

    // Trusted certificates: FreeRDP keeps one PEM file per host:port.
    auto *certificates = page->addCard(QStringLiteral("Trusted certificates"));
    auto *list = new QListWidget;
    list->setMinimumHeight(140);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    const QString folder = portableFilePath(QStringLiteral("settings/rdp/server"));
    auto reload = [list, folder] {
        list->clear();
        for (const auto &file : QDir(folder).entryList({QStringLiteral("*.pem")}, QDir::Files, QDir::Name)) {
            auto name = file.chopped(4);
            const int underscore = name.lastIndexOf(QLatin1Char('_'));
            auto *item = new QListWidgetItem(
                underscore > 0 ? QStringLiteral("%1 (port %2)").arg(name.left(underscore), name.mid(underscore + 1))
                               : name);
            item->setData(Qt::UserRole, file);
            list->addItem(item);
        }
    };
    reload();
    auto *forget = new QPushButton(QStringLiteral("Forget selected"));
    certificates->addRow(QStringLiteral("Computers you trusted"),
                         QStringLiteral("Forget one after its certificate was renewed or the "
                                        "computer was reinstalled; you will be asked again."),
                         forget, QStringLiteral("certificate fingerprint trust pem"));
    certificates->addWideRow(list, QStringLiteral("rdp certificates trusted"));
    connect(forget, &QPushButton::clicked, this, [this, list, folder, reload] {
        const auto selected = list->selectedItems();
        if (selected.isEmpty())
            return;
        if (QMessageBox::question(this, QStringLiteral("Forget Certificates"),
                                  QStringLiteral("Forget %1 trusted certificate(s)?").arg(selected.size()))
            != QMessageBox::Yes)
            return;
        for (const auto *item : selected)
            QFile::remove(QDir(folder).filePath(item->data(Qt::UserRole).toString()));
        reload();
    });
}

void PreferencesWindow::buildSftpPage()
{
    auto *page = addPage(QStringLiteral("sftp"), QStringLiteral("Files and transfers"),
                         QStringLiteral("folder-remote"), QStyle::SP_DirLinkIcon);
    auto update = PREFERENCE_UPDATER(SftpPreferences, sftpPreferences, setSftpPreferences);
    const auto current = m_mainWindow->sftpPreferences();

    auto *browsing = page->addCard(QStringLiteral("Browsing"));
    auto *hidden = toggle(current.showHidden);
    browsing->addRow(QStringLiteral("Show hidden files"),
                     QStringLiteral("Files and folders whose names start with a dot. Applies when "
                                    "a folder is next listed."),
                     hidden, QStringLiteral("dotfiles"));
    bindToggle(this, hidden, update, &SftpPreferences::showHidden);
    auto *confirmDelete = toggle(current.confirmDelete);
    browsing->addRow(QStringLiteral("Ask before deleting"), QString(), confirmDelete,
                     QStringLiteral("confirm remove"));
    bindToggle(this, confirmDelete, update, &SftpPreferences::confirmDelete);

    auto *downloads = page->addCard(QStringLiteral("Downloads"));
    downloads->addRow(QStringLiteral("Download folder"),
                      QStringLiteral("Empty uses your Downloads folder."),
                      pathPicker(current.downloadFolder, QStringLiteral("Downloads"), true,
                                 [update](const QString &path) {
                                     update([&](SftpPreferences &v) { v.downloadFolder = path; });
                                 }),
                      QStringLiteral("save location"));
    auto *ask = toggle(current.askDownloadFolder);
    downloads->addRow(QStringLiteral("Ask where to save each download"), QString(), ask,
                      QStringLiteral("save location dialog"));
    bindToggle(this, ask, update, &SftpPreferences::askDownloadFolder);
    auto *open = toggle(current.openFolderAfterDownload);
    downloads->addRow(QStringLiteral("Open the folder when a download finishes"), QString(), open,
                      QStringLiteral("explorer file manager"));
    bindToggle(this, open, update, &SftpPreferences::openFolderAfterDownload);

    auto *uploads = page->addCard(QStringLiteral("Uploads"));
    auto *temporary = choiceBox({{QStringLiteral("above"), QStringLiteral("Files above a size")},
                                 {QStringLiteral("all"), QStringLiteral("All files")},
                                 {QStringLiteral("off"), QStringLiteral("Off (write in place)")}},
                                current.temporaryUpload);
    uploads->addRow(QStringLiteral("Upload to a temporary name first"),
                    QStringLiteral("The file is written as name.filepart and replaces the old one "
                                   "only when complete, so a dropped connection never leaves a "
                                   "half-written file. For SFTP, SCP, and FTP."),
                    temporary, QStringLiteral("filepart partial atomic replace winscp safe upload"));
    auto *threshold = spinBox(0, 1024 * 1024, current.temporaryUploadKb, QStringLiteral(" KB"));
    uploads->addRow(QStringLiteral("Size from which to use it"), QString(), threshold,
                    QStringLiteral("filepart threshold"));
    const auto updateThreshold = [temporary, threshold] {
        threshold->setEnabled(temporary->currentData().toString() == QStringLiteral("above"));
    };
    connect(temporary, &QComboBox::activated, this, [update, temporary, updateThreshold] {
        update([&](SftpPreferences &v) { v.temporaryUpload = temporary->currentData().toString(); });
        updateThreshold();
    });
    bindSpin(this, threshold, update, &SftpPreferences::temporaryUploadKb);
    updateThreshold();

    auto *conflicts = page->addCard(QStringLiteral("When a file already exists"));
    auto *conflict = choiceBox({{QStringLiteral("ask"), QStringLiteral("Ask me")},
                                {QStringLiteral("rename"), QStringLiteral("Keep both")},
                                {QStringLiteral("overwrite"), QStringLiteral("Overwrite")},
                                {QStringLiteral("skip"), QStringLiteral("Skip")}},
                               current.conflict);
    conflicts->addRow(QStringLiteral("Action"),
                      QStringLiteral("For uploads and downloads alike."), conflict,
                      QStringLiteral("conflict overwrite replace skip rename"));
    bindChoice(this, conflict, update, &SftpPreferences::conflict);

    auto *editing = page->addCard(QStringLiteral("Editing remote files"));
    auto *editor = new QLineEdit(current.editor);
    editor->setPlaceholderText(QStringLiteral("The system's program for each file type"));
    editing->addRow(QStringLiteral("Editor"),
                    QStringLiteral("Edit (or double-click a file) opens it here; each save uploads "
                                   "it again. For example: code --wait, notepad++, gedit."),
                    editor, QStringLiteral("edit editor open text notepad code"));
    connect(editor, &QLineEdit::editingFinished, this, [update, editor] {
        update([&](SftpPreferences &v) { v.editor = editor->text().trimmed(); });
    });
}

void PreferencesWindow::buildSecurityPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("vault"), QStringLiteral("Vault"),
                         QStringLiteral("security-high"), QStyle::SP_MessageBoxWarning);
    auto update = PREFERENCE_UPDATER(SecurityPreferences, securityPreferences, setSecurityPreferences);
    const auto current = mainWindow->securityPreferences();

    auto *vault = page->addCard(QStringLiteral("Credential vault"));
    auto *state = new QLabel;
    auto *lockButton = new QPushButton;
    auto refreshState = [mainWindow, state, lockButton] {
        auto &v = mainWindow->vault();
        state->setText(!v.exists()       ? QStringLiteral("Not created")
                       : v.isUnlocked() ? QStringLiteral("Unlocked")
                                        : QStringLiteral("Locked"));
        lockButton->setText(!v.exists() ? QStringLiteral("Create…")
                            : v.isUnlocked() ? QStringLiteral("Lock now")
                                             : QStringLiteral("Unlock…"));
    };
    refreshState();
    auto *stateWidget = new QWidget;
    auto *stateLayout = new QHBoxLayout(stateWidget);
    stateLayout->setContentsMargins(0, 0, 0, 0);
    stateLayout->addWidget(state);
    stateLayout->addWidget(lockButton);
    vault->addRow(QStringLiteral("Vault"),
                  QStringLiteral("Session passwords are encrypted with your master password "
                                 "(Argon2id and XChaCha20-Poly1305)."),
                  stateWidget, QStringLiteral("lock unlock master password"));
    connect(lockButton, &QPushButton::clicked, this, [mainWindow, refreshState] {
        if (mainWindow->vault().isUnlocked())
            mainWindow->vault().lock();
        else
            mainWindow->unlockVault();
        refreshState();
    });
    // The vault can also be locked from the menu or by auto-lock.
    auto *stateTimer = new QTimer(state);
    connect(stateTimer, &QTimer::timeout, state, refreshState);
    stateTimer->start(2000);

    auto *change = new QPushButton(QStringLiteral("Change…"));
    vault->addRow(QStringLiteral("Master password"),
                  QStringLiteral("Re-encrypts the vault with a new password. The old one stops "
                                 "working at once, also in copies of this folder you make later."),
                  change, QStringLiteral("change master password rekey"));
    connect(change, &QPushButton::clicked, this, [this, mainWindow, refreshState] {
        if (!mainWindow->vault().exists()) {
            QMessageBox::information(this, QStringLiteral("Change Master Password"),
                                     QStringLiteral("Create the vault first."));
            return;
        }
        MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Change, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        auto oldPassword = dialog.currentPassword();
        auto newPassword = dialog.newPassword();
        QString error;
        mainWindow->vault().changeMasterPassword(oldPassword, newPassword, &error);
        oldPassword.fill(QChar(0));
        newPassword.fill(QChar(0));
        if (error.isEmpty())
            QMessageBox::information(this, QStringLiteral("Change Master Password"),
                                     QStringLiteral("The master password was changed."));
        else
            QMessageBox::warning(this, QStringLiteral("Change Master Password"), error);
        refreshState();
    });

    page = addPage(QStringLiteral("locking"), QStringLiteral("Locking"),
                   QStringLiteral("system-lock-screen"), QStyle::SP_MessageBoxWarning);
    auto *locking = page->addCard(QStringLiteral("Locking"));
    auto *autoLock = spinBox(0, 24 * 60, current.autoLockMinutes, QStringLiteral(" min"),
                             QStringLiteral("Never"));
    locking->addRow(QStringLiteral("Lock after inactivity"),
                    QStringLiteral("Locks the vault when you have not used the keyboard or mouse in "
                                   "OpsRemote for this long. Open tabs stay connected."),
                    autoLock, QStringLiteral("auto lock idle timeout"));
    bindSpin(this, autoLock, update, &SecurityPreferences::autoLockMinutes);
    auto *minimize = toggle(current.lockOnMinimize);
    locking->addRow(QStringLiteral("Lock when the window is minimized"), QString(), minimize,
                    QStringLiteral("auto lock"));
    bindToggle(this, minimize, update, &SecurityPreferences::lockOnMinimize);
    auto *startup = toggle(current.unlockAtStartup);
    locking->addRow(QStringLiteral("Ask for the master password at startup"),
                    QStringLiteral("Otherwise it is asked when a session first needs it."),
                    startup, QStringLiteral("unlock start"));
    bindToggle(this, startup, update, &SecurityPreferences::unlockAtStartup);

    page = addPage(QStringLiteral("password-storage"), QStringLiteral("Password storage"),
                   QStringLiteral("dialog-password"), QStyle::SP_DialogApplyButton);
    auto *storage = page->addCard(QStringLiteral("Where passwords are kept"));
    QString why;
    const bool systemAvailable = SystemCredentials::isAvailable(&why);
    auto *store = choiceBox({{QStringLiteral("vault"), QStringLiteral("OpsRemote vault")},
                             {QStringLiteral("system"), QStringLiteral("System credential store")},
                             {QStringLiteral("both"), QStringLiteral("Both")}},
                            current.passwordStore);
    storage->addRow(QStringLiteral("Store"),
                    QStringLiteral("The vault is encrypted with your master password and travels "
                                   "with the portable folder. The system store (%1) needs no master "
                                   "password but stays on this computer. Both: saved in both, read "
                                   "from the system first. Each session can choose its own (Edit "
                                   "Session).%2")
                        .arg(SystemCredentials::name(),
                             systemAvailable ? QString() : QStringLiteral(" Not available here: ") + why),
                    store, QStringLiteral("keyring credential manager secret service keychain"));
    bindChoice(this, store, update, &SecurityPreferences::passwordStore);
    auto *storeNote = new AvailabilityNote(
        [] {
            QString reason;
            return SystemCredentials::isAvailable(&reason) ? QString() : reason;
        },
        [store](bool available) {
            // The system store's choices wait for it (the current one stays).
            if (auto *model = qobject_cast<QStandardItemModel *>(store->model())) {
                for (const auto *key : {"system", "both"}) {
                    const int row = store->findData(QLatin1String(key));
                    if (auto *item = model->item(row))
                        item->setEnabled(available || row == store->currentIndex());
                }
            }
            // The copy buttons, found by name (made after this note).
            for (const auto *name : {"copyToSystemStore", "copyToVault"}) {
                if (auto *button = store->window()->findChild<QPushButton *>(QLatin1String(name)))
                    button->setEnabled(available);
            }
        });
    storage->addWideRow(storeNote, QStringLiteral("keyring libsecret"));
    auto *copies = page->addCard(QStringLiteral("Copy saved passwords"));
    auto *toSystem = new QPushButton(QStringLiteral("Copy to the system store"));
    toSystem->setEnabled(systemAvailable);
    copies->addRow(QStringLiteral("Vault → system store"),
                   QStringLiteral("Every password in the vault (unlocks it if needed)."),
                   toSystem, QStringLiteral("migrate export"));
    auto *toVault = new QPushButton(QStringLiteral("Copy to the vault"));
    toVault->setEnabled(systemAvailable);
    copies->addRow(QStringLiteral("System store → vault"),
                   QStringLiteral("The saved sessions' passwords, so they travel with the folder."),
                   toVault, QStringLiteral("migrate import"));
    toSystem->setObjectName(QStringLiteral("copyToSystemStore"));
    toVault->setObjectName(QStringLiteral("copyToVault"));
    storeNote->check();
    connect(toSystem, &QPushButton::clicked, this, [this, mainWindow = m_mainWindow] {
        QMessageBox::information(this, QStringLiteral("Copy Passwords"), mainWindow->copyPasswords(true));
    });
    connect(toVault, &QPushButton::clicked, this, [this, mainWindow = m_mainWindow] {
        QMessageBox::information(this, QStringLiteral("Copy Passwords"), mainWindow->copyPasswords(false));
    });
}

void PreferencesWindow::buildLoggingPage()
{
    auto *page = addPage(QStringLiteral("logging"), QStringLiteral("Session logging"),
                         QStringLiteral("text-x-generic"), QStyle::SP_FileDialogContentsView);
    auto update = PREFERENCE_UPDATER(LoggingPreferences, loggingPreferences, setLoggingPreferences);
    const auto current = m_mainWindow->loggingPreferences();

    auto *card = page->addCard();
    auto *enabled = toggle(current.enabled);
    card->addRow(QStringLiteral("Log terminal output"),
                 QStringLiteral("Every tab writes what it shows to a file, one file per tab and "
                                "connection. Starts and stops in open tabs at once."),
                 enabled, QStringLiteral("record history audit"));
    bindToggle(this, enabled, update, &LoggingPreferences::enabled);
    card->addRow(QStringLiteral("Log folder"),
                 QStringLiteral("Empty uses settings/logs. A relative folder is inside the "
                                "portable folder, so logs travel with it."),
                 pathPicker(current.folder, QStringLiteral("settings/logs"), true,
                            [update](const QString &path) {
                                const auto relative = QDir(portableHome()).relativeFilePath(path);
                                update([&](LoggingPreferences &v) {
                                    v.folder = path.isEmpty() || relative.startsWith(QStringLiteral(".."))
                                                   ? path : relative;
                                });
                            }),
                 QStringLiteral("directory location"));
    auto *timestamps = toggle(current.timestamps);
    card->addRow(QStringLiteral("Add a timestamp to each line"), QString(), timestamps,
                 QStringLiteral("time"));
    bindToggle(this, timestamps, update, &LoggingPreferences::timestamps);
    auto *plain = toggle(current.plainText);
    card->addRow(QStringLiteral("Plain text"),
                 QStringLiteral("Removes colours and other terminal control codes, so logs read "
                                "well in any text editor."),
                 plain, QStringLiteral("ansi escape strip"));
    bindToggle(this, plain, update, &LoggingPreferences::plainText);
    auto *open = new QPushButton(QStringLiteral("Open log folder"));
    card->addRow(QStringLiteral("Logs"), QString(), open, QStringLiteral("browse"));
    connect(open, &QPushButton::clicked, this, [mainWindow = m_mainWindow] {
        const auto folder = sessionLogFolder(mainWindow->loggingPreferences());
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });

    auto *recordings = page->addCard(QStringLiteral("Recordings"));
    auto *record = toggle(current.record);
    recordings->addRow(QStringLiteral("Record every session"),
                       QStringLiteral("Saves each terminal tab as an asciinema recording (.cast) "
                                      "in the log folder, to replay later with its timing (Tools "
                                      "→ Play Recording…, or asciinema). A single tab can also be "
                                      "recorded from its right-click menu."),
                       record, QStringLiteral("record replay asciinema cast audit training video"));
    bindToggle(this, record, update, &LoggingPreferences::record);
}

void PreferencesWindow::buildSnippetsPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("snippets"), QStringLiteral("Snippets and broadcast"),
                         QStringLiteral("insert-text"), QStyle::SP_FileDialogListView);

    auto *snippets = page->addCard(QStringLiteral("Snippets"));
    auto *table = editorTable({QStringLiteral("Name"), QStringLiteral("Press Enter"),
                               QStringLiteral("Text")});
    auto *buttons = new QWidget;
    auto *buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    auto *add = new QPushButton(QStringLiteral("Add"), buttons);
    auto *remove = new QPushButton(QStringLiteral("Remove"), buttons);
    buttonLayout->addWidget(add);
    buttonLayout->addWidget(remove);
    snippets->addRow(QStringLiteral("Saved commands"),
                     QStringLiteral("Send one to the current tab from Sessions → Send Snippet or "
                                    "the terminal's right-click menu. Double-click a cell to edit; "
                                    "use \\n for several lines."),
                     buttons, QStringLiteral("commands macro text"));
    snippets->addWideRow(table, QStringLiteral("snippets commands macro"));
    auto addRow = [table](const Snippet &snippet) {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(snippet.name));
        auto *enter = new QTableWidgetItem;
        enter->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        enter->setCheckState(snippet.pressEnter ? Qt::Checked : Qt::Unchecked);
        table->setItem(row, 1, enter);
        QString text = snippet.text;
        table->setItem(row, 2, new QTableWidgetItem(text.replace(QLatin1Char('\n'), QStringLiteral("\\n"))));
    };
    for (const auto &snippet : mainWindow->snippets())
        addRow(snippet);
    table->resizeColumnToContents(1);
    table->setColumnWidth(0, 160);
    auto save = [mainWindow, table] {
        QList<Snippet> list;
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto name = table->item(row, 0) ? table->item(row, 0)->text().trimmed() : QString();
            if (name.isEmpty())
                continue;
            QString text = table->item(row, 2) ? table->item(row, 2)->text() : QString();
            list.append({name, text.replace(QStringLiteral("\\n"), QStringLiteral("\n")),
                         table->item(row, 1) && table->item(row, 1)->checkState() == Qt::Checked});
        }
        mainWindow->setSnippets(list);
    };
    connect(table, &QTableWidget::itemChanged, this, save);
    connect(add, &QPushButton::clicked, this, [table, addRow] {
        addRow({QStringLiteral("New snippet"), QString(), true});
        table->setCurrentCell(table->rowCount() - 1, 0);
        table->editItem(table->item(table->rowCount() - 1, 0));
    });
    connect(remove, &QPushButton::clicked, this, [table, save] {
        QList<int> rows;
        for (const auto *item : table->selectedItems())
            rows.append(item->row());
        std::sort(rows.begin(), rows.end(), std::greater<>());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        for (int row : rows)
            table->removeRow(row);
        save();
    });

    auto *broadcast = page->addCard(QStringLiteral("Broadcast"));
    broadcast->addRow(QStringLiteral("Type into every SSH tab at once"),
                      QStringLiteral("Turn it on and off with View → Broadcast Typing to All SSH "
                                     "Tabs (Ctrl+Shift+B by default). A red BROADCAST sign shows "
                                     "in the status bar while it is on."),
                      nullptr, QStringLiteral("multi exec cluster all tabs send"));
}

void PreferencesWindow::buildHighlightsPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("highlights"), QStringLiteral("Highlight rules"),
                         QStringLiteral("format-text-color"), QStyle::SP_DialogApplyButton);
    auto *card = page->addCard();
    auto *table = editorTable({QStringLiteral("On"), QStringLiteral("Colour"), QStringLiteral("Bold"),
                               QStringLiteral("Match case"), QStringLiteral("Pattern")});
    auto *buttons = new QWidget;
    auto *buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    auto *add = new QPushButton(QStringLiteral("Add"), buttons);
    auto *remove = new QPushButton(QStringLiteral("Remove"), buttons);
    buttonLayout->addWidget(add);
    buttonLayout->addWidget(remove);
    card->addRow(QStringLiteral("Colour matching text"),
                 QStringLiteral("Words or regular expressions coloured in SSH output, for example "
                                "error, warning, or your production host names. Full-screen "
                                "programs such as vim are left alone. Double-click the colour to "
                                "change it."),
                 buttons, QStringLiteral("regex keyword color highlight"));
    card->addWideRow(table, QStringLiteral("highlight rules regex pattern"));
    auto checkItem = [](bool checked) {
        auto *item = new QTableWidgetItem;
        item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        return item;
    };
    auto addRow = [table, checkItem](const HighlightRule &rule) {
        const QSignalBlocker blocker(table);
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, checkItem(rule.enabled));
        auto *colour = new QTableWidgetItem(rule.color.name());
        colour->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        colour->setBackground(rule.color);
        colour->setForeground(rule.color.lightness() > 128 ? Qt::black : Qt::white);
        table->setItem(row, 1, colour);
        table->setItem(row, 2, checkItem(rule.bold));
        table->setItem(row, 3, checkItem(rule.caseSensitive));
        table->setItem(row, 4, new QTableWidgetItem(rule.pattern));
    };
    for (const auto &rule : mainWindow->highlightRules())
        addRow(rule);
    for (int column = 0; column < 4; ++column)
        table->resizeColumnToContents(column);
    auto save = [this, mainWindow, table] {
        QList<HighlightRule> rules;
        QStringList invalid;
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto pattern = table->item(row, 4) ? table->item(row, 4)->text() : QString();
            if (pattern.isEmpty())
                continue;
            if (!QRegularExpression(pattern).isValid())
                invalid.append(pattern);
            rules.append({pattern, QColor(table->item(row, 1)->text()),
                          table->item(row, 2)->checkState() == Qt::Checked,
                          table->item(row, 3)->checkState() == Qt::Checked,
                          table->item(row, 0)->checkState() == Qt::Checked});
        }
        mainWindow->setHighlightRules(rules);
        if (!invalid.isEmpty())
            QMessageBox::warning(this, QStringLiteral("Highlight Rules"),
                                 QStringLiteral("These patterns are not valid regular expressions "
                                                "and are ignored:\n%1").arg(invalid.join(QLatin1Char('\n'))));
    };
    connect(table, &QTableWidget::itemChanged, this, save);
    connect(table, &QTableWidget::cellDoubleClicked, this, [this, table, save](int row, int column) {
        if (column != 1)
            return;
        const auto colour = QColorDialog::getColor(QColor(table->item(row, 1)->text()), this);
        if (!colour.isValid())
            return;
        {
            const QSignalBlocker blocker(table);
            auto *item = table->item(row, 1);
            item->setText(colour.name());
            item->setBackground(colour);
            item->setForeground(colour.lightness() > 128 ? Qt::black : Qt::white);
        }
        save();
    });
    connect(add, &QPushButton::clicked, this, [table, addRow] {
        addRow({QStringLiteral("\\bpattern\\b"), QColor(0x3d, 0x8b, 0xfd), false, false, true});
        table->setCurrentCell(table->rowCount() - 1, 4);
        table->editItem(table->item(table->rowCount() - 1, 4));
    });
    connect(remove, &QPushButton::clicked, this, [table, save] {
        QList<int> rows;
        for (const auto *item : table->selectedItems())
            rows.append(item->row());
        std::sort(rows.begin(), rows.end(), std::greater<>());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        for (int row : rows)
            table->removeRow(row);
        save();
    });
}

void PreferencesWindow::buildLocalShellPage()
{
    auto *page = addPage(QStringLiteral("local-shell"), QStringLiteral("Local shell"),
                         QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    auto update = PREFERENCE_UPDATER(LocalShellPreferences, localShellPreferences,
                                     setLocalShellPreferences);
    const auto current = m_mainWindow->localShellPreferences();
    auto *card = page->addCard(QStringLiteral("Local terminals"));
#ifdef Q_OS_WIN
    auto *shell = choiceBox({{QStringLiteral("powershell"), QStringLiteral("Windows PowerShell")},
                             {QStringLiteral("pwsh"), QStringLiteral("PowerShell 7 (pwsh)")},
                             {QStringLiteral("cmd"), QStringLiteral("Command Prompt")},
                             {QStringLiteral("wsl"), QStringLiteral("WSL (the default Linux distribution)")},
                             {QStringLiteral("custom"), QStringLiteral("Another program")}},
                            current.windowsShell);
    card->addRow(QStringLiteral("Shell"),
                 QStringLiteral("What File → New Local Terminal starts, in a Windows pseudo "
                                "console. PowerShell 7 and WSL need to be installed."),
                 shell, QStringLiteral("powershell pwsh cmd wsl bash command prompt conpty"));
    bindChoice(this, shell, update, &LocalShellPreferences::windowsShell);
    auto *custom = new QLineEdit(current.windowsCustom);
    custom->setPlaceholderText(QStringLiteral("e.g. C:\\msys64\\usr\\bin\\bash.exe --login"));
    card->addRow(QStringLiteral("Another program"),
                 QStringLiteral("With its arguments; used when the shell is \"Another program\"."),
                 custom, QStringLiteral("custom command"));
    connect(custom, &QLineEdit::editingFinished, this, [update, custom] {
        update([&](LocalShellPreferences &v) { v.windowsCustom = custom->text().trimmed(); });
    });

    auto *admin = page->addCard(QStringLiteral("Run as administrator"));
    admin->addRow(QStringLiteral("Administrator terminals"),
                  QStringLiteral("File → New Local Terminal (Administrator), or Administrator in "
                                 "the split pane chooser. Windows asks first (UAC); a standard user "
                                 "types an administrator's name and password, and the shell then "
                                 "runs as that administrator. Not for WSL."),
                  nullptr, QStringLiteral("admin elevated uac runas root"));
    auto *sudo = toggle(current.useSudo);
    admin->addRow(QStringLiteral("Use Windows sudo when it is turned on"),
                  QStringLiteral("Windows 11's sudo, if an administrator turned it on in inline "
                                 "mode (Settings → System → For developers). Otherwise OpsRemote "
                                 "starts the shell through its own elevated helper."),
                  sudo, QStringLiteral("sudo inline"));
    bindToggle(this, sudo, update, &LocalShellPreferences::useSudo);
#else
    auto *shell = choiceBox({{QStringLiteral("default"), QStringLiteral("My login shell ($SHELL)")},
                             {QStringLiteral("custom"), QStringLiteral("Another program")}},
                            current.linuxShell);
    card->addRow(QStringLiteral("Shell"),
                 QStringLiteral("What File → New Local Terminal starts."), shell,
                 QStringLiteral("bash zsh fish shell"));
    bindChoice(this, shell, update, &LocalShellPreferences::linuxShell);
    auto *custom = new QLineEdit(current.linuxCustom);
    custom->setPlaceholderText(QStringLiteral("e.g. /usr/bin/fish -l"));
    card->addRow(QStringLiteral("Another program"),
                 QStringLiteral("With its arguments; used when the shell is \"Another program\"."),
                 custom, QStringLiteral("custom command"));
    connect(custom, &QLineEdit::editingFinished, this, [update, custom] {
        update([&](LocalShellPreferences &v) { v.linuxCustom = custom->text().trimmed(); });
    });
#endif
}

void PreferencesWindow::buildSessionListPage()
{
    auto *page = addPage(QStringLiteral("appearance-sessions"), QStringLiteral("Session list"),
                         QStringLiteral("view-list-tree"), QStyle::SP_FileDialogListView);
    auto update = PREFERENCE_UPDATER(SessionListPreferences, sessionListPreferences,
                                     setSessionListPreferences);
    const auto current = m_mainWindow->sessionListPreferences();

    auto *folders = page->addCard(QStringLiteral("Pinned and recent"));
    auto *pinned = toggle(current.showPinned);
    folders->addRow(QStringLiteral("Show Pinned"),
                    QStringLiteral("Sessions pinned with right-click → Pin, at the top of the list."),
                    pinned, QStringLiteral("favourites favorites star"));
    bindToggle(this, pinned, update, &SessionListPreferences::showPinned);
    auto *recent = toggle(current.showRecent);
    folders->addRow(QStringLiteral("Show Recent"),
                    QStringLiteral("The sessions opened last, below Pinned."), recent,
                    QStringLiteral("history last used"));
    bindToggle(this, recent, update, &SessionListPreferences::showRecent);
    auto *recentCount = spinBox(1, 20, current.recentCount, QString());
    folders->addRow(QStringLiteral("Recent sessions"), QString(), recentCount,
                    QStringLiteral("history count"));
    bindSpin(this, recentCount, update, &SessionListPreferences::recentCount);

    auto *search = page->addCard(QStringLiteral("Search"));
    auto *showSearch = toggle(current.showSearch);
    search->addRow(QStringLiteral("Show the search box"),
                   QStringLiteral("Above the list. Type parts of a name, host, user, protocol, or "
                                  "folder; every word must match. Down goes to the results, Enter "
                                  "opens the first one, Esc clears. Sessions → Search Sessions "
                                  "(Ctrl+Shift+S) jumps to it even when it is hidden."),
                   showSearch, QStringLiteral("filter find search box"));
    bindToggle(this, showSearch, update, &SessionListPreferences::showSearch);

    auto *columns = page->addCard(QStringLiteral("Columns"));
    auto *host = toggle(current.showHost);
    columns->addRow(QStringLiteral("Host"),
                    QStringLiteral("The status dot and the name always show."), host,
                    QStringLiteral("ip address column"));
    bindToggle(this, host, update, &SessionListPreferences::showHost);
    auto *protocol = toggle(current.showProtocol);
    columns->addRow(QStringLiteral("Protocol"), QString(), protocol,
                    QStringLiteral("ssh rdp vnc column"));
    bindToggle(this, protocol, update, &SessionListPreferences::showProtocol);
    auto *summary = choiceBox({{QStringLiteral("name"), QStringLiteral("After the folder name")},
                               {QStringLiteral("column"), QStringLiteral("In the next column")},
                               {QStringLiteral("off"), QStringLiteral("Disabled")}},
                              current.folderSummary);
    columns->addRow(QStringLiteral("Folder summary"),
                    QStringLiteral("How many hosts of a folder answer, like 3 ● 1 ○. In the next "
                                   "column it lines up with the column after the name."),
                    summary, QStringLiteral("count reachable folder"));
    bindChoice(this, summary, update, &SessionListPreferences::folderSummary);
    auto *fit = toggle(current.fitPanel);
    columns->addRow(QStringLiteral("Always fit the panel to the list"),
                    QStringLiteral("The side panel grows and shrinks by itself with the names, "
                                   "columns, and open folders, so it never needs resizing by "
                                   "hand. Off: drag its edge."),
                    fit, QStringLiteral("width resize auto size"));
    bindToggle(this, fit, update, &SessionListPreferences::fitPanel);

    auto *status = page->addCard(QStringLiteral("Status"));
    auto *check = toggle(current.checkStatus);
    status->addRow(QStringLiteral("Check whether hosts answer"),
                   QStringLiteral("A filled dot when the host answers on its port, an empty "
                                  "one when it does not. Only knocks on the port; never logs on. "
                                  "Sessions through a jump host or proxy show a dash."),
                   check, QStringLiteral("reachable online ping up down status"));
    bindToggle(this, check, update, &SessionListPreferences::checkStatus);
    auto *every = spinBox(10, 3600, current.checkSeconds, QStringLiteral(" s"));
    status->addRow(QStringLiteral("Check every"), QString(), every, QStringLiteral("interval"));
    bindSpin(this, every, update, &SessionListPreferences::checkSeconds);
}

void PreferencesWindow::buildPresentationPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("presentation"), QStringLiteral("Presentation mode"),
                         QStringLiteral("video-display"), QStyle::SP_DesktopIcon);
    auto update = PREFERENCE_UPDATER(PresentationPreferences, presentationPreferences,
                                     setPresentationPreferences);
    const auto current = mainWindow->presentationPreferences();
    auto *card = page->addCard();
    auto *start = new QPushButton(QStringLiteral("Start"));
    card->addRow(QStringLiteral("Presentation mode"),
                 QStringLiteral("For screen sharing, demos, and projectors: bigger terminal text "
                                "and nothing else on screen. Toggle it with View → Presentation "
                                "Mode (Ctrl+Shift+P by default)."),
                 start, QStringLiteral("demo screen share projector big"));
    connect(start, &QPushButton::clicked, this, [this, mainWindow] {
        hide();
        mainWindow->setPresentationMode(true);
    });
    auto *zoom = spinBox(0, 20, current.zoomSteps, QStringLiteral(" steps"));
    card->addRow(QStringLiteral("Enlarge terminal text"), QStringLiteral("Zoom steps, like Ctrl++."),
                 zoom, QStringLiteral("font size zoom"));
    bindSpin(this, zoom, update, &PresentationPreferences::zoomSteps);
    auto *fullScreen = toggle(current.fullScreen);
    card->addRow(QStringLiteral("Full screen"), QString(), fullScreen);
    bindToggle(this, fullScreen, update, &PresentationPreferences::fullScreen);
    auto *panels = toggle(current.hidePanels);
    card->addRow(QStringLiteral("Hide the side panel, toolbar, and status bar"),
                 QStringLiteral("Also keeps session names and hosts in the side panel off "
                                "screen."),
                 panels, QStringLiteral("privacy clean"));
    bindToggle(this, panels, update, &PresentationPreferences::hidePanels);
}

void PreferencesWindow::buildDataPage()
{
    auto *page = addPage(QStringLiteral("locations"), QStringLiteral("Locations"),
                         QStringLiteral("drive-removable-media"), QStyle::SP_DriveFDIcon);
    auto current = loadBackupPreferences();

    auto *locations = page->addCard(QStringLiteral("Where your data lives"));
    auto addFolderRow = [this, locations](const QString &title, const QString &description,
                                          const QString &folder, const QString &keywords) {
        auto *open = new QPushButton(QStringLiteral("Open"));
        locations->addRow(title, description + QStringLiteral("\n") + QDir::toNativeSeparators(folder),
                          open, keywords);
        connect(open, &QPushButton::clicked, this, [folder] {
            QDir().mkpath(folder);
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        });
    };
    addFolderRow(QStringLiteral("Settings"),
                 QStringLiteral("Sessions, preferences, the encrypted vault, and trusted host keys. "
                                "Shared by the Linux and Windows programs."),
                 portableFilePath(QStringLiteral("settings")), QStringLiteral("portable usb folder"));
    addFolderRow(QStringLiteral("Backups"), QStringLiteral("Copies of the settings folder."),
                 backupsFolder(), QStringLiteral("restore"));
    addFolderRow(QStringLiteral("Program"),
                 QStringLiteral("The running program. In the package it is unpacked from the "
                                "compressed folder into runtime once per version; older copies "
                                "are removed automatically."),
                 QCoreApplication::applicationDirPath(),
                 QStringLiteral("runtime cache unpacked compressed zstd"));

    page = addPage(QStringLiteral("backups"), QStringLiteral("Backups"),
                   QStringLiteral("document-save"), QStyle::SP_DialogSaveButton);
    auto *backups = page->addCard(QStringLiteral("Backups"));
    auto *onStart = toggle(current.backupOnStart);
    backups->addRow(QStringLiteral("Back up at every start"),
                    QStringLiteral("Logs are not included. The vault stays encrypted in backups."),
                    onStart, QStringLiteral("automatic"));
    connect(onStart, &QCheckBox::toggled, this, [](bool on) {
        auto value = loadBackupPreferences();
        value.backupOnStart = on;
        saveBackupPreferences(value);
    });
    auto *removeRuntime = toggle(current.removeRuntimeOnExit);
    locations->addRow(QStringLiteral("Remove the unpacked program when closing"),
                      QStringLiteral("Deletes runtime after OpsRemote closes, unless another "
                                     "window still uses it. Every start then unpacks it again, which "
                                     "takes a few seconds (longer on slow USB sticks). Settings are "
                                     "never affected."),
                      removeRuntime, QStringLiteral("runtime cache clean delete traces exit"));
    connect(removeRuntime, &QCheckBox::toggled, this, [](bool on) {
        auto value = loadBackupPreferences();
        value.removeRuntimeOnExit = on;
        saveBackupPreferences(value);
    });

    auto *keep = spinBox(1, 100, current.keep, QStringLiteral(" backups"));
    backups->addRow(QStringLiteral("Keep the newest"), QString(), keep, QStringLiteral("prune"));
    connect(keep, &QSpinBox::valueChanged, this, [](int value) {
        auto preferences = loadBackupPreferences();
        preferences.keep = value;
        saveBackupPreferences(preferences);
        pruneBackups(value);
    });

    auto *list = new QListWidget;
    list->setMinimumHeight(160);
    auto reload = [list] {
        list->clear();
        for (const auto &name : listBackups())
            list->addItem(name);
    };
    reload();
    auto *buttons = new QWidget;
    auto *buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    auto *now = new QPushButton(QStringLiteral("Back up now"), buttons);
    auto *restore = new QPushButton(QStringLiteral("Restore selected…"), buttons);
    buttonLayout->addWidget(now);
    buttonLayout->addWidget(restore);
    backups->addRow(QStringLiteral("Saved backups"),
                    QStringLiteral("Restoring first backs up the current settings, then needs a "
                                   "restart of OpsRemote."),
                    buttons, QStringLiteral("restore undo"));
    backups->addWideRow(list, QStringLiteral("backups restore list"));
    connect(now, &QPushButton::clicked, this, [this, reload] {
        QString error;
        if (createBackup(&error).isEmpty())
            QMessageBox::warning(this, QStringLiteral("Back Up"), error);
        pruneBackups(loadBackupPreferences().keep);
        reload();
    });
    connect(restore, &QPushButton::clicked, this, [this, list, reload] {
        auto *item = list->currentItem();
        if (!item)
            return;
        if (QMessageBox::question(this, QStringLiteral("Restore Backup"),
                                  QStringLiteral("Replace the current settings with the backup from "
                                                 "%1? The current settings are backed up first.")
                                      .arg(item->text()))
            != QMessageBox::Yes)
            return;
        QString error;
        if (restoreBackup(item->text(), &error))
            QMessageBox::information(this, QStringLiteral("Restore Backup"),
                                     QStringLiteral("Restored. Restart OpsRemote to use the "
                                                    "restored settings; until then, changes you "
                                                    "make may overwrite them."));
        else
            QMessageBox::warning(this, QStringLiteral("Restore Backup"), error);
        reload();
    });
}

void PreferencesWindow::buildAboutPage()
{
    auto *page = addPage(QStringLiteral("about-program"), QStringLiteral("OpsRemote"),
                         QStringLiteral("help-about"), QStyle::SP_MessageBoxInformation);

    auto *product = page->addCard();
    // The program's icon beside its name, so the page shows what OpsRemote is.
    auto *identity = new QWidget;
    auto *identityLayout = new QHBoxLayout(identity);
    identityLayout->setContentsMargins(0, 8, 0, 12);
    identityLayout->setSpacing(16);
    auto *logo = new QLabel(identity);
    logo->setPixmap(QApplication::windowIcon().pixmap(QSize(64, 64), devicePixelRatioF()));
    logo->setFixedSize(64, 64);
    identityLayout->addWidget(logo, 0, Qt::AlignVCenter);
    auto *identityTexts = new QVBoxLayout;
    identityTexts->setSpacing(2);
    auto *name = new QLabel(QStringLiteral("OpsRemote"), identity);
    setHeadingFont(name, 1.4);
    identityTexts->addWidget(name);
    identityTexts->addWidget(new QLabel(
        QStringLiteral("Version %1").arg(QStringLiteral(OPSREMOTE_VERSION)), identity));
    identityTexts->addWidget(
        secondaryLabel(QStringLiteral("A portable terminal and remote-access workspace for Linux "
                                      "and Windows. © 2026 Thanos Athanasopoulos."),
                       identity));
    identityLayout->addLayout(identityTexts, 1);
    product->addWideRow(identity, QStringLiteral("opsremote version author creator copyright"));
    // GPLv3's "Appropriate Legal Notices": the licence, how to read it, and
    // that there is no warranty.
    auto *licenceButton = new QPushButton(QStringLiteral("Read Licence"));
    product->addRow(QStringLiteral("Licence"),
                    QStringLiteral("OpsRemote is free software: you can redistribute it and/or "
                                   "modify it under the terms of the GNU General Public License, "
                                   "version 3 or (at your option) any later version. It comes "
                                   "with ABSOLUTELY NO WARRANTY, to the extent permitted by law."),
                    licenceButton, QStringLiteral("licence license gpl warranty free software"));
    connect(licenceButton, &QPushButton::clicked, this, [] {
        // The package's LICENSE file, or the text on gnu.org.
        const QString file = portableFilePath(QStringLiteral("LICENSE"));
        if (!QFile::exists(file)
            || !QDesktopServices::openUrl(QUrl::fromLocalFile(file)))
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.gnu.org/licenses/gpl-3.0.html")));
    });
    auto *sourceButton = new QPushButton(QStringLiteral("Source code"));
    product->addRow(QStringLiteral("Source and licences"),
                    QStringLiteral("github.com/sektorhybrid/OpsRemote. The release packages "
                                   "include LICENSE, THIRD_PARTY.md, and every component's licence "
                                   "texts in licenses/."),
                    sourceButton, QStringLiteral("github source licence license"));
    connect(sourceButton, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/sektorhybrid/OpsRemote")));
    });

    // This copy: where it keeps its data and what it runs on.
    auto *system = page->addCard(QStringLiteral("This copy"));
    auto *openSettings = new QPushButton(QStringLiteral("Open folder"));
    system->addRow(QStringLiteral("Settings folder"),
                   QDir::toNativeSeparators(portableFilePath(QStringLiteral("settings"))),
                   openSettings, QStringLiteral("data portable location path"));
    connect(openSettings, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(portableFilePath(QStringLiteral("settings"))));
    });

    const auto colourScheme = [] {
        switch (QGuiApplication::styleHints()->colorScheme()) {
        case Qt::ColorScheme::Dark: return QStringLiteral("dark");
        case Qt::ColorScheme::Light: return QStringLiteral("light");
        default: return QStringLiteral("not reported");
        }
    };
    const auto diagnostics = [colourScheme] {
        return QStringList{
            QStringLiteral("OpsRemote %1").arg(QStringLiteral(OPSREMOTE_VERSION)),
            QStringLiteral("System: %1 (%2), kernel %3")
                .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
                     QSysInfo::kernelVersion()),
            QStringLiteral("Qt: %1, platform %2").arg(QString::fromLatin1(qVersion()),
                                                      QGuiApplication::platformName()),
            QStringLiteral("libssh: %1").arg(QString::fromLatin1(ssh_version(0))),
            QStringLiteral("libsodium: %1").arg(QString::fromLatin1(sodium_version_string())),
            QStringLiteral("OS colour scheme: %1").arg(colourScheme()),
            QStringLiteral("Data folder: %1").arg(QDir::toNativeSeparators(portableHome())),
        }.join(QLatin1Char('\n'));
    };
    system->addRow(QStringLiteral("System"),
                   QStringLiteral("%1 (%2) · Qt %3 on %4")
                       .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
                            QString::fromLatin1(qVersion()), QGuiApplication::platformName()),
                   nullptr, QStringLiteral("platform operating system qt diagnostics"));
    auto *copyDiagnostics = new QPushButton(QStringLiteral("Copy"));
    system->addRow(QStringLiteral("Diagnostics"),
                   QStringLiteral("Versions and paths, for reporting a problem."),
                   copyDiagnostics, QStringLiteral("support debug report versions"));
    connect(copyDiagnostics, &QPushButton::clicked, this, [diagnostics, copyDiagnostics] {
        QApplication::clipboard()->setText(diagnostics());
        copyDiagnostics->setText(QStringLiteral("Copied"));
    });

    // Bundled open-source components and their licences.
    page = addPage(QStringLiteral("components"), QStringLiteral("Components"),
                   QStringLiteral("help-about"), QStyle::SP_MessageBoxInformation);
    auto *components = page->addCard(QStringLiteral("Open-source components"));
    const QList<QPair<QString, QString>> list{
        {QStringLiteral("Qt %1").arg(QString::fromLatin1(qVersion())), QStringLiteral("LGPL-3.0 · qt.io")},
        {QStringLiteral("QTermWidget (with OpsRemote changes)"), QStringLiteral("GPL-2.0-or-later, parts LGPL-2.0-or-later · github.com/lxqt/qtermwidget")},
        {QStringLiteral("FreeRDP"), QStringLiteral("Apache-2.0 · freerdp.com")},
        {QStringLiteral("LibVNCServer (libvncclient)"), QStringLiteral("GPL-2.0-or-later · libvnc.github.io")},
        {QStringLiteral("libssh %1").arg(QString::fromLatin1(ssh_version(0))), QStringLiteral("LGPL-2.1-or-later · libssh.org")},
        {QStringLiteral("OpenSSL"), QStringLiteral("Apache-2.0 · openssl.org")},
        {QStringLiteral("libsodium %1").arg(QString::fromLatin1(sodium_version_string())), QStringLiteral("ISC · libsodium.org")},
        {QStringLiteral("yaml-cpp"), QStringLiteral("MIT · github.com/jbeder/yaml-cpp")},
        {QStringLiteral("zstd"), QStringLiteral("BSD-3-Clause · facebook.github.io/zstd")},
        {QStringLiteral("zlib, libjpeg-turbo"), QStringLiteral("Zlib, IJG/BSD-3-Clause")},
        {QStringLiteral("ICU (Linux), utf8proc (Windows)"), QStringLiteral("Unicode-3.0, MIT")},
        {QStringLiteral("Kvantum 1.1.8 (with OpsRemote changes)"), QStringLiteral("GPL-3.0-or-later · github.com/tsujan/Kvantum")},
        {QStringLiteral("iTerm2-Color-Schemes"), QStringLiteral("MIT · github.com/mbadolato/iTerm2-Color-Schemes")},
        {QStringLiteral("Icon themes"), QStringLiteral("GPL-3.0, LGPL-3.0, CC-BY-SA-4.0 · licence files in data/icons/<theme>/")},
    };
    for (const auto &component : list)
        components->addRow(component.first, component.second, nullptr,
                           QStringLiteral("licence license credits third-party"));
}

// Cards stand out from the page like in Windows 11. Shades are mixed from the
// palette's text and window colours rather than taken from roles such as
// Midlight, which colour themes do not set and platforms fill in
// inconsistently (dark grey on a light Windows palette).
void PreferencesWindow::applyStyleSheet()
{
    const auto mix = [](const QColor &a, const QColor &b, qreal amount) {
        return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount,
                                a.greenF() + (b.greenF() - a.greenF()) * amount,
                                a.blueF() + (b.blueF() - a.blueF()) * amount)
            .name();
    };
    const auto window = palette().color(QPalette::Window);
    const auto text = palette().color(QPalette::Text);
    const auto line = mix(window, text, 0.18);
    const auto hover = mix(window, text, 0.07);
    const auto selected = mix(window, text, 0.11);
    setStyleSheet(QStringLiteral(
        "#settingsCard { background: palette(base); border: 1px solid %1; border-radius: 8px; }"
        "#settingsDivider { background: %1; border: none; }"
        "#settingsPageContent { background: palette(window); }"
        "QListWidget#settingsNavigation { border: none; background: transparent; }"
        "QListWidget#settingsNavigation::item { padding: 3px 6px; margin: 1px 0; border-radius: 4px;"
        " border-left: 2px solid transparent;"
        " color: palette(text); }"
        "QListWidget#settingsNavigation::item:hover { background: %2; }"
        "QListWidget#settingsNavigation::item:selected { background: %3;"
        " border-left-color: palette(highlight); color: palette(text); }")
                      .arg(line, hover, selected));
}

void PreferencesWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange && !m_applyingStyle) {
        m_applyingStyle = true;
        applyStyleSheet();
        m_applyingStyle = false;
    }
    // Headings have their own font, so follow interface font changes by hand.
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange) {
        for (auto *label : findChildren<QLabel *>()) {
            const auto scale = label->property("headingScale");
            if (scale.isValid())
                setHeadingFont(label, scale.toReal());
        }
    }
    QWidget::changeEvent(event);
}
