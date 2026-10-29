// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "opsremotestyle.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>

namespace {

constexpr qreal Radius = 6.0;

QColor mix(const QColor &a, const QColor &b, qreal amount)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount,
                            a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount);
}

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

// The outline of controls: the text colour faded into the background.
QColor outline(const QPalette &palette)
{
    return mix(palette.color(QPalette::Window), palette.color(QPalette::WindowText),
               isDark(palette) ? 0.28 : 0.22);
}

void roundedPanel(QPainter *painter, const QRectF &rect, const QColor &fill, const QColor &border,
                  qreal borderWidth = 1.0)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF inner = rect.adjusted(borderWidth / 2, borderWidth / 2, -borderWidth / 2,
                                       -borderWidth / 2);
    painter->setPen(border.isValid() ? QPen(border, borderWidth) : Qt::NoPen);
    painter->setBrush(fill);
    painter->drawRoundedRect(inner, Radius, Radius);
    painter->restore();
}

}

OpsRemoteStyle::OpsRemoteStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
{
    setObjectName(key());
}

// Hover effects need hover events on these widgets.
void OpsRemoteStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);
    if (qobject_cast<QAbstractButton *>(widget) || qobject_cast<QComboBox *>(widget)
        || qobject_cast<QScrollBar *>(widget) || qobject_cast<QTabBar *>(widget)
        || qobject_cast<QAbstractSpinBox *>(widget))
        widget->setAttribute(Qt::WA_Hover);
}

void OpsRemoteStyle::unpolish(QWidget *widget)
{
    QProxyStyle::unpolish(widget);
}

int OpsRemoteStyle::pixelMetric(PixelMetric metric, const QStyleOption *option,
                                const QWidget *widget) const
{
    switch (metric) {
    case PM_ScrollBarExtent:
        return 11;
    case PM_ScrollBarSliderMin:
        return 28;
    case PM_ButtonMargin:
        return 10;
    case PM_TabBarTabHSpace:
        return 22;
    case PM_TabBarTabVSpace:
        return 10;
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight:
        return 16;
    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

int OpsRemoteStyle::styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                              QStyleHintReturn *returnData) const
{
    switch (hint) {
    case SH_ScrollBar_Transient:
        return 0;
    case SH_ScrollBar_LeftClickAbsolutePosition:
        return 1;
    default:
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
}

void OpsRemoteStyle::drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                                   QPainter *painter, const QWidget *widget) const
{
    const QPalette &palette = option->palette;
    const bool enabled = option->state & State_Enabled;
    const bool hover = enabled && (option->state & State_MouseOver);
    const bool sunken = option->state & (State_Sunken | State_On);
    switch (element) {
    case PE_PanelButtonCommand:
    case PE_PanelButtonBevel: {
        QColor fill = palette.color(QPalette::Button);
        fill = isDark(palette) ? fill.lighter(118) : fill.darker(102);
        if (sunken)
            fill = mix(fill, palette.color(QPalette::Highlight), 0.25);
        else if (hover)
            fill = mix(fill, palette.color(QPalette::Highlight), 0.10);
        QColor border = outline(palette);
        if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option);
            button && (button->features & QStyleOptionButton::DefaultButton) && enabled)
            border = palette.color(QPalette::Highlight);
        if (!enabled)
            fill = mix(fill, palette.color(QPalette::Window), 0.5);
        roundedPanel(painter, option->rect, fill, border);
        return;
    }
    case PE_PanelButtonTool:
        if (!(hover || sunken))
            return; // flat until hovered, as in toolbars
        roundedPanel(painter, option->rect,
                     mix(palette.color(QPalette::Window), palette.color(QPalette::Highlight),
                         sunken ? 0.30 : 0.15),
                     QColor());
        return;
    case PE_FrameLineEdit:
    case PE_PanelLineEdit: {
        // A frame inside a combo or spin box is drawn by that control.
        if (widget && widget->parentWidget()
            && (qobject_cast<const QComboBox *>(widget->parentWidget())
                || qobject_cast<const QAbstractSpinBox *>(widget->parentWidget())))
            return;
        const bool focus = option->state & State_HasFocus;
        roundedPanel(painter, option->rect,
                     enabled ? palette.color(QPalette::Base)
                             : mix(palette.color(QPalette::Base), palette.color(QPalette::Window), 0.5),
                     focus ? palette.color(QPalette::Highlight) : outline(palette), focus ? 1.6 : 1.0);
        return;
    }
    case PE_FrameFocusRect:
        return; // focus shows as the accent border instead
    case PE_FrameTabWidget:
        roundedPanel(painter, option->rect, palette.color(QPalette::Window), outline(palette));
        return;
    case PE_IndicatorCheckBox: {
        const QRectF box = QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool on = option->state & State_On;
        const bool partial = option->state & State_NoChange;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(on || partial ? palette.color(QPalette::Highlight)
                                           : (hover ? palette.color(QPalette::Highlight) : outline(palette)),
                             1.2));
        painter->setBrush(on || partial ? palette.color(QPalette::Highlight) : palette.color(QPalette::Base));
        painter->drawRoundedRect(box, 4, 4);
        painter->setPen(QPen(palette.color(QPalette::HighlightedText), 2.0, Qt::SolidLine,
                             Qt::RoundCap, Qt::RoundJoin));
        if (on) {
            const QPointF a(box.left() + box.width() * 0.25, box.top() + box.height() * 0.52);
            const QPointF b(box.left() + box.width() * 0.43, box.top() + box.height() * 0.70);
            const QPointF c(box.left() + box.width() * 0.76, box.top() + box.height() * 0.32);
            painter->drawPolyline(QList<QPointF>{a, b, c});
        } else if (partial) {
            painter->drawLine(QPointF(box.left() + 4, box.center().y()),
                              QPointF(box.right() - 4, box.center().y()));
        }
        painter->restore();
        return;
    }
    case PE_IndicatorRadioButton: {
        const QRectF circle = QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool on = option->state & State_On;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(on || hover ? palette.color(QPalette::Highlight) : outline(palette), 1.2));
        painter->setBrush(palette.color(QPalette::Base));
        painter->drawEllipse(circle);
        if (on) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(palette.color(QPalette::Highlight));
            painter->drawEllipse(circle.adjusted(4, 4, -4, -4));
        }
        painter->restore();
        return;
    }
    default:
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
}

