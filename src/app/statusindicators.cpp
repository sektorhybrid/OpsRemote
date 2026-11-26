// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "statusindicators.h"
#include "tabactivity.h"
#include "transfers.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPainter>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace {
constexpr int MaxEntries = 200;
}

void MessageLog::add(const QString &text, Level level, bool read)
{
    if (text.trimmed().isEmpty())
        return;
    m_entries.prepend({QDateTime::currentDateTime(), text.trimmed(), level, read});
    while (m_entries.size() > MaxEntries)
        m_entries.removeLast();
    emit changed();
}

void MessageLog::markRead(const QDateTime &time)
{
    for (auto &entry : m_entries) {
        if (entry.time == time && !entry.read) {
            entry.read = true;
            emit changed();
            return;
        }
    }
}

int MessageLog::unreadCount() const
{
    return int(std::count_if(m_entries.cbegin(), m_entries.cend(), [](const Entry &e) { return !e.read; }));
}

void MessageLog::markAllRead()
{
    for (auto &entry : m_entries)
        entry.read = true;
    emit changed();
}

void MessageLog::clear()
{
    m_entries.clear();
    emit changed();
}

namespace {
bool promptsInStrip = true;
}

TabPrompt *TabPrompt::of(QWidget *page)
{
    if (auto *prompt = page->findChild<TabPrompt *>(QStringLiteral("tabPrompt"), Qt::FindDirectChildrenOnly))
        return prompt;
    auto *prompt = new TabPrompt(page);
    prompt->setObjectName(QStringLiteral("tabPrompt"));
    connect(prompt, &TabPrompt::changed, hub(), &TabPrompt::changed);
    // A closed tab's prompt goes with it.
    connect(prompt, &QObject::destroyed, hub(), &TabPrompt::changed);
    return prompt;
}

TabPrompt *TabPrompt::hub()
{
    static auto *hub = new TabPrompt(QCoreApplication::instance());
    return hub;
}

bool TabPrompt::inStrip()
{
    return promptsInStrip;
}

void TabPrompt::setInStrip(bool inStrip)
{
    if (promptsInStrip == inStrip)
        return;
    promptsInStrip = inStrip;
    emit hub()->changed();
}

void TabPrompt::set(const QString &text, bool warning, const QList<Action> &actions,
                    const std::function<void()> &dismiss, const QString &owner)
{
    m_owner = owner;
    m_text = text;
    m_warning = warning;
    m_actions = actions;
    m_dismiss = dismiss;
    emit changed();
}

void TabPrompt::clear(const QString &owner)
{
    if (!isActive() || owner != m_owner)
        return;
    m_text.clear();
    m_actions.clear();
    m_dismiss = {};
    emit changed();
}

void TabPrompt::run(int action)
{
    if (action < 0 || action >= m_actions.size())
        return;
    // The action may set a new prompt (or close the tab): keep a copy.
    const auto runIt = m_actions[action].run;
    if (runIt)
        runIt();
}

void TabPrompt::dismiss()
{
    const auto dismissIt = m_dismiss;
    if (dismissIt)
        dismissIt();
}

StatusIndicator::StatusIndicator(const QIcon &icon, const QString &widestText, QWidget *parent)
    : QAbstractButton(parent), m_widestText(widestText)
{
    setIcon(icon);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    updateSize();
}

void StatusIndicator::setState(const QString &text, bool active)
{
    updateSize(); // the look's font may have changed since
    if (text == m_text && active == m_active)
        return;
    m_text = text;
    m_active = active;
    update();
}

namespace {
constexpr int Gap = 6;      // between icon and text
constexpr int Padding = 8;  // left and right

}

// The width comes from the widest text, so the look's font and size
// (and the user's tweaks) decide it, and it never changes with the text.
QSize StatusIndicator::sizeHint() const
{
    const QFontMetrics metrics(font());
    const int iconSide = style()->pixelMetric(QStyle::PM_SmallIconSize);
    return {2 * Padding + iconSide + Gap + metrics.horizontalAdvance(m_widestText),
            m_rowHeight > 0 ? m_rowHeight : qMax(iconSide, metrics.height()) + 6};
}

void StatusIndicator::setTip(const QString &tip)
{
    m_tip = tip;
    update();
}

void StatusIndicator::setRowHeight(int height)
{
    m_rowHeight = height;
    updateSize();
}

void StatusIndicator::updateSize()
{
    if (size() != sizeHint())
        setFixedSize(sizeHint());
}

void StatusIndicator::showEvent(QShowEvent *event)
{
    updateSize();
    QAbstractButton::showEvent(event);
}

void StatusIndicator::changeEvent(QEvent *event)
{
    QAbstractButton::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
        || event->type() == QEvent::PaletteChange)
        updateSize();
}

void StatusIndicator::enterEvent(QEnterEvent *event)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(event);
}

