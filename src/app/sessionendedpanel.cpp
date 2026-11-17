// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sessionendedpanel.h"
#include "statusindicators.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

SessionEndedPanel::SessionEndedPanel(Style style, QWidget *parent)
    : QFrame(parent), m_style(style)
{
    setObjectName(QStringLiteral("sessionEndedPanel"));
    setAttribute(Qt::WA_StyledBackground);
    // Only this frame gets the border; buttons keep the normal style.
    setStyleSheet(style == Style::Card
                      ? QStringLiteral("#sessionEndedPanel { background: palette(window);"
                                       " border: 1px solid palette(mid); border-radius: 8px; }")
                      : QStringLiteral("#sessionEndedPanel { background: palette(window);"
                                       " border-bottom: 1px solid palette(mid); }"));

    m_title = new QLabel(this);
    m_title->setTextFormat(Qt::PlainText);
    applyTitleFont();
    m_detail = new QLabel(this);
    m_detail->setTextFormat(Qt::PlainText);
    m_detail->setWordWrap(true);

    m_reconnect = new QPushButton(QStringLiteral("Reconnect"), this);
    m_reconnect->setIcon(style == Style::Card ? QIcon::fromTheme(QStringLiteral("view-refresh"),
                                                                 this->style()->standardIcon(QStyle::SP_BrowserReload))
                                              : QIcon());
    m_reconnect->setToolTip(QStringLiteral("Connect again (Enter)"));
    m_close = new QPushButton(QStringLiteral("Close Tab"), this);
    m_close->setToolTip(QStringLiteral("Close this tab (Esc)"));
    // The tab keeps the keyboard focus; Enter and Esc work from there.
    for (auto *button : {m_reconnect, m_close})
        button->setFocusPolicy(Qt::NoFocus);
    connect(m_reconnect, &QPushButton::clicked, this, &SessionEndedPanel::reconnectRequested);
    connect(m_close, &QPushButton::clicked, this, &SessionEndedPanel::closeRequested);

    if (style == Style::Card) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->setSpacing(10);
        m_title->setAlignment(Qt::AlignCenter);
        m_detail->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_title);
        layout->addWidget(m_detail);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        buttons->addWidget(m_reconnect);
        buttons->addWidget(m_close);
        buttons->addStretch();
        layout->addSpacing(4);
        layout->addLayout(buttons);
        // Wide enough that a wrapped reason gets its full height.
        m_detail->setMinimumWidth(380);
        setMaximumWidth(560);
    } else {
        // Only as tall as its contents, above a terminal that takes the rest.
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 5, 6, 5);
        auto *text = new QVBoxLayout;
        text->setSpacing(0);
        text->addWidget(m_title);
        text->addWidget(m_detail);
        layout->addLayout(text, 1);
        layout->addWidget(m_reconnect);
        layout->addWidget(m_close);
    }
}

void SessionEndedPanel::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange)
        applyTitleFont();
    QFrame::changeEvent(event);
}

void SessionEndedPanel::applyTitleFont()
{
    QFont bold = font();
    bold.setBold(true);
    if (m_style == Style::Card)
        bold.setPointSizeF(bold.pointSizeF() * 1.2);
    m_title->setFont(bold);
}

void SessionEndedPanel::setMessage(const QString &title, const QString &detail, bool error)
{
    m_title->setText(title);
    m_title->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
    m_detail->setText(detail);
    m_detail->setVisible(!detail.isEmpty());
    m_error = error;
    adjustSize();
    if (m_offered)
        offerInStrip();
}

// The bar's message and buttons go to the tab strip when it is on; the bar
// itself shows only without it. isOffered() says whether either shows.
void SessionEndedPanel::setVisible(bool visible)
{
    m_offered = visible;
    if (visible)
        offerInStrip();
    else if (m_style == Style::Bar && parentWidget())
        TabPrompt::of(parentWidget())->clear();
    QFrame::setVisible(visible && !usesStrip());
}

bool SessionEndedPanel::usesStrip() const
{
    return m_style == Style::Bar && parentWidget() && TabPrompt::inStrip();
}

void SessionEndedPanel::offerInStrip()
{
    if (m_style != Style::Bar || !parentWidget())
        return;
    if (!m_hubConnected) {
        m_hubConnected = true;
        // The strip turned on or off: the bar takes over, or hands over.
        connect(TabPrompt::hub(), &TabPrompt::changed, this, [this] {
            if (m_offered && isHidden() != usesStrip()) {
                QFrame::setVisible(!usesStrip());
                offerInStrip();
            }
        });
    }
    const QString text = m_detail->text().isEmpty() ? m_title->text()
                                                    : QStringLiteral("%1: %2").arg(m_title->text(), m_detail->text());
    TabPrompt::of(parentWidget())
        ->set(text, m_error,
              {{QStringLiteral("Reconnect"), m_reconnect->toolTip(), [this] { emit reconnectRequested(); }},
               {QStringLiteral("Close Tab"), m_close->toolTip(), [this] { emit closeRequested(); }}});
}