void OpsRemoteStyle::drawControl(ControlElement element, const QStyleOption *option,
                                 QPainter *painter, const QWidget *widget) const
{
    if (element == CE_TabBarTabShape) {
        // Flat tabs: the current one stands on the page colour with an
        // accent line; the others only react to hovering.
        const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option);
        if (!tab) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }
        const QPalette &palette = option->palette;
        const bool selected = tab->state & State_Selected;
        const bool hover = (tab->state & State_MouseOver) && (tab->state & State_Enabled);
        const bool vertical = tab->shape == QTabBar::RoundedWest || tab->shape == QTabBar::RoundedEast
                              || tab->shape == QTabBar::TriangularWest
                              || tab->shape == QTabBar::TriangularEast;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF rect = QRectF(tab->rect).adjusted(1, 2, -1, 0);
        if (selected || hover) {
            QColor fill = selected ? palette.color(QPalette::Base)
                                   : mix(palette.color(QPalette::Window),
                                         palette.color(QPalette::Highlight), 0.10);
            if (selected && !isDark(palette))
                fill = palette.color(QPalette::Base);
            QPainterPath path;
            path.addRoundedRect(rect, Radius, Radius);
            painter->fillPath(path, fill);
        }
        if (selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(palette.color(QPalette::Highlight));
            const QRectF line = vertical ? QRectF(rect.left(), rect.top() + 4, 3, rect.height() - 8)
                                         : QRectF(rect.left() + 6, rect.bottom() - 3, rect.width() - 12, 3);
            painter->drawRoundedRect(line, 1.5, 1.5);
        }
        painter->restore();
        return;
    }
    if (element == CE_TabBarTabLabel) {
        // Inactive tabs in a quieter colour.
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option);
            tab && !(tab->state & State_Selected)) {
            QStyleOptionTab quiet(*tab);
            quiet.palette.setColor(QPalette::WindowText,
                                   mix(tab->palette.color(QPalette::Window),
                                       tab->palette.color(QPalette::WindowText), 0.75));
            QProxyStyle::drawControl(element, &quiet, painter, widget);
            return;
        }
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