void StatusIndicator::leaveEvent(QEvent *event)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void StatusIndicator::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto &pal = palette();
    if (isEnabled() && (m_hover || isDown())) {
        QColor shade = pal.color(QPalette::Text);
        shade.setAlphaF(isDown() ? 0.16 : 0.08);
        painter.setPen(Qt::NoPen);
        painter.setBrush(shade);
        painter.drawRoundedRect(rect().adjusted(4, 1, -1, -1), 4, 4); // clear of the divider
    }
    QColor divider = pal.color(QPalette::Text);
    divider.setAlphaF(0.18);
    painter.setPen(divider);
    painter.drawLine(QPointF(0.5, height() * 0.2), QPointF(0.5, height() * 0.8));
    const int iconSide = style()->pixelMetric(QStyle::PM_SmallIconSize);
    const QRect iconRect(Padding, (height() - iconSide) / 2, iconSide, iconSide);
    icon().paint(&painter, iconRect, Qt::AlignCenter, m_active && isEnabled() ? QIcon::Normal : QIcon::Disabled);
    const QColor accent = visibleColor(pal.color(QPalette::Window),
                                       {pal.color(QPalette::Highlight), pal.color(QPalette::Link),
                                        QColor(0xf0, 0xa0, 0x30), QColor(0x3d, 0xae, 0xe9)});
    painter.setPen(m_active ? accent : pal.color(QPalette::PlaceholderText));
    const QRect textRect(iconRect.right() + 1 + Gap, 0, width() - iconRect.right() - 1 - Gap - Padding, height());
    const QString shown = fontMetrics().elidedText(m_text, Qt::ElideRight, textRect.width());
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, shown);
    setToolTip(shown == m_text ? m_tip : m_text);
}

MessagesIndicator::MessagesIndicator(MessageLog *log, QWidget *parent)
    : QObject(parent), m_log(log),
      m_button(new StatusIndicator(parent->style()->standardIcon(QStyle::SP_MessageBoxInformation),
                                   QStringLiteral("Messages · 88 new"), parent))
{
    m_button->setTip(QStringLiteral("Messages"));
    connect(m_log, &MessageLog::changed, this, &MessagesIndicator::update);
    connect(m_button, &QAbstractButton::clicked, this, &MessagesIndicator::showList);
    update();
}

void MessagesIndicator::update()
{
    // The same words in every state; only the number and the colour change.
    const int unread = m_log->unreadCount();
    m_button->setState(QStringLiteral("Messages · %1 new").arg(unread), unread > 0);
}

// A list of messages above or below anchor, the newest first: the new ones
// in the text colour, read ones grey.
namespace {

bool listsSeeThrough = false;

// The lists' card: the look's list colour, solid unless see-through lists
// are chosen (some looks' colours are see-through themselves).
QString listCardStyle(const QWidget *card)
{
    QColor base = card->palette().color(QPalette::Base);
    base.setAlphaF(listsSeeThrough ? 0.88 : 1.0);
    return QStringLiteral("QFrame#messageList { border: 1px solid palette(mid); border-radius: 8px; "
                          "background: rgba(%1, %2, %3, %4); }")
        .arg(base.red())
        .arg(base.green())
        .arg(base.blue())
        .arg(base.alphaF(), 0, 'f', 2);
}

// A list shown over the window (not a window of its own: those land in
// different places on X11, Wayland and Windows). While it is open, the rest
// of its window gets nothing (as with a modal window): no pointer moves or
// hover (a remote desktop under it shows no rollovers), no wheel, keys or
// shortcuts; the keyboard comes to the list, and goes back when it closes.
// Esc closes it, so does a click outside it, which does nothing else; a
// click on the button that opened it closes it too, without opening it
// again. It also closes when the window changes size.
class OverlayPanel final : public QWidget
{
public:
    OverlayPanel(QWidget *anchor) : QWidget(anchor->window()), m_focusBefore(qApp->focusWidget())
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::StrongFocus);
        qApp->installEventFilter(this);
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        setFocus(Qt::PopupFocusReason);
    }
    void closeEvent(QCloseEvent *event) override
    {
        if (m_focusBefore && m_focusBefore->isVisible())
            m_focusBefore->setFocus(Qt::PopupFocusReason);
        QWidget::closeEvent(event);
    }
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *widget = qobject_cast<QWidget *>(watched);
        // Elsewhere in this window (not the list, not another window).
        const bool behind = widget && widget != this && !isAncestorOf(widget) && widget->window() == window();
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            if (widget && widget != this && !isAncestorOf(widget)) {
                if (!behind) {
                    close(); // another window: its click goes on as usual
                    break;
                }
                // Swallowed with its release (buttons act on the release, the
                // tab strip too): a click on the button that opened the list
                // does not open it again. Closed after the release.
                hide();
                m_swallowRelease = true;
                // Should the release never come (the window lost the pointer),
                // the window is not left waiting.
                QTimer::singleShot(1500, this, &QWidget::close);
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (m_swallowRelease) {
                close();
                return true;
            }
            return behind;
        case QEvent::KeyPress:
            if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape && (behind || widget == this || isAncestorOf(widget))) {
                close();
                return true;
            }
            return behind;
        case QEvent::Shortcut:
        case QEvent::ShortcutOverride:
            // The window's shortcuts wait too (they go to actions, not widgets).
            return QApplication::activeWindow() == window() && !(widget && (widget == this || isAncestorOf(widget)));
        case QEvent::MouseMove:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::HoverEnter:
        case QEvent::HoverLeave:
        case QEvent::HoverMove:
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::ContextMenu:
        case QEvent::KeyRelease:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
        case QEvent::ToolTip:
            return behind;
        case QEvent::Resize:
            if (watched == parentWidget())
                close();
            break;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QPointer<QWidget> m_focusBefore;
    bool m_swallowRelease = false;
};

