// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "tabactivity.h"

#include <QPainter>
#include <QVariantAnimation>

#include <cmath>

namespace {

qreal luminance(const QColor &colour)
{
    auto channel = [](qreal value) {
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF())
           + 0.0722 * channel(colour.blueF());
}

qreal contrast(const QColor &a, const QColor &b)
{
    const qreal x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

QColor blend(const QColor &a, const QColor &b, qreal amount)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * amount,
                            a.greenF() + (b.greenF() - a.greenF()) * amount,
                            a.blueF() + (b.blueF() - a.blueF()) * amount,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * amount);
}

} // namespace

QColor visibleColor(const QColor &background, const QList<QColor> &candidates)
{
    QColor best;
    qreal bestContrast = 0;
    for (const auto &colour : candidates) {
        if (!colour.isValid())
            continue;
        const qreal ratio = contrast(colour, background);
        if (ratio >= 3.0)
            return colour;
        if (ratio > bestContrast) {
            bestContrast = ratio;
            best = colour;
        }
    }
    return best;
}

ActivityDot::ActivityDot(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedSize(sizeHint());
    m_animation = new QVariantAnimation(this);
    m_animation->setStartValue(0.0);
    m_animation->setEndValue(1.0);
    m_animation->setDuration(1800);
    m_animation->setLoopCount(-1);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        // Out and back in one loop, easing at both ends.
        m_phase = (1.0 - std::cos(value.toReal() * 2.0 * M_PI)) / 2.0;
        update();
    });
}

void ActivityDot::setColors(const QColor &first, const QColor &second)
{
    m_first = first;
    m_second = second;
    update();
}

void ActivityDot::setPulsing(bool on)
{
    m_pulsing = on;
    const bool run = m_active && m_pulsing;
    if (run && m_animation->state() != QAbstractAnimation::Running) {
        m_animation->start();
    } else if (!run) {
        m_animation->stop();
        m_phase = 0;
        update();
    }
}

void ActivityDot::setActive(bool on)
{
    m_active = on;
    setPulsing(m_pulsing);
    update();
}

QSize ActivityDot::sizeHint() const
{
    const int side = std::max(10, fontMetrics().height() * 2 / 3);
    return {side, side};
}

void ActivityDot::paintEvent(QPaintEvent *)
{
    if (!m_active)
        return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(blend(m_first, m_second, m_phase));
    const qreal diameter = width() * 0.7;
    painter.drawEllipse(QRectF((width() - diameter) / 2, (height() - diameter) / 2, diameter, diameter));
}
