// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "formcard.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QTableView>
#include <QBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>

namespace {

QFrame *card(const QString &title, QWidget *parent, QVBoxLayout **inside)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("formCard"));
    card->setStyleSheet(QStringLiteral("#formCard { border: 1px solid palette(mid); border-radius: 6px; }"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 4, 8, 6);
    layout->setSpacing(2);
    auto *heading = new QLabel(title, card);
    auto font = heading->font();
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
    *inside = layout;
    return card;
}

} // namespace

QFormLayout *addFormCard(QBoxLayout *page, const QString &title, QWidget *parent)
{
    QVBoxLayout *layout = nullptr;
    page->addWidget(card(title, parent, &layout));
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setVerticalSpacing(4);
    layout->addLayout(form);
    return form;
}

QWidget *addWidgetCard(QBoxLayout *page, const QString &title, QWidget *content)
{
    QVBoxLayout *layout = nullptr;
    auto *frame = card(title, page->parentWidget(), &layout);
    if (content->layout())
        content->layout()->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(content);
    page->addWidget(frame);
    return frame;
}

void showRows(QAbstractItemView *view, int rows)
{
    int row = view->sizeHintForRow(0) > 0 ? view->sizeHintForRow(0) : view->fontMetrics().height() + 6;
    int header = 0;
    if (auto *table = qobject_cast<QTableView *>(view)) {
        row = table->verticalHeader()->defaultSectionSize();
        if (!table->horizontalHeader()->isHidden())
            header = table->horizontalHeader()->sizeHint().height();
    }
    view->setFixedHeight(header + rows * row + 2 * view->frameWidth());
}

void alignFormLabels(QWidget *root)
{
    const auto forms = root->findChildren<QFormLayout *>();
    // At least the same width in every session type's window, so fields
    // start at the same place from tab to tab.
    int width = root->fontMetrics().horizontalAdvance(QStringLiteral("Files follow folder:")) + 8;
    for (auto *form : forms)
        for (int row = 0; row < form->rowCount(); ++row)
            if (auto *item = form->itemAt(row, QFormLayout::LabelRole); item && item->widget())
                width = qMax(width, item->widget()->sizeHint().width());
    for (auto *form : forms)
        for (int row = 0; row < form->rowCount(); ++row)
            if (auto *item = form->itemAt(row, QFormLayout::LabelRole); item && item->widget())
                item->widget()->setMinimumWidth(width);
}
