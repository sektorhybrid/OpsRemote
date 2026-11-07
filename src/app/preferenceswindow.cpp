// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "preferenceswindow.h"
#include "filedialogs.h"
#include "formcard.h"
#include "programpicker.h"
#include "reconnect.h"
#include "tabactivity.h"
#include "kvantumthemes.h"
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
#include "sshauth.h"
#include "automation.h"
#include "scripts.h"
#include "localsessions.h"
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
#include <QProcess>
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
#include <QAbstractButton>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QStandardItemModel>
#include <QScrollArea>
#include <QScrollBar>
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

// A colour to click on. Painted here, not as a button icon: styles (Kvantum)
// turn a disabled button's icon grey, and a greyed-out swatch should still
// show its colour, only fainter.
class ColourSwatch final : public QAbstractButton
{
public:
    explicit ColourSwatch(QWidget *parent = nullptr) : QAbstractButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFixedSize(40, fontMetrics().height() + 10);
    }

    void setColour(const QColor &colour)
    {
        m_colour = colour;
        update();
    }
    QColor colour() const { return m_colour; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool enabled = isEnabled();
        QPen border(palette().color(enabled && (underMouse() || hasFocus()) ? QPalette::Highlight
                                                                           : QPalette::Mid));
        border.setWidthF(enabled ? 1.0 : 0.8);
        if (!enabled)
            border.setStyle(Qt::DashLine);
        // A see-through colour (the faint pulse colour) shows as it does on
        // the tab bar: over the window colour.
        const QRectF box = QRectF(rect()).adjusted(1.5, 3.5, -1.5, -3.5);
        painter.setOpacity(enabled ? 1.0 : 0.45);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawRoundedRect(box, 4, 4);
        painter.setBrush(m_colour);
        painter.drawRoundedRect(box, 4, 4);
        painter.setOpacity(1.0);
        painter.setPen(border);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box, 4, 4);
    }
    void enterEvent(QEnterEvent *event) override
    {
        QAbstractButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent *event) override
    {
        QAbstractButton::leaveEvent(event);
        update();
    }

private:
    QColor m_colour;
};

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
// A full-width row's frame: the rows' usual space above and below, none
// while its content hides itself (an availability note with nothing to say).
class WideRow final : public QWidget
{
public:
    WideRow(QWidget *content, QWidget *parent)
        : QWidget(parent), m_layout(new QVBoxLayout(this))
    {
        setObjectName(QStringLiteral("settingsRow"));
        content->setParent(this);
        m_layout->addWidget(content);
        content->installEventFilter(this);
        fit(!content->isHidden());
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show || event->type() == QEvent::Hide)
            fit(event->type() == QEvent::Show);
        return QWidget::eventFilter(watched, event);
    }