// Above (or below) the anchor with a small gap, lined up with the window's
// right edge, wherever it was opened from.
void placeOverlay(QWidget *panel, QWidget *anchor, bool below)
{
    constexpr int Gap = 4;
    auto *window = panel->parentWidget();
    const int top = below ? anchor->mapTo(window, QPoint(0, anchor->height())).y() + Gap
                          : anchor->mapTo(window, QPoint(0, 0)).y() - panel->height() - Gap;
    panel->move(qMax(0, window->width() - panel->width() - Gap), qMax(0, top));
}

} // namespace

namespace {
// The Messages and Transfers lists' popup: a see-through window holding a
// card with round corners, its edge and background from the look, so it
// stands out from the window behind; 64 characters wide, with a bold title
// and a flat button at the right.
struct ListCard
{
    OverlayPanel *popup;
    QFrame *card;
    QVBoxLayout *layout;
    QHBoxLayout *header; // the title and the button
    QPushButton *button;
    int width;
    int unit; // spacing that follows the font size
};

ListCard listCard(QWidget *anchor, const QString &title, const QString &buttonText)
{
    auto *popup = new OverlayPanel(anchor);
    auto *outer = new QVBoxLayout(popup);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *card = new QFrame(popup);
    card->setObjectName(QStringLiteral("messageList"));
    card->setStyleSheet(listCardStyle(card));
    outer->addWidget(card);
    const int width = popup->fontMetrics().averageCharWidth() * 64;
    popup->setFixedWidth(width);
    const int unit = popup->fontMetrics().height();
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(unit, unit * 3 / 4, unit, unit * 3 / 4);
    layout->setSpacing(unit / 2);
    auto *header = new QHBoxLayout;
    auto *heading = new QLabel(card);
    heading->setTextFormat(Qt::PlainText);
    heading->setText(title);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    header->addWidget(heading, 1);
    auto *button = new QPushButton(buttonText, card);
    button->setFlat(true);
    header->addWidget(button);
    layout->addLayout(header);
    return {popup, card, layout, header, button, width, unit};
}
}

void showMessageList(QWidget *anchor, const QString &title, const QList<MessageLog::Entry> &entries,
                     const std::function<void()> &clearAll, bool below)
{
    const auto [popup, card, layout, header, clear, width, unit] = listCard(anchor, title, QStringLiteral("Clear all"));
    clear->setEnabled(!entries.isEmpty());

    auto *list = new QWidget;
    auto *rows = new QVBoxLayout(list);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(unit / 2);
    if (entries.isEmpty()) {
        auto *none = new QLabel(QStringLiteral("No messages."), list);
        none->setEnabled(false);
        rows->addWidget(none);
    }
    const int iconSize = popup->style()->pixelMetric(QStyle::PM_SmallIconSize);
    for (const auto &entry : entries) {
        auto *row = new QHBoxLayout;
        row->setSpacing(unit / 2);
        auto *icon = new QLabel(list);
        icon->setPixmap(popup->style()
                            ->standardIcon(entry.level == MessageLog::Level::Warning ? QStyle::SP_MessageBoxWarning
                                                                                     : QStyle::SP_MessageBoxInformation)
                            .pixmap(iconSize, iconSize));
        icon->setAlignment(Qt::AlignTop);
        row->addWidget(icon);
        auto *time = new QLabel(QLocale().toString(entry.time.time(), QLocale::ShortFormat), list);
        time->setEnabled(false);
        time->setAlignment(Qt::AlignTop);
        row->addWidget(time);
        auto *text = new QLabel(list);
        text->setTextFormat(Qt::PlainText);
        text->setText(entry.text);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->setEnabled(!entry.read); // new ones in the text colour, read ones grey
        row->addWidget(text, 1);
        rows->addLayout(row);
    }
    rows->addStretch();
    auto *scroll = new QScrollArea(card);
    scroll->setWidget(list);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);
    list->setAutoFillBackground(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll);
    QObject::connect(clear, &QPushButton::clicked, popup, [clearAll, popup] {
        if (clearAll)
            clearAll();
        popup->close();
    });

    // As tall as the messages at this width (wrapped), at most about 20 lines.
    const auto margins = layout->contentsMargins();
    const int listWidth = width - margins.left() - margins.right() - 2; // 2: the card's edge
    const int listHeight = rows->hasHeightForWidth() ? rows->totalHeightForWidth(listWidth)
                                                     : rows->totalSizeHint().height();
    const int height = qMin(margins.top() + header->sizeHint().height() + layout->spacing() + listHeight
                                + margins.bottom() + 2,
                            popup->fontMetrics().height() * 22);
    popup->setFixedHeight(height);
    placeOverlay(popup, anchor, below);
    popup->show();
    popup->raise();
}

void MessagesIndicator::showList()
{
    showMessageList(m_button, QStringLiteral("Messages"), m_log->entries(), [this] { m_log->clear(); }, false);
    m_log->markAllRead();
}

// A search box's height in context's font and style.
int panelRowHeight(const QWidget *context)
{
    QLineEdit probe;
    probe.setFont(context->font());
    probe.setStyle(context->style());
    return probe.sizeHint().height();
}

