// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "windowfit.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QMainWindow>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QWidget>

namespace WindowFit {

namespace {

constexpr char FittedProperty[] = "opsremoteWindowFitted";

// Space the window manager adds around a window that is not shown yet
// (a title bar above, thin borders): its real frame once it has one.
QMargins frameMargins(const QWidget *window)
{
    const QRect frame = window->frameGeometry();
    const QRect inner = window->geometry();
    if (frame != inner)
        return {inner.left() - frame.left(), inner.top() - frame.top(), frame.right() - inner.right(),
                frame.bottom() - inner.bottom()};
    return {8, 32, 8, 8};
}

// natural: the size the content asks for (the window was not sized by
// hand or by saved settings), else its current size; both within the room.
void fit(QWidget *window, bool natural)
{
    QScreen *screen = window->screen();
    if (!screen)
        return;
    const QRect available = screen->availableGeometry();
    const QMargins margins = frameMargins(window);
    const QSize room(available.width() - margins.left() - margins.right(),
                     available.height() - margins.top() - margins.bottom());
    if (room.width() < 100 || room.height() < 100)
        return;

    QSize size = natural ? window->sizeHint().expandedTo(window->minimumSizeHint()) : window->size();
    size = size.boundedTo(room).expandedTo(window->minimumSize());
    if (size != window->size())
        window->resize(size);
    if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_WINDOW_FIT") > 0) {
        const QSize smallest = window->minimumSize().expandedTo(window->minimumSizeHint());
        qInfo("Window fit: \"%s\" (%s) %dx%d, smallest %dx%d, screen room %dx%d%s",
              qUtf8Printable(window->windowTitle()), window->metaObject()->className(), size.width(),
              size.height(), smallest.width(), smallest.height(), room.width(), room.height(),
              smallest.width() > room.width() || smallest.height() > room.height() ? "  TOO BIG" : "");
        // Which tab pages set that size.
        for (const auto *tabs : window->findChildren<QTabWidget *>()) {
            for (int i = 0; i < tabs->count(); ++i) {
                const QSize page = tabs->widget(i)->minimumSizeHint();
                const QSize hint = tabs->widget(i)->sizeHint();
                qInfo("Window fit:   tab \"%s\" %s smallest %dx%d, asks %dx%d", qUtf8Printable(tabs->tabText(i)),
                      tabs->widget(i)->metaObject()->className(), page.width(), page.height(), hint.width(),
                      hint.height());
            }
        }
    }

    // Onto the screen: the title bar first, so it can always be dragged.
    QPoint position = window->pos();
    const int widest = size.width() + margins.left() + margins.right();
    const int tallest = size.height() + margins.top() + margins.bottom();
    position.setX(qBound(available.left(), position.x(), available.right() + 1 - qMin(widest, available.width())));
    position.setY(qBound(available.top(), position.y(), available.bottom() + 1 - qMin(tallest, available.height())));
    if (position != window->pos())
        window->move(position);
}

// Asks for its page's whole size (QScrollArea's own hint stops at about 24
// lines), and is content with a few lines when its window can't give more.
class FitScrollArea final : public QScrollArea
{
public:
    using QScrollArea::QScrollArea;

    QSize sizeHint() const override
    {
        if (!widget())
            return QScrollArea::sizeHint();
        const int frame = 2 * frameWidth();
        QSize hint = widget()->sizeHint();
        // Wrapped text needs more lines at the width it really gets; a
        // scroll area doesn't pass that on to its window by itself.
        if (widget()->hasHeightForWidth()) {
            const int width = viewport()->width() > 50 ? viewport()->width() : hint.width();
            hint.setHeight(qMax(hint.height(), widget()->heightForWidth(width)));
        }
        return hint + QSize(frame, frame);
    }

    QSize minimumSizeHint() const override
    {
        if (!widget())
            return QScrollArea::minimumSizeHint();
        const QSize page = widget()->minimumSizeHint();
        const int frame = 2 * frameWidth();
        if (insideScrollArea())
            return page + QSize(frame, frame);
        // As wide as the page (it scrolls up and down, not sideways), with
        // room for the scroll bar; a few lines tall.
        const int bar = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);
        return {page.width() + bar + frame, qMin(page.height(), 6 * fontMetrics().height()) + frame};
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // The page's layout changed (a row appeared): its size reaches the
        // window only through this scroll area's own hints.
        if (watched == widget() && event->type() == QEvent::LayoutRequest)
            updateGeometry();
        return QScrollArea::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QScrollArea::resizeEvent(event);
        // Another width: another height for wrapped text (sizeHint).
        if (widget() && widget()->hasHeightForWidth() && event->oldSize().width() != event->size().width())
            updateGeometry();
    }

private:
    bool insideScrollArea() const
    {
        for (const QWidget *parent = parentWidget(); parent; parent = parent->parentWidget()) {
            if (qobject_cast<const QAbstractScrollArea *>(parent))
                return true;
            if (parent->isWindow())
                break;
        }
        return false;
    }
};

class Watcher final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Show || !object->isWidgetType())
            return false;
        auto *widget = static_cast<QWidget *>(object);
        // Windows of their own only: not menus, tooltips or drop-down lists,
        // and the main window, which restores the size the user left it at
        // (QWidget::restoreGeometry keeps it on the available screen).
        if (!widget->isWindow() || widget->property(FittedProperty).toBool())
            return false;
        const Qt::WindowType type = widget->windowType();
        if (type != Qt::Window && type != Qt::Dialog && type != Qt::Sheet)
            return false;
        if (qobject_cast<QMainWindow *>(widget))
            return false;
        widget->setProperty(FittedProperty, true);
        // Qt sized it itself unless it was resized before showing.
        fit(widget, !widget->testAttribute(Qt::WA_Resized));
        return false;
    }
};

} // namespace

void install()
{
    static auto *watcher = new Watcher(qApp);
    qApp->installEventFilter(watcher);
}

void adjustSize(QWidget *window)
{
    fit(window, true);
}

void grow(QWidget *window)
{
    if (!window->isVisible() || window->isMaximized() || window->isFullScreen())
        return;
    QScreen *screen = window->screen();
    if (!screen)
        return;
    const QMargins margins = frameMargins(window);
    const QSize room = screen->availableGeometry().size() - QSize(margins.left() + margins.right(),
                                                                   margins.top() + margins.bottom());
    const QSize wanted = window->size().expandedTo(window->sizeHint()).boundedTo(room);
    if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_WINDOW_FIT") > 0)
        qInfo("Window fit: \"%s\" grows from %dx%d to %dx%d", qUtf8Printable(window->windowTitle()),
              window->width(), window->height(), wanted.width(), wanted.height());
    if (wanted != window->size()) {
        window->resize(wanted);
        fit(window, false); // moved back onto the screen if it grew past it
    }
}

QScrollArea *scrollablePage(QWidget *page)
{
    auto *scroll = new FitScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);
    return scroll;
}

QWidget *scrollingContent(QWidget *widget)
{
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *content = new QWidget;
    layout->addWidget(scrollablePage(content));
    return content;
}

int indexOfPage(const QTabWidget *tabs, const QWidget *page)
{
    for (int i = 0; i < tabs->count(); ++i) {
        const QWidget *tab = tabs->widget(i);
        if (tab == page)
            return i;
        if (const auto *scroll = qobject_cast<const QScrollArea *>(tab); scroll && scroll->widget() == page)
            return i;
    }
    return -1;
}

} // namespace WindowFit