private:
    void fit(bool shown) { m_layout->setContentsMargins(0, shown ? 8 : 0, 0, shown ? 8 : 0); }

    QVBoxLayout *m_layout;
};

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

    // A row whose content spans the card's width, with the same space above
    // and below as the other rows (not right against the dividers).
    QWidget *addWideRow(QWidget *content, const QString &keywords)
    {
        auto *row = new WideRow(content, this);
        registerRow(row, keywords);
        return row;
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

    static constexpr int PaddingX = 10;
    static constexpr int PaddingY = 8;
    static constexpr int LineGap = 4;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        // Name and description, LineGap apart, with Padding all round.
        return {200, option.fontMetrics.height() * 2 + LineGap + 2 * PaddingY};
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
        const QRect rect = option.rect.adjusted(PaddingX, PaddingY, -PaddingX, -PaddingY);
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
        const int lineHeight = option.fontMetrics.height();
        // Both lines together, centred in the row, LineGap apart.
        const int top = textRect.top() + (textRect.height() - (2 * lineHeight + LineGap)) / 2;
        painter->setFont(nameFont);
        painter->setPen(text);
        QString name = index.data(NameRole).toString();
        if (index.data(InUseRole).toBool())
            name += QStringLiteral("  ✓ in use");
        painter->drawText(QRect(textRect.left(), top, textRect.width(), lineHeight),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, textRect.width()));
        painter->setFont(option.font);
        QColor muted = text;
        muted.setAlphaF(selected ? 0.85 : 0.65);
        painter->setPen(muted);
        painter->drawText(QRect(textRect.left(), top + lineHeight + LineGap, textRect.width(), lineHeight),
                          Qt::AlignLeft | Qt::AlignVCenter,
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
    beginCategory(QStringLiteral("workspace"), QStringLiteral("Workspace"),
                  QStringLiteral("preferences-system"), QStyle::SP_TitleBarMaxButton);
    buildStartupPage();
    buildShortcutsPage();
    buildPresentationPage();
    endCategory();
    beginCategory(QStringLiteral("terminal-category"), QStringLiteral("Terminals"),
                  QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    buildTerminalPage();
    buildLocalShellPage();
    buildHighlightsPage();
    buildSnippetsPage();
    buildAutomationPage();
    buildLoggingPage();
    endCategory();
    // One tab per session type.
    beginCategory(QStringLiteral("connections"), QStringLiteral("Connections"),
                  QStringLiteral("network-workgroup"), QStyle::SP_DriveNetIcon);
    buildSshPage();
    buildRdpPage();
    buildVncPage();
    buildFtpPage();
    buildRshPage();
    buildXdmcpPage();
    buildDockerPage();
    buildKubernetesPage();
#ifdef Q_OS_WIN
    buildWslPage();
#endif
    endCategory();
    // Shared by several protocols.
    beginCategory(QStringLiteral("network"), QStringLiteral("Network"),
                  QStringLiteral("network-wired"), QStyle::SP_DriveNetIcon);
    buildProxyPage();
    buildSftpPage();
    endCategory();
    beginCategory(QStringLiteral("security"), QStringLiteral("Security & vault"),
                  QStringLiteral("security-high"), QStyle::SP_MessageBoxWarning);
    buildSecurityPage();
    buildKeysPage();
    buildKerberosPage();
    endCategory();
    beginCategory(QStringLiteral("tools"), QStringLiteral("Tools"),
                  QStringLiteral("applications-utilities"), QStyle::SP_FileDialogDetailedView);
    buildRunCommandPage();
    buildHttpRequestPage();
    buildScriptsPage();
    endCategory();
    beginCategory(QStringLiteral("data"), QStringLiteral("Data & backup"),
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

// A setting kept on another page, related to this one: its name and a
// button that goes there. The setting itself stays in one place.
void PreferencesWindow::addLinkRow(SettingsCard *card, const QString &title, const QString &description,
                                   const QString &where, const QString &pageId, const QString &keywords,
                                   const QStringList &targets)
{
    auto *go = new QPushButton(QString(where).replace(QLatin1Char('&'), QStringLiteral("&&"))
                               + QStringLiteral(" ›"));
    go->setFlat(true);
    go->setCursor(Qt::PointingHandCursor);
    go->setToolTip(QStringLiteral("Go to %1").arg(where));
    connect(go, &QPushButton::clicked, this, [this, pageId, targets] { showPage(pageId, targets); });
    card->addRow(title, description, go, keywords);
}

void PreferencesWindow::showPage(const QString &id, const QStringList &targets)
{
    QWidget *shown = nullptr;
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
        shown = category.pages.value(tab);
    }
    show();
    raise();
    activateWindow();
    if (shown && !targets.isEmpty())
        // Once the page is laid out, so it scrolls to the right place.
        QTimer::singleShot(0, shown, [this, shown, targets] { markTargets(shown, targets); });
}

// Scrolls to the rows (or cards) with these titles and tints them for a
// moment, so a link's destination is seen at once.
void PreferencesWindow::markTargets(QWidget *page, const QStringList &targets)
{
    auto *scroll = qobject_cast<QScrollArea *>(page);
    QList<QWidget *> marked;
    for (auto *label : page->findChildren<QLabel *>()) {
        if (!targets.contains(label->text()))
            continue;
        QWidget *target = label;
        while (target && target->objectName() != QStringLiteral("settingsRow")
               && target->objectName() != QStringLiteral("settingsCard"))
            target = target->parentWidget();
        if (target && !marked.contains(target))
            marked.append(target);
    }
    if (marked.isEmpty())
        return;
    // The first one near the top, with the others below it in view.
    if (scroll && scroll->widget())
        scroll->verticalScrollBar()->setValue(marked.first()->mapTo(scroll->widget(), QPoint()).y() - 40);
    QColor tint = palette().color(QPalette::Highlight);
    tint.setAlpha(70);
    const auto style = QStringLiteral("#%1 { background-color: rgba(%2, %3, %4, %5); border-radius: 6px; }");
    for (auto *target : std::as_const(marked)) {
        const QPointer<QWidget> guarded(target);
        const auto before = target->styleSheet();
        target->setStyleSheet(style.arg(target->objectName()).arg(tint.red()).arg(tint.green())
                                  .arg(tint.blue()).arg(tint.alpha()));
        QTimer::singleShot(1800, this, [guarded, before] {
            if (guarded)
                guarded->setStyleSheet(before);
        });
    }
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
        // A tab's single & would underline the next letter (a shortcut).
        category.tabs->addTab(page, QString(title).replace(QLatin1Char('&'), QStringLiteral("&&")));
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
// One page for the whole appearance: the looks, and below them what a look
// sets (interface and terminal), changed by hand there.
SettingsPage *PreferencesWindow::buildLooksPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("appearance-looks"), QStringLiteral("Looks"),
                         QStringLiteral("preferences-desktop-theme"), QStyle::SP_DesktopIcon);
    auto *card = page->addCard(QStringLiteral("Looks"));
    // Faded, like the explanations under every setting.
    auto *intro = secondaryLabel(QStringLiteral(
        "A look sets the interface colours, style, icons and fonts, the window border and the "
        "terminal colours and font together; all of them are below. Changing one by hand makes "
        "the appearance Custom: save it as a new look, into one of your own looks, or go back "
        "to the look it came from."), nullptr);
    card->addWideRow(intro, QStringLiteral("look theme preset"));

    auto *content = new QWidget;
    auto *layout = new QHBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto *list = new QListWidget(content);
    list->setMinimumHeight(300);
    list->setItemDelegate(new LookDelegate(list));
    list->setSpacing(2);
    layout->addWidget(list, 1);
    auto *buttons = new QVBoxLayout;
    auto *apply = new QPushButton(QStringLiteral("Apply"), content);
    auto *saveNew = new QPushButton(QStringLiteral("Save as New Look…"), content);
    auto *saveInto = new QPushButton(content);
    auto *revert = new QPushButton(content);
    auto *duplicate = new QPushButton(QStringLiteral("Duplicate"), content);
    auto *rename = new QPushButton(QStringLiteral("Rename…"), content);
    auto *remove = new QPushButton(QStringLiteral("Delete"), content);
    auto *exportButton = new QPushButton(QStringLiteral("Export…"), content);
    auto *importButton = new QPushButton(QStringLiteral("Import…"), content);
    saveInto->setToolTip(QStringLiteral("Replaces the selected look of yours with the current appearance."));
    revert->setToolTip(QStringLiteral("Undoes the changes made by hand since that look."));
    exportButton->setToolTip(QStringLiteral("Saves the selected look to a file, to use it on another "
                                            "computer or share it."));
    for (auto *button : {apply, saveNew, saveInto, revert})
        buttons->addWidget(button);
    buttons->addSpacing(8);
    for (auto *button : {duplicate, rename, remove})
        buttons->addWidget(button);
    buttons->addSpacing(8);
    for (auto *button : {exportButton, importButton})
        buttons->addWidget(button);
    buttons->addStretch();
    layout->addLayout(buttons);
    card->addWideRow(content, QStringLiteral("look apply save rename delete custom duplicate export import revert"));
    auto *current = new QLabel;
    current->setWordWrap(true);
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
    const auto lookById = [mainWindow](const QString &id) -> std::optional<Look> {
        for (const auto &look : mainWindow->looks()) {
            if (look.id == id)
                return look;
        }
        return std::nullopt;
    };
    // Fills the list; the look in use is marked and selected.
    const auto refresh = [mainWindow, list, current, describe, lookById] {
        const QString selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString()
                                                     : QString();
        const QString active = mainWindow->matchingLookId();
        const QString base = mainWindow->baseLookId();
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
            // A Kvantum look with the style's own colours shows the theme's
            // colours, not the ones in use now.
            const auto own = look.colorTheme == QStringLiteral("system") && KvantumThemes::isKvantumKey(look.style)
                                 ? KvantumThemes::colours(look.style)
                                 : QList<QColor>();
            if (own.size() == 4) {
                for (const auto &colour : own)
                    colors << colour;
            } else {
                for (const auto &variant : variants) {
                    const QPalette colours = mainWindow->paletteFor(variant, look.customColors);
                    colors << colours.color(QPalette::Window) << colours.color(QPalette::Base)
                           << colours.color(QPalette::Text) << colours.color(QPalette::Highlight);
                }
            }
            item->setData(LookDelegate::ColorsRole, colors);
            if (look.id == (selected.isEmpty() ? (active.isEmpty() ? base : active) : selected))
                list->setCurrentItem(item);
        }
        if (const auto look = lookById(active)) {
            current->setText(QStringLiteral("In use: <b>%1</b>").arg(look->name.toHtmlEscaped()));
        } else if (const auto from = lookById(base)) {
            current->setText(QStringLiteral("In use: <b>Custom</b>, from \"%1\" with changed %2.")
                                 .arg(from->name.toHtmlEscaped(),
                                      lookDifferences(*from, mainWindow->currentLook())
                                          .join(QStringLiteral(", ")).toHtmlEscaped()));
        } else {
            current->setText(QStringLiteral("In use: <b>Custom</b>"));
        }
    };
    const auto selectedLook = [list, lookById]() -> std::optional<Look> {
        return list->currentItem() ? lookById(list->currentItem()->data(Qt::UserRole).toString())
                                   : std::nullopt;
    };
    const auto ownCount = [mainWindow] {
        int own = 0;
        for (const auto &each : mainWindow->looks())
            own += each.isBuiltIn() ? 0 : 1;
        return own;
    };
    const auto updateButtons = [=] {
        const auto look = selectedLook();
        const Look now = mainWindow->currentLook();
        apply->setEnabled(look.has_value());
        rename->setEnabled(look && !look->isBuiltIn());
        remove->setEnabled(look && !look->isBuiltIn());
        exportButton->setEnabled(look.has_value());
        const bool room = ownCount() < MaximumUserLooks;
        const QString full = QStringLiteral("Up to %1 looks of your own; delete one first.").arg(MaximumUserLooks);
        for (auto *button : {saveNew, duplicate, importButton}) {
            button->setEnabled(room && (button != duplicate || look.has_value()));
            button->setToolTip(room ? QString() : full);
        }
        const bool own = look && !look->isBuiltIn();
        // One width whatever the name (the confirmation names the look).
        saveInto->setText(QStringLiteral("Save into Selected Look…"));
        saveInto->setToolTip(own ? QStringLiteral("Replaces \"%1\" with the current appearance.").arg(look->name)
                                 : QStringLiteral("Select one of your own looks first."));
        saveInto->setEnabled(own && !look->sameAppearance(now));
        const auto base = lookById(mainWindow->baseLookId());
        revert->setText(QStringLiteral("Revert Unsaved Changes"));
        revert->setToolTip(base ? QStringLiteral("Undoes the changes made by hand since \"%1\" was applied.").arg(base->name)
                                : QStringLiteral("Undoes the changes made by hand since a look was applied."));
        revert->setEnabled(base && !base->sameAppearance(now));
    };
    const auto askName = [this](const QString &title, const QString &name) {
        bool ok = false;
        const QString text = QInputDialog::getText(this, title, QStringLiteral("Name of the look:"),
                                                   QLineEdit::Normal, name, &ok).trimmed();
        return ok ? text : QString();
    };
    refresh();
    updateButtons();
    connect(list, &QListWidget::currentItemChanged, this, updateButtons);
    connect(list, &QListWidget::itemDoubleClicked, apply, &QPushButton::click);
    connect(apply, &QPushButton::clicked, this, [mainWindow, selectedLook] {
        if (const auto look = selectedLook())
            mainWindow->applyLook(*look);
    });
    connect(saveNew, &QPushButton::clicked, this, [this, mainWindow, askName] {
        const QString name = askName(QStringLiteral("Save as New Look"), QString());
        if (!name.isEmpty() && !mainWindow->saveCurrentLookAs(name))
            QMessageBox::information(this, QStringLiteral("Save Look"),
                                     QStringLiteral("You can keep up to %1 looks of your own. "
                                                    "Delete one first.").arg(MaximumUserLooks));
    });
    connect(saveInto, &QPushButton::clicked, this, [this, mainWindow, selectedLook] {
        const auto look = selectedLook();
        if (!look || look->isBuiltIn())
            return;
        // Named and with what changes, so the wrong look is not replaced by mistake.
        QMessageBox confirm(this);
        confirm.setIcon(QMessageBox::Question);
        confirm.setWindowTitle(QStringLiteral("Save into Look"));
        confirm.setText(QStringLiteral("Replace your look \"%1\" with the current appearance?").arg(look->name));
        confirm.setInformativeText(QStringLiteral("Changes: %1. This cannot be undone.")
                                       .arg(lookDifferences(*look, mainWindow->currentLook())
                                                .join(QStringLiteral(", "))));
        auto *replace = confirm.addButton(QStringLiteral("Replace \"%1\"").arg(look->name),
                                          QMessageBox::AcceptRole);
        confirm.addButton(QMessageBox::Cancel);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.exec();
        if (confirm.clickedButton() == replace)
            mainWindow->overwriteLook(look->id);
    });
    connect(revert, &QPushButton::clicked, this, [mainWindow, lookById] {
        if (const auto look = lookById(mainWindow->baseLookId()))
            mainWindow->applyLook(*look);
    });
    connect(duplicate, &QPushButton::clicked, this, [mainWindow, selectedLook, askName] {
        auto look = selectedLook();
        if (!look)
            return;
        const QString name = askName(QStringLiteral("Duplicate Look"), QStringLiteral("%1 (copy)").arg(look->name));
        if (name.isEmpty())
            return;
        look->name = name;
        mainWindow->addLook(*look);
    });
    connect(rename, &QPushButton::clicked, this, [mainWindow, selectedLook, askName] {
        const auto look = selectedLook();
        if (!look || look->isBuiltIn())
            return;
        const QString name = askName(QStringLiteral("Rename Look"), look->name);
        if (!name.isEmpty())
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
    connect(exportButton, &QPushButton::clicked, this, [this, selectedLook] {
        const auto look = selectedLook();
        if (!look)
            return;
        const QString path = FileDialogs::saveFileName(
            this, QStringLiteral("Export Look"),
            QDir::home().filePath(look->name + QStringLiteral(".opsremote-look.yaml")),
            QStringLiteral("OpsRemote looks (*.opsremote-look.yaml *.yaml)"));
        if (!path.isEmpty() && !exportLook(*look, path))
            QMessageBox::warning(this, QStringLiteral("Export Look"),
                                 QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(path)));
    });
    connect(importButton, &QPushButton::clicked, this, [this, mainWindow] {
        const QString path = FileDialogs::openFileName(
            this, QStringLiteral("Import Look"), QDir::homePath(),
            QStringLiteral("OpsRemote looks (*.opsremote-look.yaml *.yaml)"));
        if (path.isEmpty())
            return;
        QString error;
        const auto look = importLook(path, &error);
        if (!look)
            QMessageBox::warning(this, QStringLiteral("Import Look"), error);
        else if (!mainWindow->addLook(*look))
            QMessageBox::information(this, QStringLiteral("Import Look"),
                                     QStringLiteral("You can keep up to %1 looks of your own. "
                                                    "Delete one first.").arg(MaximumUserLooks));
    });
    // Settings changed one by one only move the "in use" mark.
    connect(mainWindow, &MainWindow::appearanceChanged, list, [refresh, updateButtons] {
        refresh();
        updateButtons();
    });
    return page;
}

void PreferencesWindow::buildAppearancePage()
{
    auto *mainWindow = m_mainWindow;
    // What a look sets, on the Looks page below the looks themselves.
    auto *page = buildLooksPage();

    // Colours and style
    auto *colours = page->addCard(QStringLiteral("Interface: colours & style"));
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
    auto *iconsAndText = page->addCard(QStringLiteral("Interface: icons & text"));
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
                                        "font, below."),
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
    auto *terminal = page->addCard(QStringLiteral("Terminal: colours & font"));
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

    auto *display = page->addCard(QStringLiteral("Cursor & display"));
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

    auto *mouse = page->addCard(QStringLiteral("Mouse, copy & paste"));
    addLinkRow(mouse, QStringLiteral("Selecting, copying, pasting, links"),
               QStringLiteral("The same in terminals and every other place of OpsRemote, on every system."),
               QStringLiteral("Workspace → Mouse & clipboard"), QStringLiteral("mouse"),
               QStringLiteral("copy on select paste middle right click word characters links multiline"));

    auto *dropping = page->addCard(QStringLiteral("Dropping files"));
    auto *uploadDropped = toggle(current.uploadDroppedFiles);
    dropping->addRow(QStringLiteral("Upload files dropped on a terminal"),
                     QStringLiteral("Files and folders dropped on a terminal go into the folder of the "
                                    "program in it, found when you drop them, after asking. Off, or "
                                    "with Shift held: their paths are typed, as before. Saved sessions "
                                    "can turn it on or off for themselves."),
                     uploadDropped, QStringLiteral("drag drop upload files folder paths"));
    connectToggle(uploadDropped, &TerminalBehaviour::uploadDroppedFiles);

    auto *notifications = page->addCard(QStringLiteral("Notifications"));
    auto *bell = choiceBox({{QStringLiteral("visual"), QStringLiteral("Flash the terminal")},
                            {QStringLiteral("sound"), QStringLiteral("Play the system sound")},
                            {QStringLiteral("none"), QStringLiteral("Nothing")}},
                           current.bell);
    notifications->addRow(QStringLiteral("Terminal bell"),
                          QStringLiteral("When a program rings the bell (for example Tab completion "
                                         "with nothing to complete). Flash: the terminal dims for a "
                                         "moment. A background tab is also marked, as for new output."),
                          bell, QStringLiteral("beep alert flash visual sound"));
    connectChoice(bell, &TerminalBehaviour::bell);
}