TabMessageStrip::TabMessageStrip(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setFixedHeight(panelRowHeight(this));
    // As at the bottom of the window: the tab's messages on the right, past a
    // divider (only the icon and the number, leaving the room to the
    // message); the transfer part and the message (or prompt) to its left.
    m_messages = new StatusIndicator(style()->standardIcon(QStyle::SP_MessageBoxInformation),
                                     QStringLiteral("88"), this);
    m_messages->setCursor(Qt::ArrowCursor);
    connect(m_messages, &QAbstractButton::clicked, this, &TabMessageStrip::historyRequested);
    setMessageCount(-1); // no tab yet
}

void TabMessageStrip::setCountShown(bool shown)
{
    m_countShown = shown;
    m_messages->setVisible(shown);
    // A pane's bar: nothing to open by a click on its message.
    setPointer({-1, -1});
    placeButtons();
    update();
}

int TabMessageStrip::partsRight() const
{
    return m_countShown && m_messages->isVisibleTo(this) ? m_messages->x() : width();
}

void TabMessageStrip::setMessageCount(int unread)
{
    m_messages->setVisible(m_countShown && unread >= 0);
    m_messages->setState(QString::number(qMax(unread, 0)), unread > 0);
    m_messages->setTip(QStringLiteral("This tab's messages: %1 new").arg(unread));
}

namespace {
const QString MoveTip = QStringLiteral("Drag to move this terminal: next to another one, to another tab, or to a tab "
                                       "of its own");
}

void TabMessageStrip::setMovable(bool movable)
{
    if (m_movable == movable)
        return;
    m_movable = movable;
    m_hoverHandle = false;
    setPointer({-1, -1});
    update();
}

void TabMessageStrip::setMoveStyle(bool handleOnly, bool handleMenu)
{
    m_handleOnly = handleOnly;
    m_handleMenu = handleMenu;
    setPointer({-1, -1});
}

void TabMessageStrip::setFixedParts(bool fixed)
{
    if (m_fixedParts == fixed)
        return;
    m_fixedParts = fixed;
    placeButtons();
    update();
}

bool TabMessageStrip::drags(const QPoint &at) const
{
    if (!m_movable)
        return false;
    if (handleRect().contains(at))
        return true;
    return !m_handleOnly && !clickable(at);
}

// The open hand where a press drags, the pointing hand where it clicks.
void TabMessageStrip::setPointer(const QPoint &at)
{
    if (!m_movable)
        setCursor(m_countShown ? Qt::PointingHandCursor : Qt::ArrowCursor);
    else if (rect().contains(at) ? drags(at) : !m_handleOnly)
        setCursor(Qt::OpenHandCursor);
    else
        setCursor(clickable(at) ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

// About two characters wide, the whole height, at the left end.
QRect TabMessageStrip::handleRect() const
{
    if (!m_movable)
        return {};
    return {0, 0, qMax(12, fontMetrics().averageCharWidth() * 2), height()};
}

bool TabMessageStrip::clickable(const QPoint &at) const
{
    if (m_movable && handleRect().contains(at))
        return false;
    return m_countShown || (showsClose() && closeRect().contains(at)) || (m_transfer.shown && transferRect().contains(at));
}

void TabMessageStrip::showEntry(const MessageLog::Entry &entry, const QString &pane)
{
    m_entry = entry;
    m_pane = pane;
    update();
}

void TabMessageStrip::clear()
{
    clearPrompt();
    m_entry = {};
    m_pane.clear();
    update();
}

void TabMessageStrip::showPrompt(const QString &text, bool warning, const QString &pane,
                                 const QList<QPair<QString, QString>> &buttons, bool closable, int more)
{
    m_prompt = true;
    m_promptClosable = closable;
    m_promptMore = more;
    m_entry = {};
    m_entry.text = text;
    m_entry.level = warning ? MessageLog::Level::Warning : MessageLog::Level::Notice;
    m_pane = pane;
    // The same buttons again keep their widgets (no flicker on updates).
    bool same = m_buttons.size() == buttons.size();
    for (int index = 0; same && index < buttons.size(); ++index)
        same = m_buttons[index]->text() == buttons[index].first;
    if (!same) {
        qDeleteAll(m_buttons);
        m_buttons.clear();
        for (int index = 0; index < buttons.size(); ++index) {
            auto *button = new QPushButton(buttons[index].first, this);
            // The tab keeps the keyboard focus (Enter and Esc work there).
            button->setFocusPolicy(Qt::NoFocus);
            button->setCursor(Qt::ArrowCursor);
            connect(button, &QPushButton::clicked, this, [this, index] { emit promptActionRequested(index); });
            m_buttons.append(button);
        }
    }
    for (int index = 0; index < buttons.size(); ++index) {
        m_buttons[index]->setToolTip(buttons[index].second);
        m_buttons[index]->show();
    }
    placeButtons();
    update();
}

void TabMessageStrip::clearPrompt()
{
    if (!m_prompt)
        return;
    m_prompt = false;
    m_promptMore = 0;
    qDeleteAll(m_buttons);
    m_buttons.clear();
    m_entry = {};
    m_pane.clear();
    update();
}

void TabMessageStrip::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    placeButtons();
}

// Right to left from the ✕ (or where it would be), each as tall as the
// strip allows; the text is shortened to make room.
void TabMessageStrip::placeButtons()
{
    m_messages->setRowHeight(height());
    m_messages->move(width() - m_messages->width(), 0);
    int right = showsClose() ? closeRect().left() - 4 : closeRect().right();
    const int buttonHeight = height() - 4;
    for (int index = int(m_buttons.size()) - 1; index >= 0; --index) {
        auto *button = m_buttons[index];
        const int buttonWidth = qMax(button->sizeHint().width(), fontMetrics().horizontalAdvance(button->text()) + 16);
        button->setGeometry(right - buttonWidth, 2, buttonWidth, buttonHeight);
        right -= buttonWidth + 4;
    }
}

int TabMessageStrip::textRight() const
{
    if (!m_buttons.isEmpty())
        return m_buttons.first()->geometry().left() - 4;
    return showsClose() ? closeRect().left() : closeRect().right();
}

void TabMessageStrip::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        setFixedHeight(panelRowHeight(this));
        placeButtons();
    }
}

