// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "roundedcorners.h"

#include <QEvent>
#include <QFrame>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace {

enum Corner { TopLeft, TopRight, BottomLeft, BottomRight };

int cornerRadius = 0;
QList<QPointer<RoundedCorners>> everyCorners;

class CornerPiece final : public QWidget
{
public:
    CornerPiece(Corner corner, QWidget *parent) : QWidget(parent), m_corner(corner)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }
    QColor border; // the area's own border, curved here too (invalid: none)

protected:
    void paintEvent(QPaintEvent *) override
    {
        // The square less a quarter circle centred on the inner corner.
        const qreal side = width();
        const QPointF centre(m_corner == TopLeft || m_corner == BottomLeft ? side : 0,
                             m_corner == TopLeft || m_corner == TopRight ? side : 0);
        QPainterPath outside;
        outside.addRect(rect());
        QPainterPath inside;
        inside.addEllipse(centre, side, side);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(outside.subtracted(inside), parentWidget()->palette().color(QPalette::Window));
        if (border.isValid()) {
            // Meets the straight border lines (1 px, on the area's edge) at the
            // square's sides.
            painter.setPen(QPen(border, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(centre, side - 0.5, side - 0.5);
        }
    }

private:
    Corner m_corner;
};

}

RoundedCorners::RoundedCorners(QWidget *parent) : QObject(parent)
{
    for (int corner = TopLeft; corner <= BottomRight; ++corner) {
        m_pieces[corner] = new CornerPiece(Corner(corner), parent);
        m_pieces[corner]->hide();
    }
    everyCorners.append(this);
}

int RoundedCorners::radius()
{
    return cornerRadius;
}

void RoundedCorners::setRadius(int radius)
{
    if (radius == cornerRadius)
        return;
    cornerRadius = radius;
    everyCorners.removeAll(nullptr);
    for (const auto &corners : std::as_const(everyCorners))
        corners->setArea(corners->m_area);
}

void RoundedCorners::follow(QWidget *target)
{
    auto *corners = new RoundedCorners(target->parentWidget());
    corners->m_target = target;
    target->installEventFilter(corners);
    corners->setArea(target->geometry());
}

bool RoundedCorners::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::Hide: {
        auto *target = static_cast<QWidget *>(watched);
        setArea(target->isVisibleTo(target->parentWidget()) ? target->geometry() : QRect());
        break;
    }
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
        setArea(m_area); // the border's colour again
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void RoundedCorners::setArea(const QRect &area)
{
    m_area = area;
    const int side = radius();
    const bool shown = side > 0 && area.width() > 2 * side && area.height() > 2 * side;
    const QPoint at[4] = {area.topLeft(), {area.right() + 1 - side, area.top()},
                          {area.left(), area.bottom() + 1 - side},
                          {area.right() + 1 - side, area.bottom() + 1 - side}};
    for (int corner = TopLeft; corner <= BottomRight; ++corner) {
        m_pieces[corner]->setVisible(shown);
        if (!shown)
            continue;
        m_pieces[corner]->setGeometry(QRect(at[corner], QSize(side, side)));
        m_pieces[corner]->raise();
    }
    // A framed target (a list): its border's colour, as the style drew it
    // half way down its left edge, once it has been drawn.
    auto *frame = qobject_cast<QFrame *>(m_target.data());
    if (shown && frame && frame->frameWidth() > 0) {
        QTimer::singleShot(0, this, [this] {
            auto *frame = qobject_cast<QFrame *>(m_target.data());
            if (!frame || !frame->isVisible())
                return;
            const QColor border = frame->grab(QRect(0, frame->height() / 2, 1, 1)).toImage().pixelColor(0, 0);
            for (auto *piece : m_pieces) {
                auto *corner = static_cast<CornerPiece *>(piece);
                if (corner->border != border) {
                    corner->border = border;
                    corner->update();
                }
            }
        });
    }
}