// On Workspace → Tabs; the settings are the terminals' (settings.yaml "behaviour:").
void PreferencesWindow::buildTabActivityCard(SettingsPage *page)
{
    auto *mainWindow = m_mainWindow;
    auto update = [mainWindow](auto change) {
        auto behaviour = mainWindow->terminalBehaviour();
        change(behaviour);
        mainWindow->setTerminalBehaviour(behaviour);
    };
    const auto current = mainWindow->terminalBehaviour();

    auto *card = page->addCard(QStringLiteral("Activity in background tabs"));
    // A dot as the tabs show it, beside the choice.
    auto *preview = new ActivityDot;
    preview->setToolTip(QStringLiteral("How a marked tab looks"));
    auto *style = choiceBox({{QStringLiteral("pulse"), QStringLiteral("Pulsing dot")},
                             {QStringLiteral("steady"), QStringLiteral("Steady dot")},
                             {QStringLiteral("text"), QStringLiteral("Title colour")},
                             {QStringLiteral("off"), QStringLiteral("Off")}},
                            current.markActivity ? current.activityStyle : QStringLiteral("off"));
    auto *styleRow = new QWidget;
    auto *styleLayout = new QHBoxLayout(styleRow);
    styleLayout->setContentsMargins(0, 0, 0, 0);
    styleLayout->addWidget(preview);
    styleLayout->addWidget(style);
    card->addRow(QStringLiteral("Mark background tabs"),
                 QStringLiteral("A tab that prints something while another is shown is marked until "
                                "you open it. Title colour does not show with Kvantum styles: they "
                                "colour tab titles themselves."),
                 styleRow, QStringLiteral("activity monitor tab highlight blink pulse dot"));

    // One colour: Automatic or Custom, and a swatch of the colour in use.
    // The swatch chooses a colour only with Custom; with Automatic it is
    // greyed out but still shows what OpsRemote picked.
    struct ColourRow
    {
        QWidget *row;
        ColourSwatch *swatch;
        QComboBox *source;
    };
    auto addColour = [&](const QString &label, const QString &description,
                         QColor TerminalBehaviour::*field) {
        auto *swatch = new ColourSwatch;
        auto *source = choiceBox({{QStringLiteral("auto"), QStringLiteral("Automatic")},
                                  {QStringLiteral("custom"), QStringLiteral("Custom")}},
                                 (current.*field).isValid() ? QStringLiteral("custom") : QStringLiteral("auto"));
        auto *row = new QWidget;
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(source);
        layout->addWidget(swatch);
        card->addRow(label, description, row, QStringLiteral("activity tab colour color"));
        connect(swatch, &QAbstractButton::clicked, this, [this, update, swatch, field] {
            const auto chosen = QColorDialog::getColor(swatch->colour(), this);
            if (chosen.isValid())
                update([&](TerminalBehaviour &b) { b.*field = chosen; });
        });
        // Custom starts from the colour in use; the swatch then changes it.
        connect(source, &QComboBox::activated, this, [update, source, swatch, field] {
            const bool custom = source->currentData().toString() == QStringLiteral("custom");
            const auto colour = custom ? swatch->colour() : QColor();
            update([&](TerminalBehaviour &b) { b.*field = colour; });
        });
        return ColourRow{row, swatch, source};
    };
    const auto first = addColour(QStringLiteral("Dot colour"),
                                 QStringLiteral("Also the title's colour with Title colour. "
                                                "Automatic: the accent colour of your look, or "
                                                "another one if the accent would be hard to see on "
                                                "the tab bar."),
                                 &TerminalBehaviour::activityColor);
    const auto second = addColour(QStringLiteral("Pulse colour"),
                                  QStringLiteral("The colour the dot fades into while it pulses. "
                                                 "Automatic: a faint dot colour."),
                                  &TerminalBehaviour::activityPulseColor);

    // Swatches, preview and what can be changed follow the settings.
    auto refresh = [mainWindow, preview, style, first, second] {
        const auto behaviour = mainWindow->terminalBehaviour();
        const auto [colour, pulse] = mainWindow->activityColors();
        const auto shown = behaviour.markActivity ? behaviour.activityStyle : QStringLiteral("off");
        for (const auto &[row, value, own] : {std::tuple{first, colour, behaviour.activityColor},
                                              std::tuple{second, pulse, behaviour.activityPulseColor}}) {
            row.swatch->setColour(value);
            row.swatch->setEnabled(own.isValid());
            row.swatch->setToolTip(own.isValid() ? QStringLiteral("Choose a colour")
                                                 : QStringLiteral("Chosen automatically; pick Custom "
                                                                  "to choose your own"));
            row.source->setCurrentIndex(row.source->findData(own.isValid() ? QStringLiteral("custom")
                                                                            : QStringLiteral("auto")));
        }
        first.row->setEnabled(shown != QStringLiteral("off"));
        second.row->setEnabled(shown == QStringLiteral("pulse"));
        preview->setColors(colour, pulse);
        preview->setPulsing(shown == QStringLiteral("pulse"));
        preview->setActive(shown == QStringLiteral("pulse") || shown == QStringLiteral("steady"));
    };
    refresh();
    connect(style, &QComboBox::activated, this, [update, style] {
        const auto chosen = style->currentData().toString();
        update([&](TerminalBehaviour &b) {
            b.markActivity = chosen != QStringLiteral("off");
            if (b.markActivity)
                b.activityStyle = chosen;
        });
    });
    // Also after a change of look, which changes the colours from the look.
    connect(mainWindow, &MainWindow::terminalBehaviourChanged, preview, refresh);
    connect(mainWindow, &MainWindow::appearanceChanged, preview,
            [preview, refresh] { QTimer::singleShot(0, preview, refresh); });
}