QRect TabMessageStrip::closeRect() const
{
    const int side = height() - 8;
    const int right = transferPart() ? transferRect().left() - 4 : partsRight() - 6;
    return {right - side, 4, side, side};
}

// The transfer part left of the Messages part, about 30 characters wide, while the tab
// transfers; the message on the left is shortened to make room.
QRect TabMessageStrip::transferRect() const
{
    const int character = fontMetrics().averageCharWidth();
    int widthWanted = qMin(character * 42, partsRight() * 2 / 5);
    // On a narrow pane the message keeps about 12 characters (with its icon
    // and ✕) before the transfer part gets smaller.
    const int messageLeast = (m_movable ? handleRect().width() : 0) + character * 12 + height() * 2;
    widthWanted = qMax(qMin(widthWanted, partsRight() - messageLeast), qMin(character * 10, partsRight() / 3));
    return {partsRight() - widthWanted, 0, widthWanted, height()};
}

QRect TabMessageStrip::transferCancelRect() const
{
    const int side = height() - 8;
    return {partsRight() - side - 6, 4, side, side};
}

void TabMessageStrip::showTransfer(const QString &name, bool upload, int percent, int more, const QString &cancelHint,
                                   const QString &ended)
{
    const bool placeChanged = !m_transfer.shown;
    m_transfer = {name, upload, percent, more, cancelHint, true, ended};
    if (placeChanged)
        placeButtons();
    update();
}

void TabMessageStrip::clearTransfer()
{
    if (!m_transfer.shown)
        return;
    m_transfer = {};
    placeButtons();
    update();
}

void TabMessageStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const auto &pal = palette();
    // Flat, no line of its own. With a tab open, halfway toward the colour of
    // lists and fields, so it reads as the tab's own row; with none, the
    // window colour.
    QColor ground = pal.color(QPalette::Window);
    if (!m_countShown || m_messages->isVisibleTo(this)) {
        const QColor base = pal.color(QPalette::Base);
        ground = QColor::fromRgbF((ground.redF() + base.redF()) / 2, (ground.greenF() + base.greenF()) / 2,
                                  (ground.blueF() + base.blueF()) / 2);
    }
    painter.fillRect(rect(), ground);
    if (m_movable) {
        // The handle: two columns of three dots, the accent colour under the
        // pointer.
        const QRect handle = handleRect();
        QColor dots = pal.color(QPalette::Text);
        dots.setAlphaF(0.4);
        if (m_hoverHandle)
            dots = visibleColor(ground, {pal.color(QPalette::Highlight), pal.color(QPalette::Link),
                                         QColor(0xf0, 0xa0, 0x30), QColor(0x3d, 0xae, 0xe9)});
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(dots);
        const QPointF centre = QRectF(handle).center();
        for (int column = -1; column <= 1; column += 2)
            for (int row = -1; row <= 1; ++row)
                painter.drawEllipse(centre + QPointF(column * 2.5, row * 4.5), 1.4, 1.4);
        painter.setRenderHint(QPainter::Antialiasing, false);
    }
    QColor edge = pal.color(QPalette::Text); // the transfer part's divider
    edge.setAlphaF(0.15);
    // Dividers: after the handle, before the transfer part.
    painter.setPen(edge);
    if (m_movable && m_fixedParts)
        painter.drawLine(handleRect().right(), 5, handleRect().right(), height() - 6);
    if (transferPart())
        painter.drawLine(transferRect().left(), 5, transferRect().left(), height() - 6);
    if (m_transfer.shown) {
        // ↑/↓, the file, a small bar, %, +N, ✕.
        const QRect area = transferRect();
        const int iconSide = style()->pixelMetric(QStyle::PM_SmallIconSize);
        const QRect arrow(area.left() + 8, (height() - iconSide) / 2, iconSide, iconSide);
        style()->standardIcon(m_transfer.upload ? QStyle::SP_ArrowUp : QStyle::SP_ArrowDown).paint(&painter, arrow);
        const QRect cancel = transferCancelRect();
        const QString more = m_transfer.more > 0 ? QStringLiteral("+%1").arg(m_transfer.more) : QString();
        // Ended: its state where the % was, no bar.
        const QString percent = m_transfer.ended.isEmpty() ? QStringLiteral("%1%").arg(m_transfer.percent)
                                                           : m_transfer.ended;
        int barWidth = fontMetrics().averageCharWidth() * 5;
        int x = cancel.left() - 6;
        const QColor accent = visibleColor(pal.color(QPalette::Window),
                                           {pal.color(QPalette::Highlight), pal.color(QPalette::Link),
                                            QColor(0xf0, 0xa0, 0x30), QColor(0x3d, 0xae, 0xe9)});
        if (!more.isEmpty()) {
            const int w = fontMetrics().horizontalAdvance(more);
            painter.setPen(accent);
            painter.drawText(QRect(x - w, 0, w, height()), Qt::AlignVCenter, more);
            x -= w + 6;
        }
        const int pw = qMax(fontMetrics().horizontalAdvance(QStringLiteral("100%")),
                            fontMetrics().horizontalAdvance(percent));
        painter.setPen(pal.color(QPalette::PlaceholderText));
        painter.drawText(QRect(x - pw, 0, pw, height()), Qt::AlignVCenter | Qt::AlignRight, percent);
        x -= pw + 6;
        // Narrow: the small bar goes first, so the file keeps a short name.
        if (x - barWidth - 12 - arrow.right() < fontMetrics().averageCharWidth() * 8)
            barWidth = 0;
        const QRectF bar(x - barWidth, height() / 2.0 - 3, barWidth, 6);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        QColor trough = pal.color(QPalette::Text);
        trough.setAlphaF(0.15);
        if (m_transfer.ended.isEmpty() && barWidth > 0) {
            painter.setBrush(trough);
            painter.drawRoundedRect(bar, 3, 3);
            painter.setBrush(accent);
            painter.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * m_transfer.percent / 100.0, bar.height()), 3, 3);
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QRect name(arrow.right() + 6, 0, int(bar.left()) - arrow.right() - (barWidth > 0 ? 12 : 6), height());
        painter.setPen(pal.color(QPalette::Text));
        painter.drawText(name, Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(m_transfer.name, Qt::ElideMiddle, name.width()));
        if (!m_transfer.cancelHint.isEmpty()) {
            QColor cross = pal.color(QPalette::Text);
            cross.setAlphaF(m_hoverCancel ? 0.9 : 0.45);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(cross, 1.4));
            const QRectF xr = QRectF(cancel).adjusted(4, 4, -4, -4);
            painter.drawLine(xr.topLeft(), xr.bottomRight());
            painter.drawLine(xr.topRight(), xr.bottomLeft());
            painter.setRenderHint(QPainter::Antialiasing, false);
        }
    }
    if (m_entry.text.isEmpty())
        return; // Empty, the same height.
    const int iconSide = style()->pixelMetric(QStyle::PM_SmallIconSize);
    const int left = m_movable ? handleRect().right() + 4 : 8; // past the handle
    const QRect iconRect(left, (height() - iconSide) / 2, iconSide, iconSide);
    style()->standardIcon(m_entry.level == MessageLog::Level::Warning ? QStyle::SP_MessageBoxWarning
                                                                      : QStyle::SP_MessageBoxInformation)
        .paint(&painter, iconRect);
    const QRect close = closeRect();
    QString text = m_pane.isEmpty() ? m_entry.text : QStringLiteral("%1: %2").arg(m_pane, m_entry.text);
    if (m_promptMore > 0)
        text = QStringLiteral("(+%1) %2").arg(m_promptMore).arg(text);
    const QRect textRect(iconRect.right() + 7, 0, textRight() - iconRect.right() - 10, height());
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QString shown = fontMetrics().elidedText(text, Qt::ElideRight, textRect.width());
    painter.setPen(pal.color(QPalette::Text));
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, shown);
    // A pane's bar opens nothing by a click on its message.
    const QString hint = m_countShown ? QStringLiteral("Click for this tab's messages") : QString();
    // A pane's bar: the whole message, always.
    m_messageTip = hint.isEmpty() ? text : shown == text ? hint : text + QStringLiteral("\n\n") + hint;
    if (!showsClose())
        return;
    // The ✕ that puts the message away (it stays in the tab's list).
    QColor cross = pal.color(QPalette::Text);
    cross.setAlphaF(m_hoverClose ? 0.9 : 0.45);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(cross, 1.4));
    const QRectF x = QRectF(close).adjusted(4, 4, -4, -4);
    painter.drawLine(x.topLeft(), x.bottomRight());
    painter.drawLine(x.topRight(), x.bottomLeft());
}

void TabMessageStrip::mousePressEvent(QMouseEvent *event)
{
    m_pressedAt.reset();
    const QPoint at = event->position().toPoint();
    m_pressedOnHandle = event->button() == Qt::LeftButton && m_movable && handleRect().contains(at);
    if (event->button() == Qt::LeftButton && (!m_movable || drags(at)))
        m_pressedAt = at;
}

void TabMessageStrip::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint at = event->position().toPoint();
    // Pressed and moved far enough: a drag of the pane, not a click.
    if (m_pressedAt && (event->buttons() & Qt::LeftButton)
        && (at - *m_pressedAt).manhattanLength() >= QApplication::startDragDistance()) {
        m_pressedAt.reset();
        emit dragRequested();
        return;
    }
    const bool over = showsClose() && closeRect().contains(at);
    const bool overCancel = m_transfer.shown && !m_transfer.cancelHint.isEmpty() && transferCancelRect().contains(at);
    const bool overHandle = m_movable && handleRect().contains(at);
    if (over != m_hoverClose || overCancel != m_hoverCancel || overHandle != m_hoverHandle) {
        m_hoverClose = over;
        m_hoverCancel = overCancel;
        m_hoverHandle = overHandle;
        update();
    }
    setPointer(at);
    // The tooltip of the part under the pointer.
    if (overHandle)
        setToolTip(m_handleMenu ? MoveTip + QStringLiteral(".\nClick for the Move menu.") : MoveTip);
    else if (overCancel)
        setToolTip(m_transfer.cancelHint);
    else if (m_transfer.shown && transferRect().contains(at))
        setToolTip(QStringLiteral("%1\n\nClick for this tab's transfers").arg(m_transfer.name));
    else if (over)
        setToolTip(m_prompt ? QStringLiteral("Close this without answering") : QStringLiteral("Put this message away"));
    else if (!m_entry.text.isEmpty() && !m_messageTip.isEmpty())
        setToolTip(m_messageTip);
    else
        setToolTip(m_movable && !m_handleOnly ? MoveTip : QString());
}

