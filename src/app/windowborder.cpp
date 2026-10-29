// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "windowborder.h"

#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QPointer>
#include <QWidget>

namespace {

// Drawn on top of the window's contents; mouse events pass through it.
class BorderOverlay final : public QWidget
{
public:
    explicit BorderOverlay(QWidget *window) : QWidget(window)
    {
        setObjectName(QStringLiteral("windowBorderOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(window->rect());
        raise();
        show();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        // Text colour at low opacity contrasts in light and dark themes alike.
        QColor colour = palette().color(QPalette::WindowText);
        colour.setAlphaF(0.35);
        QPainter painter(this);
        painter.setPen(colour);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};

bool isFramedWindow(const QWidget *widget)
{
    if (!widget->isWindow())
        return false;
    const auto type = widget->windowType();
    return type == Qt::Window || type == Qt::Dialog;
}

QWidget *overlayOf(QWidget *window)
{
    return window->findChild<QWidget *>(QStringLiteral("windowBorderOverlay"),
                                        Qt::FindDirectChildrenOnly);
}

class WindowBorders final : public QObject
{
public:
    bool enabled = false;

    void addTo(QWidget *window)
    {
        if (!overlayOf(window))
            new BorderOverlay(window);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (!enabled || !object->isWidgetType())
            return false;
        auto *widget = static_cast<QWidget *>(object);
        switch (event->type()) {
        case QEvent::Show:
            if (isFramedWindow(widget))
                addTo(widget);
            break;
        case QEvent::Resize:
            if (auto *overlay = isFramedWindow(widget) ? overlayOf(widget) : nullptr)
                overlay->setGeometry(widget->rect());
            break;
        case QEvent::ChildAdded:
            // Keep the outline above widgets added later, such as new panels.
            if (auto *overlay = isFramedWindow(widget) ? overlayOf(widget) : nullptr)
                overlay->raise();
            break;
        default:
            break;
        }
        return false;
    }
};

WindowBorders *borders()
{
    static QPointer<WindowBorders> instance;
    if (!instance) {
        instance = new WindowBorders;
        instance->setParent(qApp);
        qApp->installEventFilter(instance);
    }
    return instance;
}

}

void setWindowBordersEnabled(bool enabled)
{
    auto *filter = borders();
    filter->enabled = enabled;
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (!isFramedWindow(widget))
            continue;
        if (enabled && widget->isVisible())
            filter->addTo(widget);
        else if (!enabled)
            delete overlayOf(widget);
    }
}

bool windowBordersEnabled()
{
    return borders()->enabled;
}

bool defaultWindowBorders()
{
#ifdef Q_OS_WIN
    return false;
#else
    return true;
#endif
}