void PreferencesWindow::buildStartupPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("startup"), QStringLiteral("Startup & exit"),
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

    auto *closing = page->addCard(QStringLiteral("When OpsRemote closes"));
    auto *exit = toggle(current.confirmExit);
    closing->addRow(QStringLiteral("Ask before exiting with open terminals"), QString(), exit,
                    QStringLiteral("confirm quit close"));
    connectToggle(exit, &StartupPreferences::confirmExit);

    page = addPage(QStringLiteral("tabs"), QStringLiteral("Tabs"), QStringLiteral("tab-new"),
                   QStyle::SP_FileDialogDetailedView);
    auto *tabs = page->addCard(QStringLiteral("General"));
    auto *title = choiceBox({{QStringLiteral("name"), QStringLiteral("Session name")},
                             {QStringLiteral("userhost"), QStringLiteral("user@host")},
                             {QStringLiteral("both"), QStringLiteral("Session name (user@host)")}},
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
    auto *duplicates = toggle(current.allowDuplicateSessionTabs);
    tabs->addRow(QStringLiteral("Allow the same saved session in multiple tabs"),
                 QStringLiteral("Off: opening an already-open saved session selects its tab. "
                                "On: every Open command creates an independent connection. "
                                "Applies to every session type."),
                 duplicates, QStringLiteral("duplicate same session multiple tabs connections"));
    connectToggle(duplicates, &StartupPreferences::allowDuplicateSessionTabs);

    auto *closeConnected = toggle(current.confirmCloseConnected);
    tabs->addRow(QStringLiteral("Ask before closing a connected SSH tab"), QString(),
                 closeConnected, QStringLiteral("confirm disconnect"));
    connectToggle(closeConnected, &StartupPreferences::confirmCloseConnected);
    buildTabActivityCard(page);

    buildMouseClipboardPage();
}

// Workspace → Mouse and clipboard: selecting, copying and pasting, the same
// in terminals and text fields, on every system (clipboard.h).
void PreferencesWindow::buildMouseClipboardPage()
{
    auto *mainWindow = m_mainWindow;
    auto *page = addPage(QStringLiteral("mouse"), QStringLiteral("Mouse & clipboard"),
                         QStringLiteral("input-mouse"), QStyle::SP_ArrowUp);
    auto update = [mainWindow](auto change) {
        auto preferences = mainWindow->mouseClipboardPreferences();
        change(preferences);
        mainWindow->setMouseClipboardPreferences(preferences);
    };
    auto connectToggle = [this, update](QCheckBox *box, bool MouseClipboardPreferences::*field) {
        connect(box, &QCheckBox::toggled, this, [update, field](bool on) {
            update([&](MouseClipboardPreferences &p) { p.*field = on; });
        });
    };
    auto connectChoice = [this, update](QComboBox *box, QString MouseClipboardPreferences::*field) {
        connect(box, &QComboBox::activated, this, [update, box, field] {
            update([&](MouseClipboardPreferences &p) { p.*field = box->currentData().toString(); });
        });
    };
    const auto current = mainWindow->mouseClipboardPreferences();

    auto *copying = page->addCard(QStringLiteral("Selecting & copying"));
    auto *doubleClick = choiceBox({{QStringLiteral("word-characters"), QStringLiteral("Words with these characters too")},
                                   {QStringLiteral("letters"), QStringLiteral("Letters and digits only")}},
                                  current.doubleClick);
    copying->addRow(QStringLiteral("Double-click selects"),
                    QStringLiteral("In terminals and text fields. With the characters below, paths, "
                                   "web addresses and user@host are one word."),
                    doubleClick, QStringLiteral("double click word selection"));
    connectChoice(doubleClick, &MouseClipboardPreferences::doubleClick);
    auto *wordChars = new QLineEdit(current.wordCharacters);
    wordChars->setMaximumWidth(200);
    copying->addRow(QStringLiteral("Word characters"),
                    QStringLiteral("Characters a double-click keeps in a word, besides letters and digits."),
                    wordChars, QStringLiteral("double click selection path url"));
    connect(wordChars, &QLineEdit::editingFinished, this, [update, wordChars] {
        update([&](MouseClipboardPreferences &p) { p.wordCharacters = wordChars->text(); });
    });
    wordChars->setEnabled(current.doubleClick != QStringLiteral("letters"));
    connect(doubleClick, &QComboBox::activated, wordChars, [doubleClick, wordChars] {
        wordChars->setEnabled(doubleClick->currentData().toString() != QStringLiteral("letters"));
    });
    auto *copyOnSelect = toggle(current.copyOnSelect);
    copying->addRow(QStringLiteral("Copy on select"),
                    QStringLiteral("Selected text also goes to the clipboard when you finish selecting, "
                                   "like PuTTY. Not from password fields."),
                    copyOnSelect, QStringLiteral("clipboard selection putty"));
    connectToggle(copyOnSelect, &MouseClipboardPreferences::copyOnSelect);

    auto *pasting = page->addCard(QStringLiteral("Pasting"));
    auto *paste = choiceBox({{QStringLiteral("newest"), QStringLiteral("The newest")},
                             {QStringLiteral("clipboard"), QStringLiteral("Always the clipboard")},
                             {QStringLiteral("system"), QStringLiteral("System standard")}},
                            current.paste);
    pasting->addRow(QStringLiteral("Paste"),
                    QStringLiteral("What every paste gives (middle-click, right-click, Ctrl+Shift+V, the "
                                   "menu), in terminals and text fields. The newest: your last selection "
                                   "in OpsRemote or your last copy in any program, whichever came last. "
                                   "System standard: on Linux middle-click pastes the selection, the rest "
                                   "the clipboard."),
                    paste, QStringLiteral("clipboard selection primary newest middle"));
    connectChoice(paste, &MouseClipboardPreferences::paste);
#ifndef Q_OS_WIN
    auto *otherPrograms = toggle(current.otherProgramsSelection);
    pasting->addRow(QStringLiteral("Selections in other programs count too"),
                    QStringLiteral("With The newest: text only selected (not copied) in another program "
                                   "counts as well. Off: reading a web page does not change what you paste."),
                    otherPrograms, QStringLiteral("primary selection linux browser"));
    connectToggle(otherPrograms, &MouseClipboardPreferences::otherProgramsSelection);
    otherPrograms->setEnabled(current.paste == QStringLiteral("newest"));
    connect(paste, &QComboBox::activated, otherPrograms, [paste, otherPrograms] {
        otherPrograms->setEnabled(paste->currentData().toString() == QStringLiteral("newest"));
    });
#endif

    auto *buttons = page->addCard(QStringLiteral("Mouse buttons"));
    auto *rightClick = choiceBox({{QStringLiteral("menu"), QStringLiteral("Show menu")},
                                  {QStringLiteral("paste"), QStringLiteral("Paste")},
                                  {QStringLiteral("none"), QStringLiteral("Nothing")}},
                                 current.rightClick);
    buttons->addRow(QStringLiteral("Right-click"),
                    QStringLiteral("In terminals and text fields. Shift+right-click does the other: the "
                                   "menu, or with the menu set, a paste."),
                    rightClick, QStringLiteral("context menu mouse button putty mobaxterm"));
    connectChoice(rightClick, &MouseClipboardPreferences::rightClick);
    auto *middleClick = choiceBox({{QStringLiteral("paste"), QStringLiteral("Paste")},
                                   {QStringLiteral("menu"), QStringLiteral("Show menu")},
                                   {QStringLiteral("none"), QStringLiteral("Nothing")}},
                                  current.middleClick);
    buttons->addRow(QStringLiteral("Middle-click"),
                    QStringLiteral("In terminals and text fields. Programs that use the mouse (vim, mc) "
                                   "still get the click unless Shift is held."),
                    middleClick, QStringLiteral("middle mouse button wheel paste"));
    connectChoice(middleClick, &MouseClipboardPreferences::middleClick);
    auto *closesTab = toggle(current.middleClickClosesTab);
    buttons->addRow(QStringLiteral("Middle-click closes a tab"), QString(), closesTab,
                    QStringLiteral("mouse wheel button tab"));
    connectToggle(closesTab, &MouseClipboardPreferences::middleClickClosesTab);
    auto *wheel = choiceBox({{QStringLiteral("focused"), QStringLiteral("Only after clicking the field")},
                             {QStringLiteral("always"), QStringLiteral("Always")},
                             {QStringLiteral("never"), QStringLiteral("Never")}},
                            current.wheelOnFields);
    buttons->addRow(QStringLiteral("Mouse wheel changes drop-down lists and number fields"),
                    QStringLiteral("In Preferences, the session window, and every other window. Only "
                                   "after clicking: scrolling over a field that is not selected scrolls "
                                   "the page, so a choice does not change by accident. Never: change "
                                   "them with a click or the keyboard."),
                    wheel, QStringLiteral("scroll wheel combo box spin accident change select"));
    connectChoice(wheel, &MouseClipboardPreferences::wheelOnFields);

    auto *lines = page->addCard(QStringLiteral("Pasting several lines"));
    auto *confirmPaste = toggle(current.confirmMultilinePaste);
    lines->addRow(QStringLiteral("Confirm multi-line paste"),
                  QStringLiteral("Asks first, so a pasted script does not run line by line by accident. "
                                 "Also for commands of several lines sent from the Commands tab. Saved "
                                 "sessions can turn it on or off for themselves."),
                  confirmPaste, QStringLiteral("safety clipboard confirm multiline commands snippets"));
    connectToggle(confirmPaste, &MouseClipboardPreferences::confirmMultilinePaste);
    auto *editPaste = toggle(current.editMultilinePaste);
    lines->addRow(QStringLiteral("Start in editing mode"),
                  QStringLiteral("The window that asks before several lines opens with its text "
                                 "editable, so you can change it before it is pasted or sent."),
                  editPaste, QStringLiteral("edit change multiline paste confirm commands"));
    connectToggle(editPaste, &MouseClipboardPreferences::editMultilinePaste);
    // Editing happens in the asking window: only while it asks.
    editPaste->setEnabled(current.confirmMultilinePaste);
    connect(confirmPaste, &QCheckBox::toggled, editPaste, &QWidget::setEnabled);
    auto *trim = toggle(current.trimPastedNewlines);
    lines->addRow(QStringLiteral("Remove trailing newlines when pasting"),
                  QStringLiteral("A pasted command waits for Enter instead of running."), trim,
                  QStringLiteral("clipboard"));
    connectToggle(trim, &MouseClipboardPreferences::trimPastedNewlines);

    auto *linksCard = page->addCard(QStringLiteral("Links"));
    auto *links = choiceBox({{QStringLiteral("ctrl-click"), QStringLiteral("Open with Ctrl+click or the menu")},
                             {QStringLiteral("menu"), QStringLiteral("Open from the menu only")},
                             {QStringLiteral("off"), QStringLiteral("Off")}},
                            current.links);
    linksCard->addRow(QStringLiteral("Links in terminal output"),
                      QStringLiteral("Web addresses and links programs print are underlined when the "
                                     "mouse is over them, and open in your browser. Off: not recognised. "
                                     "Saved sessions can turn them on or off for themselves."),
                      links, QStringLiteral("url hyperlink browser osc8 ctrl click open"));
    connectChoice(links, &MouseClipboardPreferences::links);

    auto *remote = page->addCard(QStringLiteral("Remote desktops clipboard & file transfers"));
    addLinkRow(remote, QStringLiteral("Clipboard of RDP sessions"),
               QStringLiteral("Sharing clipboard text and files with the remote computer."),
               QStringLiteral("Connections → RDP"), QStringLiteral("rdp"),
               QStringLiteral("rdp clipboard share files"),
               {QStringLiteral("Share clipboard text"), QStringLiteral("Share files through the clipboard")});
    addLinkRow(remote, QStringLiteral("Clipboard of VNC sessions"),
               QStringLiteral("Sharing clipboard text with the remote computer."),
               QStringLiteral("Connections → VNC"), QStringLiteral("vnc"),
               QStringLiteral("vnc clipboard share"), {QStringLiteral("Share clipboard text")});
    addLinkRow(remote, QStringLiteral("Copying files in the Files panel"),
               QStringLiteral("Copy, paste and drag files between this computer and a server: where "
                              "downloads go, uploads under a temporary name, and what happens when a "
                              "file already exists."),
               QStringLiteral("Network → Files & transfers"), QStringLiteral("sftp"),
               QStringLiteral("files copy paste drag drop upload download sftp ftp"),
               {QStringLiteral("Downloads"), QStringLiteral("Uploads"),
                QStringLiteral("When a file already exists")});
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
        const auto chosen = folder ? FileDialogs::existingDirectory(widget, QString(), start)
                                   : FileDialogs::openFileName(widget, QString(), start);
        if (chosen.isEmpty())
            return;
        edit->setText(QDir::toNativeSeparators(chosen));
        changed(chosen);
    });
    return widget;
}