void OpsRemoteStyle::drawComplexControl(ComplexControl control, const QStyleOptionComplex *option,
                                        QPainter *painter, const QWidget *widget) const
{
    const QPalette &palette = option->palette;
    if (control == CC_ScrollBar) {
        const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option);
        if (!bar)
            return;
        const bool horizontal = bar->orientation == Qt::Horizontal;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->fillRect(bar->rect, palette.color(QPalette::Window));
        QRectF slider = subControlRect(control, option, SC_ScrollBarSlider, widget);
        slider = horizontal ? slider.adjusted(1, 2.5, -1, -2.5) : slider.adjusted(2.5, 1, -2.5, -1);
        const bool hover = (bar->state & State_MouseOver)
                           && bar->activeSubControls & SC_ScrollBarSlider;
        const bool pressed = (bar->state & State_Sunken) && bar->activeSubControls & SC_ScrollBarSlider;
        QColor fill = mix(palette.color(QPalette::Window), palette.color(QPalette::WindowText), 0.30);
        if (hover || pressed)
            fill = mix(fill, palette.color(QPalette::Highlight), pressed ? 0.8 : 0.5);
        if (bar->maximum > bar->minimum) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            const qreal round = (horizontal ? slider.height() : slider.width()) / 2;
            painter->drawRoundedRect(slider, round, round);
        }
        painter->restore();
        return;
    }
    if (control == CC_ComboBox) {
        const auto *combo = qstyleoption_cast<const QStyleOptionComboBox *>(option);
        if (!combo) {
            QProxyStyle::drawComplexControl(control, option, painter, widget);
            return;
        }
        const bool enabled = combo->state & State_Enabled;
        const bool hover = enabled && (combo->state & State_MouseOver);
        const bool focus = combo->state & State_HasFocus;
        QColor fill = combo->editable ? palette.color(QPalette::Base) : palette.color(QPalette::Button);
        if (!combo->editable)
            fill = isDark(palette) ? fill.lighter(118) : fill.darker(102);
        if (hover && !combo->editable)
            fill = mix(fill, palette.color(QPalette::Highlight), 0.10);
        roundedPanel(painter, combo->rect, fill,
                     focus && combo->editable ? palette.color(QPalette::Highlight) : outline(palette),
                     focus && combo->editable ? 1.6 : 1.0);
        QStyleOption arrow(*combo);
        const QRect arrowRect = subControlRect(control, option, SC_ComboBoxArrow, widget);
        arrow.rect = QRect(0, 0, 9, 9);
        arrow.rect.moveCenter(arrowRect.center());
        proxy()->drawPrimitive(PE_IndicatorArrowDown, &arrow, painter, widget);
        return;
    }
    QProxyStyle::drawComplexControl(control, option, painter, widget);
}

QRect OpsRemoteStyle::subControlRect(ComplexControl control, const QStyleOptionComplex *option,
                                     SubControl subControl, const QWidget *widget) const
{
    if (control != CC_ScrollBar)
        return QProxyStyle::subControlRect(control, option, subControl, widget);
    // No arrow buttons: the whole bar is the groove.
    const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option);
    if (!bar)
        return {};
    const QRect rect = bar->rect;
    const bool horizontal = bar->orientation == Qt::Horizontal;
    const int length = horizontal ? rect.width() : rect.height();
    const qint64 range = qint64(bar->maximum) - bar->minimum;
    int sliderLength = length;
    if (range > 0)
        sliderLength = int(qint64(length) * bar->pageStep / (range + bar->pageStep));
    sliderLength = qBound(qMin(length, proxy()->pixelMetric(PM_ScrollBarSliderMin, bar, widget)),
                          sliderLength, length);
    const int position = sliderPositionFromValue(bar->minimum, bar->maximum, bar->sliderPosition,
                                                 length - sliderLength, bar->upsideDown);
    const auto span = [&](int start, int size) {
        return horizontal ? QRect(rect.x() + start, rect.y(), size, rect.height())
                          : QRect(rect.x(), rect.y() + start, rect.width(), size);
    };
    switch (subControl) {
    case SC_ScrollBarSlider:
        return span(position, sliderLength);
    case SC_ScrollBarSubPage:
        return span(0, position);
    case SC_ScrollBarAddPage:
        return span(position + sliderLength, length - position - sliderLength);
    case SC_ScrollBarGroove:
        return rect;
    case SC_ScrollBarAddLine:
    case SC_ScrollBarSubLine:
    case SC_ScrollBarFirst:
    case SC_ScrollBarLast:
    default:
        return {};
    }
}
