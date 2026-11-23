// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "availabilitynote.h"

#include "windowfit.h"
#include "workerthreads.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

AvailabilityNote::AvailabilityNote(std::function<QString()> problem, std::function<void(bool)> apply,
                                   QWidget *parent, Run run)
    : QWidget(parent), m_problem(std::move(problem)), m_apply(std::move(apply)), m_run(run)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_text = new QLabel(this);
    m_text->setWordWrap(true);
    m_text->setStyleSheet(QStringLiteral("color: #b9770e;"));
    auto *retry = new QPushButton(QStringLiteral("Check again"), this);
    layout->addWidget(m_text, 1);
    layout->addWidget(retry);
    connect(retry, &QPushButton::clicked, this, &AvailabilityNote::check);
    // Hidden until a problem is known (no flash of an empty note).
    hide();
    check();
}

void AvailabilityNote::check()
{
    const int generation = ++m_generation;
    if (m_run == Run::Here) {
        show(m_problem());
        return;
    }
    // A pooled worker thread (workerthreads.h: an ending thread stalls
    // Windows), given back once the answer is on its way.
    auto *worker = new QObject;
    worker->moveToThread(WorkerThreads::acquire(QStringLiteral("availability check")));
    QMetaObject::invokeMethod(worker, [worker, problem = m_problem, note = QPointer<AvailabilityNote>(this),
                                       generation] {
        QElapsedTimer timer;
        timer.start();
        const QString reason = problem();
        if (timer.elapsed() > 100)
            qInfo("availability check: %lld ms (%s)", qint64(timer.elapsed()),
                  reason.isEmpty() ? "available" : qUtf8Printable(reason.left(60)));
        QMetaObject::invokeMethod(qApp, [note, reason, generation] {
            if (note && generation == note->m_generation)
                note->show(reason);
        }, Qt::QueuedConnection);
        WorkerThreads::release(worker);
    }, Qt::QueuedConnection);
}

void AvailabilityNote::show(const QString &reason)
{
    m_text->setText(QStringLiteral("⚠ %1").arg(reason));
    // Without a parent yet (still being placed in a page), showing it would
    // open it as a window of its own for a moment; it appears with the page.
    if (reason.isEmpty()) {
        hide();
    } else if (parentWidget() && isHidden()) {
        QWidget::show();
        // A background answer comes after its window took its size: make
        // room for the note instead of a scroll bar (windowfit.h).
        if (m_run == Run::Background)
            QTimer::singleShot(0, window(), [window = window()] { WindowFit::grow(window); });
    }
    if (m_apply)
        m_apply(reason.isEmpty());
}