void TabMessageStrip::leaveEvent(QEvent *)
{
    if (m_hoverClose || m_hoverCancel || m_hoverHandle) {
        m_hoverClose = m_hoverCancel = m_hoverHandle = false;
        update();
    }
}

void TabMessageStrip::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_pressedAt.reset();
    const QPoint at = event->position().toPoint();
    const bool onHandle = std::exchange(m_pressedOnHandle, false);
    if (m_movable && handleRect().contains(at)) {
        if (onHandle && m_handleMenu)
            emit handleClicked(mapToGlobal(QPoint(0, height())));
        return;
    }
    if (m_transfer.shown && !m_transfer.cancelHint.isEmpty() && transferCancelRect().contains(at))
        emit cancelTransferRequested();
    else if (m_transfer.shown && transferRect().contains(at))
        emit transfersRequested();
    else if (showsClose() && closeRect().contains(at) && m_prompt)
        emit promptDismissRequested();
    else if (showsClose() && closeRect().contains(at))
        emit dismissRequested();
    else if (m_countShown)
        emit historyRequested(); // also when empty: the messages put away
}

namespace {

// One row of the transfer list, updated in place while it runs.
struct TransferRow
{
    QLabel *name = nullptr;
    QLabel *detail = nullptr;
    QProgressBar *bar = nullptr;
    QLabel *percent = nullptr;
    QToolButton *pause = nullptr;  // ⏸ / ▶ (greyed once ended)
    QToolButton *cancel = nullptr; // ✕, or ↻ once cancelled or failed
};

QString stateText(const TransferList::Transfer &transfer)
{
    switch (transfer.state) {
    case TransferList::State::Done:
        return QStringLiteral("done");
    case TransferList::State::Failed:
        return QStringLiteral("failed");
    case TransferList::State::Cancelled:
        return QStringLiteral("cancelled");
    case TransferList::State::Running:
        break;
    }
    return transfer.paused ? QStringLiteral("paused") : transfer.detail;
}

} // namespace

