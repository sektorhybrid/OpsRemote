// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "newentry.h"

#include <QComboBox>
#include <QPointer>
#include <QTimer>

namespace {

constexpr int NewRole = Qt::UserRole + 77;
const char *PreviousIndex = "newEntryPreviousIndex";
const char *PreviousText = "newEntryPreviousText";

void remember(QComboBox *combo)
{
    const int index = combo->currentIndex();
    if (NewEntry::isNew(combo, index))
        return;
    combo->setProperty(PreviousIndex, index);
    combo->setProperty(PreviousText, combo->currentText());
}

}

namespace NewEntry {

bool isNew(const QComboBox *combo, int index)
{
    return index >= 0 && combo->itemData(index, NewRole).toBool();
}

void add(QComboBox *combo, SessionMaker make, SessionChoices choices,
         std::function<QString(const QString &label)> text,
         std::function<QVariant(const QString &id)> data)
{
    combo->addItem(QStringLiteral("New…"));
    const int entry = combo->count() - 1;
    combo->setItemData(entry, true, NewRole);
    combo->setItemData(entry, QStringLiteral("Opens a new window to add one"), Qt::ToolTipRole);
    remember(combo);
    QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [combo] { remember(combo); });
    QObject::connect(combo, &QComboBox::editTextChanged, combo, [combo] { remember(combo); });
    QObject::connect(combo, &QComboBox::activated, combo, [=](int index) {
        if (!isNew(combo, index))
            return;
        // Back to the previous choice at once (a cancel leaves it so), then
        // the window, once the list has finished handling the click.
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(combo->property(PreviousIndex).toInt());
        if (combo->isEditable())
            combo->setEditText(combo->property(PreviousText).toString());
        QPointer<QComboBox> guard(combo);
        QTimer::singleShot(0, combo, [guard, make, choices, text, data] {
            const QString id = make(guard ? guard->window() : nullptr);
            if (!guard || id.isEmpty())
                return;
            for (const auto &[choiceId, label] : choices()) {
                if (choiceId != id)
                    continue;
                int at = guard->count() - 1;
                while (at >= 0 && !isNew(guard, at))
                    --at;
                if (at < 0)
                    at = guard->count();
                guard->insertItem(at, text(label), data(id));
                guard->setCurrentIndex(at);
                emit guard->activated(at);
                return;
            }
        });
    });
}

}