// "Wait before reconnecting" and "Tries" (reconnect.h), the same rows on every
// session type's tab, greyed out while its "Reconnect automatically" is off.
void addReconnectTimingRows(SettingsCard *card, QCheckBox *enabled, int delay, int attempts,
                            std::function<void(int)> setDelay, std::function<void(int)> setAttempts)
{
    auto *delayBox = reconnectDelayBox(delay);
    card->addRow(QStringLiteral("Wait before reconnecting"), QString(), delayBox,
                 QStringLiteral("reconnect retry delay wait"));
    auto *attemptsBox = reconnectAttemptsBox(attempts);
    card->addRow(QStringLiteral("Tries"),
                 QStringLiteral("How many times it tries before giving up; 0 keeps trying."),
                 attemptsBox, QStringLiteral("reconnect retry attempts tries forever"));
    QObject::connect(delayBox, &QSpinBox::valueChanged, card, std::move(setDelay));
    QObject::connect(attemptsBox, &QSpinBox::valueChanged, card, std::move(setAttempts));
    for (QWidget *box : {static_cast<QWidget *>(delayBox), static_cast<QWidget *>(attemptsBox)}) {
        box->setEnabled(enabled->isChecked());
        QObject::connect(enabled, &QCheckBox::toggled, box, &QWidget::setEnabled);
    }
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
    showRows(table); // five rows, more scroll
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
    addReconnectTimingRows(reconnect, autoReconnect, current.reconnectDelay, current.reconnectAttempts,
                           [update](int v) { update([v](SshPreferences &p) { p.reconnectDelay = v; }); },
                           [update](int v) { update([v](SshPreferences &p) { p.reconnectAttempts = v; }); });

    auto *browser = page->addCard(QStringLiteral("File browser"));
    auto *follow = toggle(current.followTerminalFolder);
    browser->addRow(QStringLiteral("Follow the terminal's folder"),
                    QStringLiteral("Off by default. Sets the default for new SSH tabs: after each "
                                   "command, the Files pane can show the remote shell's folder. "
                                   "Saved sessions can explicitly enable or disable this. "
                                   "Right-click a terminal or its Files pane to change the current tab."),
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

// Login scripts and triggers: on or off, and triggers for every tab.
void PreferencesWindow::buildAutomationPage()
{
    auto *page = addPage(QStringLiteral("automation"), QStringLiteral("Automation"),
                         QStringLiteral("system-run"), QStyle::SP_MediaPlay);
    const auto current = loadAutomationPreferences();
    // Saved at once, like the other pages; tabs opened later use it.
    const auto update = [](const std::function<void(AutomationPreferences &)> &change) {
        auto value = loadAutomationPreferences();
        change(value);
        saveAutomationPreferences(value);
    };
    auto *scripts = page->addCard(QStringLiteral("Login scripts"));
    auto *scriptsOn = toggle(current.loginScripts);
    scripts->addRow(QStringLiteral("Run login scripts"),
                    QStringLiteral("After connecting, a session's login script waits for each text "
                                   "(a prompt, a menu) and types the answer. Set them on the "
                                   "Automation tab of a session or folder."),
                    scriptsOn, QStringLiteral("login script expect enable password menu"));
    connect(scriptsOn, &QCheckBox::toggled, this,
            [update](bool on) { update([on](AutomationPreferences &v) { v.loginScripts = on; }); });
    auto *timeout = spinBox(1, 3600, current.stepTimeout, QStringLiteral(" s"));
    scripts->addRow(QStringLiteral("Wait for each text up to"),
                    QStringLiteral("Steps can set their own time."), timeout,
                    QStringLiteral("timeout"));
    connect(timeout, &QSpinBox::valueChanged, this,
            [update](int seconds) { update([seconds](AutomationPreferences &v) { v.stepTimeout = seconds; }); });
    auto *stop = toggle(current.stopOnTimeout);
    scripts->addRow(QStringLiteral("Stop when a text does not appear"),
                    QStringLiteral("Off: the answer is typed anyway and the script goes on."),
                    stop, QStringLiteral("timeout stop continue"));
    connect(stop, &QCheckBox::toggled, this,
            [update](bool on) { update([on](AutomationPreferences &v) { v.stopOnTimeout = on; }); });

    auto *triggers = page->addCard(QStringLiteral("Triggers"));
    auto *triggersOn = toggle(current.triggers);
    triggers->addRow(QStringLiteral("Use triggers"),
                     QStringLiteral("While a tab is open, text in its output can type an answer, "
                                    "notify you (status bar and taskbar), beep, or mark the tab."),
                     triggersOn, QStringLiteral("trigger watch output notify"));
    connect(triggersOn, &QCheckBox::toggled, this,
            [update](bool on) { update([on](AutomationPreferences &v) { v.triggers = on; }); });
    auto *password = toggle(current.triggersMaySendPassword);
    triggers->addRow(QStringLiteral("Triggers may type the password"),
                     QStringLiteral("Lets a trigger's text use {password}. Off: only login scripts "
                                    "can, which wait for the right moment."),
                     password, QStringLiteral("password secret trigger"));
    connect(password, &QCheckBox::toggled, this, [update](bool on) {
        update([on](AutomationPreferences &v) { v.triggersMaySendPassword = on; });
    });
    auto *global = new TriggerTable(current.globalTriggers);
    triggers->addRow(QStringLiteral("Triggers for every tab"),
                     QStringLiteral("Used in all terminal tabs, before a session's own."),
                     new QWidget, QStringLiteral("global triggers"));
    triggers->addWideRow(global, QStringLiteral("global triggers"));
    connect(global, &TriggerTable::changed, this, [update, global] {
        const auto rules = global->value();
        update([rules](AutomationPreferences &v) { v.globalTriggers = rules; });
    });
}

// Saved scripts (Tools → Scripts).
void PreferencesWindow::buildScriptsPage()
{
    auto *page = addPage(QStringLiteral("scripts"), QStringLiteral("Scripts"),
                         QStringLiteral("text-x-script"), QStyle::SP_FileIcon);
    const auto current = loadScriptPreferences();
    const auto update = [](const std::function<void(ScriptPreferences &)> &change) {
        auto value = loadScriptPreferences();
        change(value);
        saveScriptPreferences(value);
    };
    auto *scripts = page->addCard(QStringLiteral("Saved scripts"));
    auto *enabled = toggle(current.enabled);
    scripts->addRow(QStringLiteral("Use saved scripts"),
                    QStringLiteral("Scripts kept in settings/scripts/, run whole on a server: in a "
                                   "tab (Run Script in the terminal's menu), on many hosts (Run on "
                                   "Hosts), after logging on, or from a command. Edit them in "
                                   "Tools → Scripts."),
                    enabled, QStringLiteral("script scripts bash shell run remote library"));
    connect(enabled, &QCheckBox::toggled, this,
            [update](bool on) { update([on](ScriptPreferences &v) { v.enabled = on; }); });
    auto *interpreter = new QComboBox;
    interpreter->setEditable(true);
    interpreter->addItems({QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("python3"),
                           QStringLiteral("pwsh"), QStringLiteral("perl")});
    interpreter->setEditText(current.interpreter);
    scripts->addRow(QStringLiteral("Interpreter for new scripts"),
                    QStringLiteral("The program on the server that runs a script; each script can "
                                   "choose its own."),
                    interpreter, QStringLiteral("interpreter bash python powershell"));
    connect(interpreter, &QComboBox::currentTextChanged, this, [update](const QString &text) {
        update([text](ScriptPreferences &v) { v.interpreter = text.trimmed(); });
    });
    auto *confirm = spinBox(0, 10000, current.confirmAbove, QStringLiteral(" hosts"), QStringLiteral("Never"));
    scripts->addRow(QStringLiteral("Ask before running on more than"),
                    QStringLiteral("Run on Hosts asks first when a script goes to more hosts than this."),
                    confirm, QStringLiteral("confirm hosts many"));
    connect(confirm, &QSpinBox::valueChanged, this,
            [update](int hosts) { update([hosts](ScriptPreferences &v) { v.confirmAbove = hosts; }); });
    auto *open = new QPushButton(QStringLiteral("Scripts…"));
    connect(open, &QPushButton::clicked, m_mainWindow, [this] { m_mainWindow->showScripts(); });
    scripts->addRow(QStringLiteral("Your scripts"), QStringLiteral("Create, edit, and run them."),
                    open, QStringLiteral("edit scripts"));
}

namespace {
// A container program's path row, with a note when it is missing.
void addProgramRow(SettingsCard *card, const QString &name, const QString &value,
                   std::function<void(const QString &)> save)
{
    auto *picker = pathPicker(value, programPathHint(name), false, std::move(save));
    if (auto *field = picker->findChild<QLineEdit *>()) {
        field->setMinimumWidth(330);
        field->setToolTip(programPathHint(name));
    }
    card->addRow(name, QStringLiteral("Not needed for sessions that run on an SSH server."), picker,
                 QStringLiteral("path program ") + name);
    card->addWideRow(new AvailabilityNote([name] {
        return containerProgram(name).isEmpty()
                   ? QStringLiteral("%1 is not installed here (or not on the PATH).").arg(name)
                   : QString();
    }, {}), name);
}

void updateContainers(const std::function<void(ContainerPreferences &)> &change)
{
    auto value = loadContainerPreferences();
    change(value);
    saveContainerPreferences(value);
}
}

// Docker (and Podman) sessions.
void PreferencesWindow::buildDockerPage()
{
    auto *page = addPage(QStringLiteral("docker"), QStringLiteral("Container (Docker)"),
                         QStringLiteral("package-x-generic"), QStyle::SP_DirIcon);
    const auto current = loadContainerPreferences();
    auto *sessions = page->addCard(QStringLiteral("Docker sessions"));
    auto *enabled = toggle(current.docker.enabled);
    sessions->addRow(QStringLiteral("Docker sessions"),
                     QStringLiteral("A session type that opens a shell in a container (docker exec, "
                                    "or Podman), on this computer or on an SSH server; and Open a "
                                    "Container… on SSH sessions."),
                     enabled, QStringLiteral("docker podman container"));
    connect(enabled, &QCheckBox::toggled, this,
            [](bool on) { updateContainers([on](ContainerPreferences &v) { v.docker.enabled = on; }); });
    auto *shell = new QLineEdit(current.docker.shell);
    shell->setPlaceholderText(QStringLiteral("bash if there is one, else sh"));
    shell->setMinimumWidth(280);
    sessions->addRow(QStringLiteral("Shell in containers"), QStringLiteral("Sessions can choose their own."),
                     shell, QStringLiteral("shell bash sh"));
    connect(shell, &QLineEdit::editingFinished, this, [shell] {
        const auto text = shell->text().trimmed();
        updateContainers([text](ContainerPreferences &v) { v.docker.shell = text; });
    });
    auto *timeout = spinBox(3, 600, current.docker.listTimeout, QStringLiteral(" s"));
    sessions->addRow(QStringLiteral("List timeout"),
                     QStringLiteral("How long listing containers and contexts may take in the session "
                                    "window, before it says it could not."),
                     timeout, QStringLiteral("timeout list containers"));
    connect(timeout, &QSpinBox::valueChanged, this,
            [](int seconds) { updateContainers([seconds](ContainerPreferences &v) { v.docker.listTimeout = seconds; }); });
    auto *programs = page->addCard(QStringLiteral("Programs on this computer"));
    addProgramRow(programs, QStringLiteral("docker"), current.docker.program, [](const QString &path) {
        updateContainers([&](ContainerPreferences &v) { v.docker.program = path; });
    });
    addProgramRow(programs, QStringLiteral("podman"), current.podman, [](const QString &path) {
        updateContainers([&](ContainerPreferences &v) { v.podman = path; });
    });
}

// Kubernetes sessions.
void PreferencesWindow::buildKubernetesPage()
{
    auto *page = addPage(QStringLiteral("kubernetes"), QStringLiteral("Container (Kubernetes)"),
                         QStringLiteral("network-server"), QStyle::SP_DriveNetIcon);
    const auto current = loadContainerPreferences();
    auto *sessions = page->addCard(QStringLiteral("Kubernetes sessions"));
    auto *enabled = toggle(current.kubernetes.enabled);
    sessions->addRow(QStringLiteral("Kubernetes sessions"),
                     QStringLiteral("A session type that opens a shell in a pod (kubectl exec): "
                                    "context, namespace, pod and container, on this computer or on an "
                                    "SSH server."),
                     enabled, QStringLiteral("kubernetes kubectl pod k8s"));
    connect(enabled, &QCheckBox::toggled, this,
            [](bool on) { updateContainers([on](ContainerPreferences &v) { v.kubernetes.enabled = on; }); });
    auto *shell = new QLineEdit(current.kubernetes.shell);
    shell->setPlaceholderText(QStringLiteral("bash if there is one, else sh"));
    shell->setMinimumWidth(280);
    sessions->addRow(QStringLiteral("Shell in pods"), QStringLiteral("Sessions can choose their own."),
                     shell, QStringLiteral("shell bash sh"));
    connect(shell, &QLineEdit::editingFinished, this, [shell] {
        const auto text = shell->text().trimmed();
        updateContainers([text](ContainerPreferences &v) { v.kubernetes.shell = text; });
    });
    auto *timeout = spinBox(3, 600, current.kubernetes.listTimeout, QStringLiteral(" s"));
    sessions->addRow(QStringLiteral("List timeout"),
                     QStringLiteral("How long listing contexts, namespaces, pods and containers may take "
                                    "in the session window (an unreachable cluster), before it says it could not."),
                     timeout, QStringLiteral("timeout list pods"));
    connect(timeout, &QSpinBox::valueChanged, this,
            [](int seconds) { updateContainers([seconds](ContainerPreferences &v) { v.kubernetes.listTimeout = seconds; }); });
    auto *programs = page->addCard(QStringLiteral("Programs on this computer"));
    addProgramRow(programs, QStringLiteral("kubectl"), current.kubernetes.program, [](const QString &path) {
        updateContainers([&](ContainerPreferences &v) { v.kubernetes.program = path; });
    });
}

// WSL sessions (Windows).
void PreferencesWindow::buildWslPage()
{
    auto *page = addPage(QStringLiteral("wsl"), QStringLiteral("WSL"),
                         QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon);
    const auto current = loadContainerPreferences();
    auto *sessions = page->addCard(QStringLiteral("WSL sessions"));
    auto *enabled = toggle(current.wsl.enabled);
    sessions->addRow(QStringLiteral("WSL sessions"),
                     QStringLiteral("A session type that opens a Linux distribution of the Windows "
                                    "Subsystem for Linux, with its user and start folder."),
                     enabled, QStringLiteral("wsl linux ubuntu distribution"));
    connect(enabled, &QCheckBox::toggled, this,
            [](bool on) { updateContainers([on](ContainerPreferences &v) { v.wsl.enabled = on; }); });
    sessions->addWideRow(new AvailabilityNote([] { return localSessionProblem(QStringLiteral("WSL")); }, {}),
                         QStringLiteral("wsl install"));
    auto *distribution = new QLineEdit(current.wsl.distribution);
    distribution->setPlaceholderText(QStringLiteral("WSL's default"));
    sessions->addRow(QStringLiteral("Distribution"),
                     QStringLiteral("For sessions that do not choose one (wsl -l lists them)."),
                     distribution, QStringLiteral("distribution ubuntu debian"));
    connect(distribution, &QLineEdit::editingFinished, this, [distribution] {
        const auto text = distribution->text().trimmed();
        updateContainers([text](ContainerPreferences &v) { v.wsl.distribution = text; });
    });
    auto *timeout = spinBox(3, 600, current.wsl.listTimeout, QStringLiteral(" s"));
    sessions->addRow(QStringLiteral("List timeout"),
                     QStringLiteral("How long listing the distributions may take in the session window."),
                     timeout, QStringLiteral("timeout list"));
    connect(timeout, &QSpinBox::valueChanged, this,
            [](int seconds) { updateContainers([seconds](ContainerPreferences &v) { v.wsl.listTimeout = seconds; }); });
}

// Certificates and FIDO2 security keys for SSH logons.
void PreferencesWindow::buildKeysPage()
{
    auto *page = addPage(QStringLiteral("ssh-keys"), QStringLiteral("Keys & certificates"),
                         QStringLiteral("dialog-password"), QStyle::SP_DialogApplyButton);
    auto update = PREFERENCE_UPDATER(SshPreferences, sshPreferences, setSshPreferences);
    const auto current = m_mainWindow->sshPreferences();

    auto *certificates = page->addCard(QStringLiteral("Certificates"));
    auto *useCertificates = toggle(current.useCertificates);
    certificates->addRow(QStringLiteral("Use certificates"),
                         QStringLiteral("Sends a key's OpenSSH certificate (key-cert.pub next to "
                                        "it, signed by your organisation) with the key. Sessions "
                                        "can choose another certificate or none."),
                         useCertificates, QStringLiteral("certificate ca cert-pub signed principals"));
    bindToggle(this, useCertificates, update, &SshPreferences::useCertificates);

    auto *securityKeys = page->addCard(QStringLiteral("Security keys (FIDO2)"));
    auto *useSecurityKeys = toggle(current.securityKeys);
    securityKeys->addRow(QStringLiteral("Use security keys"),
                         QStringLiteral("Keys kept on a YubiKey or another FIDO2 key "
                                        "(sk-ed25519, sk-ecdsa), or in Windows Hello. OpenSSH "
                                        "asks you to touch the key, and for its PIN when it "
                                        "needs one. Off: such keys are skipped."),
                         useSecurityKeys, QStringLiteral("fido fido2 u2f yubikey sk webauthn hello"));
    bindToggle(this, useSecurityKeys, update, &SshPreferences::securityKeys);
    securityKeys->addWideRow(new AvailabilityNote(&securityKeyProblem, [useSecurityKeys](bool available) {
        useSecurityKeys->setEnabled(available || useSecurityKeys->isChecked());
    }), QStringLiteral("fido fido2 yubikey"));
    auto *openKeys = new QPushButton(QStringLiteral("SSH Keys…"));
    connect(openKeys, &QPushButton::clicked, m_mainWindow, &MainWindow::showKeyManager);
    securityKeys->addRow(QStringLiteral("Create and import"),
                         QStringLiteral("New security keys, and keys kept on the key itself "
                                        "(resident), are made in Tools → SSH Keys."),
                         openKeys, QStringLiteral("fido resident generate"));
}

// Kerberos (GSSAPI) logons: the default, and the current ticket.
void PreferencesWindow::buildKerberosPage()
{
    auto *page = addPage(QStringLiteral("kerberos"), QStringLiteral("Kerberos"),
                         QStringLiteral("security-high"), QStyle::SP_DialogYesButton);
    auto update = PREFERENCE_UPDATER(SshPreferences, sshPreferences, setSshPreferences);
    const auto current = m_mainWindow->sshPreferences();

    auto *logon = page->addCard(QStringLiteral("Kerberos logons"));
    auto *kerberos = toggle(current.kerberos);
    logon->addRow(QStringLiteral("Log on with Kerberos"),
                  QStringLiteral("SSH servers in your domain accept your Kerberos ticket instead "
                                 "of a password (GSSAPI). Folders and sessions can turn it on or "
                                 "off."),
                  kerberos, QStringLiteral("kerberos gssapi active directory domain ticket sso"));
    bindToggle(this, kerberos, update, &SshPreferences::kerberos);
    auto *delegate = toggle(current.kerberosDelegate);
    logon->addRow(QStringLiteral("Pass the ticket on"),
                  QStringLiteral("Servers can use your ticket to reach other servers and file "
                                 "shares (ssh -K, delegation). Only for servers you trust."),
                  delegate, QStringLiteral("delegate delegation forward ticket -K"));
    bindToggle(this, delegate, update, &SshPreferences::kerberosDelegate);
    delegate->setEnabled(current.kerberos);
    connect(kerberos, &QAbstractButton::toggled, delegate, &QWidget::setEnabled);
    auto *order = choiceBox({{QStringLiteral("first"), QStringLiteral("First, before keys and passwords")},
     {QStringLiteral("after-keys"), QStringLiteral("After keys")},
     {QStringLiteral("last"), QStringLiteral("Last, after keys and the saved password")}},
                            current.kerberosOrder);
    logon->addRow(QStringLiteral("Sequence of authentication"),
                  QStringLiteral("When Kerberos is tried among the ways to log on: first (the "
                                 "usual choice in a domain), after your keys, or last, after keys "
                                 "and the saved password but still before anything that asks you "
                                 "to type. Sessions can choose their own."),
                  order, QStringLiteral("kerberos order sequence authentication first last keys"));
    connect(order, &QComboBox::activated, this, [update, order] {
        update([&](SshPreferences &p) { p.kerberosOrder = order->currentData().toString(); });
    });
    order->setEnabled(current.kerberos);
    connect(kerberos, &QAbstractButton::toggled, order, &QWidget::setEnabled);
    logon->addWideRow(new AvailabilityNote(&kerberosProblem, [kerberos, delegate](bool available) {
        kerberos->setEnabled(available || kerberos->isChecked());
        delegate->setEnabled(available && kerberos->isChecked());
    }), QStringLiteral("kerberos gssapi"));

    auto *ticket = page->addCard(QStringLiteral("Your ticket"));
    auto *status = new QLabel(QStringLiteral("Not checked yet."));
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *buttons = new QWidget;
    auto *buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    auto *check = new QPushButton(QStringLiteral("Check Again"), buttons);
    auto *getTicket = new QPushButton(QStringLiteral("Get Ticket…"), buttons);
    buttonLayout->addWidget(check);
    buttonLayout->addWidget(getTicket);
    ticket->addRow(QStringLiteral("Ticket"),
                   QStringLiteral("Shown by klist. Get Ticket… runs kinit in a terminal tab, "
                                  "which asks for your domain password."),
                   buttons, QStringLiteral("klist kinit ticket principal realm"));
    ticket->addWideRow(status, QStringLiteral("klist ticket"));
    // klist runs in the background, so a slow KDC never stalls the window.
    const auto refresh = [this, status, getTicket] {
        getTicket->setEnabled(!kinitProgram().isEmpty() && kerberosProblem().isEmpty());
        if (!kerberosProblem().isEmpty()) {
            status->setText(kerberosProblem());
            return;
        }
        const auto klist = klistProgram();
        if (klist.isEmpty()) {
            status->setText(QStringLiteral("klist is not installed, so the ticket cannot be shown "
                                           "(Linux: the krb5-user, krb5-workstation, or krb5 "
                                           "package). Logons still use a ticket you have."));
            return;
        }
        status->setText(QStringLiteral("Checking…"));
        auto *process = new QProcess(this);
        process->setProcessChannelMode(QProcess::MergedChannels);
        connect(process, &QProcess::finished, this, [process, status](int exitCode) {
            status->setText(kerberosTicketSummary(process->readAll(), exitCode));
            process->deleteLater();
        });
        connect(process, &QProcess::errorOccurred, this, [process, status] {
            status->setText(QStringLiteral("Could not run klist: %1").arg(process->errorString()));
            process->deleteLater();
        });
        QTimer::singleShot(5000, process, [process] { process->kill(); });
        process->start(klist, {});
    };
    connect(check, &QPushButton::clicked, this, refresh);
    connect(getTicket, &QPushButton::clicked, this, [this] {
        m_mainWindow->openLocalProgram(kinitProgram(), {}, QStringLiteral("kinit"));
    });
    QTimer::singleShot(0, this, refresh);
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
    // Saved as you type, but not while the chosen proxy lacks its host or
    // port (the editor says so under the fields).
    connect(proxy, &ProxyEditor::changed, this, [update, proxy] {
        if (proxy->problem().isEmpty())
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
        return box;
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
    auto *vncReconnect = addSwitch(connection, QStringLiteral("Reconnect automatically"),
                                   QStringLiteral("When the connection drops (not when the server "
                                                  "closes the session)."),
                                   current.autoReconnect, QStringLiteral("reconnect network drop vnc"),
                                   [](VncOptions &o, int v) { o.autoReconnect = v; });
    addReconnectTimingRows(connection, vncReconnect, current.reconnectDelay, current.reconnectAttempts,
                           [update](int v) { update([v](VncOptions &o) { o.reconnectDelay = v; }); },
                           [update](int v) { update([v](VncOptions &o) { o.reconnectAttempts = v; }); });
    connection->addRow(QStringLiteral("Per session"),
                       QStringLiteral("Every VNC session can override these in its own settings "
                                      "(Sessions → Edit…), and can also listen for "
                                      "a reverse connection or use an UltraVNC repeater."),
                       nullptr, QStringLiteral("override listen reverse repeater"));
}

void PreferencesWindow::buildRshPage()
{
    auto *page = addPage(QStringLiteral("rlogin"), QStringLiteral("Rlogin & RSH"),
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
#ifdef Q_OS_WIN
    path->setPlaceholderText(programPathHint(QStringLiteral("vcxsrv")));
#else
    path->setPlaceholderText(programPathHint(QStringLiteral("Xephyr")));
#endif
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

    auto *kept = page->addCard(QStringLiteral("Results & history"));
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
        return box;
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
    auto *rdpReconnect = addSwitch(connection, QStringLiteral("Reconnect automatically"),
                                   QStringLiteral("When the network drops, it resumes the same "
                                                  "remote session."),
                                   current.autoReconnect, QStringLiteral("reconnect network drop resume"),
                                   [](RdpOptions &o, int v) { o.autoReconnect = v; });
    addReconnectTimingRows(connection, rdpReconnect, current.reconnectDelay, current.reconnectAttempts,
                           [update](int v) { update([v](RdpOptions &o) { o.reconnectDelay = v; }); },
                           [update](int v) { update([v](RdpOptions &o) { o.reconnectAttempts = v; }); });
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
    showRows(list); // five rows, more scroll
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
    auto *page = addPage(QStringLiteral("sftp"), QStringLiteral("Files & transfers"),
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
    auto *places = toggle(current.showPlaces);
    browsing->addRow(QStringLiteral("Show . and .. entries"),
                     QStringLiteral("At the top of the list, as in classic file-transfer programs: "
                                    "\"..\" opens the folder above, \".\" lists this one again. Drop "
                                    "files on them to upload or move there. Applies when a folder is "
                                    "next listed."),
                     places, QStringLiteral("parent up dot"));
    bindToggle(this, places, update, &SftpPreferences::showPlaces);
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
    auto *editor = new ProgramPicker(ProgramPicker::Kind::TextEditor, current.editor,
                                     QStringLiteral("The system's program for each file type"));
    editing->addRow(QStringLiteral("Editor"),
                    QStringLiteral("Edit (or double-click a file) opens it here; each save uploads "
                                   "it again. Choose a program found on this computer, browse for "
                                   "one, or type a command with its arguments."),
                    editor, QStringLiteral("edit editor open text notepad code program"));
    connect(editor, &ProgramPicker::changed, this, [update, editor] {
        update([&](SftpPreferences &v) { v.editor = editor->value(); });
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
                   QStringLiteral("Every password in the vault (unlocks it if needed). Shows first "
                                  "what is new and what differs; nothing is replaced unless you tick it."),
                   toSystem, QStringLiteral("migrate export"));
    auto *toVault = new QPushButton(QStringLiteral("Copy to the vault"));
    toVault->setEnabled(systemAvailable);
    copies->addRow(QStringLiteral("System store → vault"),
                   QStringLiteral("The saved sessions' passwords, so they travel with the folder. "
                                  "Shows first what is new and what differs."),
                   toVault, QStringLiteral("migrate import"));
    toSystem->setObjectName(QStringLiteral("copyToSystemStore"));
    toVault->setObjectName(QStringLiteral("copyToVault"));
    storeNote->check();
    connect(toSystem, &QPushButton::clicked, this, [this, mainWindow = m_mainWindow] {
        if (const auto result = mainWindow->copyPasswords(true); !result.isEmpty())
            QMessageBox::information(this, QStringLiteral("Copy Passwords"), result);
    });
    connect(toVault, &QPushButton::clicked, this, [this, mainWindow = m_mainWindow] {
        if (const auto result = mainWindow->copyPasswords(false); !result.isEmpty())
            QMessageBox::information(this, QStringLiteral("Copy Passwords"), result);
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
    auto *page = addPage(QStringLiteral("snippets"), QStringLiteral("Commands & broadcast"),
                         QStringLiteral("insert-text"), QStyle::SP_FileDialogListView);

    auto *commands = page->addCard(QStringLiteral("Commands"));
    commands->addRow(QStringLiteral("Saved commands"),
                     QStringLiteral("Kept in the main window's Commands tab (View → Commands Tab), in "
                                    "groups: double-click one to send it to the current tab, or use "
                                    "Sessions → Send Snippet and the terminal's right-click menu. A "
                                    "command can also run a saved script."),
                     nullptr, QStringLiteral("snippets commands macro text"));
    addLinkRow(commands, QStringLiteral("Ask before sending several lines"),
               QStringLiteral("The same setting as asking before a paste of several lines."),
               QStringLiteral("Workspace → Mouse & clipboard"), QStringLiteral("mouse"),
               QStringLiteral("confirm multiline commands snippets edit"),
               {QStringLiteral("Confirm multi-line paste")});
    Q_UNUSED(mainWindow);

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

// How administrator terminals start, for sessions that do not choose. (Which
// shell a terminal runs is the Local session's choice.)
void PreferencesWindow::buildLocalShellPage()
{
    auto *page = addPage(QStringLiteral("local-shell"), QStringLiteral("Administrator"),
                         QStringLiteral("security-high"), QStyle::SP_VistaShield);
    auto update = PREFERENCE_UPDATER(LocalShellPreferences, localShellPreferences,
                                     setLocalShellPreferences);
    const auto current = m_mainWindow->localShellPreferences();
    auto *admin = page->addCard(QStringLiteral("Run as administrator"));
#ifdef Q_OS_WIN
    admin->addRow(QStringLiteral("Administrator terminals"),
                  QStringLiteral("Local sessions with Run as administrator, File → New Local Terminal "
                                 "(Administrator), or Administrator in the split pane chooser. "
                                 "Windows asks first (UAC); a standard user types an "
                                 "administrator's name and password. Sessions can choose their "
                                 "own way."),
                  nullptr, QStringLiteral("admin elevated uac runas root"));
    auto *sudo = toggle(current.useSudo);
    admin->addRow(QStringLiteral("Use Windows sudo when it is turned on"),
                  QStringLiteral("Windows 11's sudo, if an administrator turned it on in inline "
                                 "mode (Settings → System → For developers). Otherwise OpsRemote "
                                 "starts the shell through its own elevated helper."),
                  sudo, QStringLiteral("sudo inline"));
    bindToggle(this, sudo, update, &LocalShellPreferences::useSudo);
#else
    auto *method = choiceBox(elevationChoices(), current.linuxElevation);
    admin->addRow(QStringLiteral("Run as root with"),
                  QStringLiteral("For Local sessions with Run as administrator, File → New Local Terminal "
                                 "(Administrator), and Administrator in the split pane chooser. "
                                 "Sessions can choose their own way."),
                  method, QStringLiteral("admin root sudo pkexec polkit"));
    bindChoice(this, method, update, &LocalShellPreferences::linuxElevation);
#endif
}

void PreferencesWindow::buildSessionListPage()
{
    auto *page = addPage(QStringLiteral("appearance-sessions"), QStringLiteral("Session list"),
                         QStringLiteral("view-list-tree"), QStyle::SP_FileDialogListView);
    auto update = PREFERENCE_UPDATER(SessionListPreferences, sessionListPreferences,
                                     setSessionListPreferences);
    const auto current = m_mainWindow->sessionListPreferences();

    auto *folders = page->addCard(QStringLiteral("Pinned & recent"));
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
    auto *tree = page->addCard(QStringLiteral("Folders"));
    auto *indentation = spinBox(4, 40, current.indentation, QStringLiteral(" px"));
    tree->addRow(QStringLiteral("Indent per folder level"),
                    QStringLiteral("How far the sessions and folders inside a folder move right."),
                    indentation, QStringLiteral("indentation indent nesting width"));
    bindSpin(this, indentation, update, &SessionListPreferences::indentation);
    auto *folderGap = spinBox(0, 30, current.folderGap, QStringLiteral(" px"));
    tree->addRow(QStringLiteral("Space after a folder's arrow"),
                    QStringLiteral("Between a folder's open/close arrow and its icon; sessions have "
                                   "their status dot there. Styles draw the arrow differently, so "
                                   "set it to what looks even."),
                    folderGap, QStringLiteral("gap spacing arrow icon align"));
    bindSpin(this, folderGap, update, &SessionListPreferences::folderGap);
    auto *foldersAtStart = choiceBox({{QStringLiteral("remember"), QStringLiteral("As they were when closed")},
                                      {QStringLiteral("expanded"), QStringLiteral("All expanded")},
                                      {QStringLiteral("collapsed"), QStringLiteral("All collapsed")}},
                                     current.foldersAtStart);
    tree->addRow(QStringLiteral("Folders at startup"),
                    QStringLiteral("Which folders are open when OpsRemote starts. As they were: "
                                   "folders it has no record of start collapsed. Pinned and Recent "
                                   "keep their own state."),
                    foldersAtStart, QStringLiteral("expand collapse open closed remember tree"));
    bindChoice(this, foldersAtStart, update, &SessionListPreferences::foldersAtStart);

    auto *windows = page->addCard(QStringLiteral("Session settings windows"));
    auto *revert = toggle(current.showRevertButtons);
    windows->addRow(QStringLiteral("Show a button to go back to the inherited value"),
                    QStringLiteral("In a session's or folder's settings, a setting you changed gets "
                                   "a small ↺ button that returns it to the value from the "
                                   "Preferences or the folder. For windows opened from now on."),
                    revert, QStringLiteral("revert reset inherited default button session"));
    bindToggle(this, revert, update, &SessionListPreferences::showRevertButtons);

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
    status->addRow(QStringLiteral("Run background host checks"),
                   QStringLiteral("Off pauses all optional host and serial-port availability "
                                  "checks immediately. On: a filled dot means the host answers "
                                  "on its port; an empty one means it does not. Only knocks on "
                                  "the port; never logs on. Sessions not checked (through a jump host "
                                  "or proxy, or with checks off) show an empty grey circle."),
                   check, QStringLiteral("reachable online ping up down status"));
    bindToggle(this, check, update, &SessionListPreferences::checkStatus);
    auto *every = spinBox(10, 3600, current.checkSeconds, QStringLiteral(" s"));
    status->addRow(QStringLiteral("Check every"), QString(), every, QStringLiteral("interval"));
    bindSpin(this, every, update, &SessionListPreferences::checkSeconds);
    every->setEnabled(check->isChecked());
    connect(check, &QCheckBox::toggled, every, &QWidget::setEnabled);
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
    showRows(list); // five rows, more scroll
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
        // Item views keep the font that existed when their delegate was made.
        // Make the Preferences navigation follow the current interface font
        // just like the page text and headings.
        m_navigation->setFont(QApplication::font());
        for (auto *label : findChildren<QLabel *>()) {
            const auto scale = label->property("headingScale");
            if (scale.isValid())
                setHeadingFont(label, scale.toReal());
        }
    }
    QWidget::changeEvent(event);
}