void showTransferList(QWidget *anchor, const std::function<bool(QWidget *)> &include,
                      const std::function<QString(QWidget *)> &whereOf, bool below)
{
    const auto [popup, card, layout, header, clear, width, unit] =
        listCard(anchor, include ? QStringLiteral("Transfers in this tab") : QStringLiteral("Transfers"),
                 QStringLiteral("Clear done"));
    auto *list = new QWidget(card);
    auto *rows = new QVBoxLayout(list);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(unit / 2);
    layout->addWidget(list);

    auto *transfers = TransferList::instance();
    // Starts as an impossible list, so the first call always builds the rows
    // and places the list (also when there are no transfers at all).
    auto ids = std::make_shared<QList<int>>(QList<int>{-1});
    auto shownRows = std::make_shared<QHash<int, TransferRow>>();
    const auto chosen = [include] {
        QList<TransferList::Transfer> result;
        for (const auto &transfer : TransferList::instance()->transfers()) {
            if (!include || (transfer.owner && include(transfer.owner)))
                result.append(transfer);
        }
        // Running ones first, each group newest first.
        std::stable_partition(result.begin(), result.end(),
                              [](const auto &t) { return t.state == TransferList::State::Running; });
        return result;
    };
    const auto place = [popup, anchor, below, layout, header, list, width] {
        popup->setFixedHeight(qMin(layout->contentsMargins().top() + header->sizeHint().height() + layout->spacing()
                                       + list->sizeHint().height() + layout->contentsMargins().bottom() + 2,
                                   popup->fontMetrics().height() * 22));
        placeOverlay(popup, anchor, below);
    };
    const auto fill = [whereOf, shownRows](const TransferList::Transfer &transfer) {
        const auto row = shownRows->value(transfer.id);
        if (!row.bar)
            return;
        const bool running = transfer.state == TransferList::State::Running;
        row.bar->setValue(transfer.percent());
        row.bar->setVisible(running);
        row.percent->setText(running ? QStringLiteral("%1%").arg(transfer.percent()) : QString());
        row.name->setText(transfer.name);
        row.name->setEnabled(running);
        // Running: pause (or continue) and cancel. Cancelled or failed: a
        // greyed continue, and retry from the beginning. Done: nothing.
        const bool retryable = (transfer.state == TransferList::State::Cancelled
                                || transfer.state == TransferList::State::Failed)
                               && transfer.retry;
        auto *style = row.pause->style();
        row.pause->setIcon(style->standardIcon(running && !transfer.paused ? QStyle::SP_MediaPause
                                                                           : QStyle::SP_MediaPlay));
        row.pause->setToolTip(transfer.paused ? QStringLiteral("Continue") : QStringLiteral("Pause"));
        row.pause->setVisible((running && transfer.pause) || retryable);
        row.pause->setEnabled(running);
        row.cancel->setVisible((running && transfer.cancel) || retryable);
        row.cancel->setText(running ? QStringLiteral("✕") : QString());
        row.cancel->setIcon(running ? QIcon() : style->standardIcon(QStyle::SP_BrowserReload));
        row.cancel->setToolTip(running ? transfer.cancelHint : QStringLiteral("Start again from the beginning"));
        const QString where = whereOf ? whereOf(transfer.owner) : QString();
        row.detail->setText(where.isEmpty() ? stateText(transfer)
                                            : QStringLiteral("%1 · %2").arg(where, stateText(transfer)));
    };
    // Built again when transfers come or go; otherwise only the numbers change.
    const auto rebuild = [=] {
        const auto current = chosen();
        QList<int> currentIds;
        for (const auto &transfer : current)
            currentIds.append(transfer.id);
        if (currentIds == *ids) {
            for (const auto &transfer : current)
                fill(transfer);
            return;
        }
        *ids = currentIds;
        shownRows->clear();
        while (auto *item = rows->takeAt(0)) {
            if (auto *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        clear->setEnabled(std::any_of(current.cbegin(), current.cend(),
                                      [](const auto &t) { return t.state != TransferList::State::Running; }));
        if (current.isEmpty()) {
            auto *none = new QLabel(QStringLiteral("No transfers."), list);
            none->setEnabled(false);
            rows->addWidget(none);
            none->show(); // now, not later: the list is measured below
        }
        const int iconSize = popup->style()->pixelMetric(QStyle::PM_SmallIconSize);
        for (const auto &transfer : current) {
            auto *rowWidget = new QWidget(list);
            auto *row = new QHBoxLayout(rowWidget);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(unit / 2);
            auto *icon = new QLabel(rowWidget);
            icon->setPixmap(popup->style()
                                ->standardIcon(transfer.upload ? QStyle::SP_ArrowUp : QStyle::SP_ArrowDown)
                                .pixmap(iconSize, iconSize));
            icon->setAlignment(Qt::AlignTop);
            row->addWidget(icon);
            // Name and % on one line, the bar under them, then where and how fast.
            auto *texts = new QVBoxLayout;
            texts->setSpacing(2);
            TransferRow shown;
            auto *top = new QHBoxLayout;
            shown.name = new QLabel(rowWidget);
            shown.name->setTextFormat(Qt::PlainText);
            shown.name->setMinimumWidth(1);
            top->addWidget(shown.name, 1);
            shown.percent = new QLabel(rowWidget);
            shown.percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            top->addWidget(shown.percent);
            texts->addLayout(top);
            shown.bar = new QProgressBar(rowWidget);
            shown.bar->setRange(0, 100);
            shown.bar->setTextVisible(false);
            shown.bar->setFixedHeight(qMax(4, unit / 3));
            texts->addWidget(shown.bar);
            shown.detail = new QLabel(rowWidget);
            shown.detail->setTextFormat(Qt::PlainText);
            shown.detail->setMinimumWidth(1);
            shown.detail->setEnabled(false);
            QFont small = shown.detail->font();
            small.setPointSizeF(small.pointSizeF() * 0.9);
            shown.detail->setFont(small);
            texts->addWidget(shown.detail);
            row->addLayout(texts, 1);
            auto *pause = shown.pause = new QToolButton(rowWidget);
            pause->setAutoRaise(true);
            QObject::connect(pause, &QToolButton::clicked, popup, [id = transfer.id] {
                auto *list = TransferList::instance();
                for (const auto &other : list->transfers())
                    if (other.id == id)
                        list->pause(id, !other.paused);
            });
            auto *cancel = shown.cancel = new QToolButton(rowWidget);
            cancel->setAutoRaise(true);
            // Kept (invisible) when it cannot be used, so rows line up;
            // shown or not by fill().
            auto policy = cancel->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            cancel->setSizePolicy(policy);
            pause->setSizePolicy(policy);
            QObject::connect(cancel, &QToolButton::clicked, popup, [id = transfer.id] {
                auto *list = TransferList::instance();
                for (const auto &other : list->transfers())
                    if (other.id == id) {
                        if (other.state == TransferList::State::Running)
                            list->cancel(id);
                        else
                            list->retry(id);
                        return;
                    }
            });
            row->addWidget(pause, 0, Qt::AlignTop);
            row->addWidget(cancel, 0, Qt::AlignTop);
            rows->addWidget(rowWidget);
            // Shown now (a layout shows it only later): the list is measured
            // below, also when rebuilt while open.
            rowWidget->show();
            shownRows->insert(transfer.id, shown);
        }
        for (const auto &transfer : current)
            fill(transfer);
        list->adjustSize();
        place();
    };
    QObject::connect(clear, &QPushButton::clicked, popup, [] { TransferList::instance()->clearEnded(); });
    QObject::connect(transfers, &TransferList::changed, popup, rebuild);
    rebuild();
    popup->show();
    popup->raise();
}

bool seeThroughLists()
{
    return listsSeeThrough;
}

void setSeeThroughLists(bool seeThrough)
{
    listsSeeThrough = seeThrough;
}
