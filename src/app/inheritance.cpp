// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "inheritance.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QFormLayout>
#include <QToolButton>

#include "preferences.h"

#include <functional>

namespace {

// Marks a drop-down's first entry as the inherited one.
constexpr int InheritedRole = Qt::UserRole + 100;

// The space before every field of a session window: the ↺ button's place,
// kept free on rows without one so all fields start at the same line.
int revertWidth()
{
    static const int width = QToolButton().fontMetrics().horizontalAdvance(QStringLiteral("↺")) + 8;
    return width;
}

// The row: the ↺ button once the session has its own value (its place kept
// free while inherited), then the control.
template <typename Control, typename Signal>
QWidget *row(Control *control, Signal changed, std::function<bool()> own, std::function<void()> reset)
{
    auto *widget = new QWidget;
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *slot = new QWidget(widget);
    slot->setFixedWidth(revertWidth());
    auto *slotLayout = new QHBoxLayout(slot);
    slotLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(slot);
    control->setParent(widget);
    layout->addWidget(control, 1);
    layout->addStretch();
    // Without the button (Appearance → Session list), only the free place.
    if (!loadSessionListPreferences().showRevertButtons)
        return widget;
    auto *back = new QToolButton(slot);
    back->setText(QStringLiteral("↺"));
    back->setAutoRaise(true);
    back->setFixedWidth(revertWidth());
    // No taller than the field, so the row keeps its height when it appears
    // (a style's tool button can be taller than a checkbox).
    back->setMaximumHeight(control->sizeHint().height());
    back->setToolTip(QStringLiteral("Revert to default"));
    slotLayout->addWidget(back);
    const auto show = [back, own] { back->setVisible(own()); };
    QObject::connect(control, changed, widget, show);
    QObject::connect(back, &QToolButton::clicked, widget, [reset, show] {
        reset();
        show();
    });
    show();
    return widget;
}

} // namespace

void addInheritedItem(QComboBox *box, const QString &text, const QVariant &data)
{
    insertInheritedItem(box, text, data);
}

void insertInheritedItem(QComboBox *box, const QString &text, const QVariant &data)
{
    box->insertItem(0, text, data);
    box->setItemData(0, true, InheritedRole);
}

QWidget *inheritable(QCheckBox *box, bool inheritedOn)
{
    return row(box, &QCheckBox::toggled, [box, inheritedOn] { return box->isChecked() != inheritedOn; },
               [box, inheritedOn] { box->setChecked(inheritedOn); });
}

QWidget *inheritable(QComboBox *box, const QString &)
{
    return row(box, &QComboBox::currentIndexChanged, [box] { return box->currentIndex() > 0; },
               [box] { box->setCurrentIndex(0); });
}

QWidget *inheritable(QLineEdit *edit, const QString &)
{
    return row(edit, &QLineEdit::textChanged, [edit] { return !edit->text().isEmpty(); },
               [edit] { edit->clear(); });
}

QWidget *inheritable(QSpinBox *box, const QString &)
{
    return row(box, &QSpinBox::valueChanged, [box] { return box->value() != box->minimum(); },
               [box] { box->setValue(box->minimum()); });
}

namespace {

// Whether a form field takes part (see applyInheritance()).
bool inherits(QWidget *field)
{
    if (auto *box = qobject_cast<QCheckBox *>(field))
        return box->property("inheritedOn").isValid();
    if (auto *box = qobject_cast<QComboBox *>(field))
        return !box->isEditable() && box->count() > 0 && box->itemData(0, InheritedRole).toBool();
    if (auto *box = qobject_cast<QSpinBox *>(field))
        return box->property("inherits").toBool();
    if (auto *edit = qobject_cast<QLineEdit *>(field))
        return edit->property("inheritedText").isValid();
    return false;
}

QWidget *wrapped(QWidget *field)
{
    if (auto *box = qobject_cast<QCheckBox *>(field))
        return inheritable(box, box->property("inheritedOn").toBool());
    if (auto *box = qobject_cast<QComboBox *>(field))
        return inheritable(box, box->itemText(0));
    if (auto *box = qobject_cast<QSpinBox *>(field))
        return inheritable(box, box->specialValueText());
    auto *edit = qobject_cast<QLineEdit *>(field);
    return inheritable(edit, edit->property("inheritedText").toString());
}

} // namespace

void applyInheritance(QWidget *editor)
{
    for (auto *form : editor->findChildren<QFormLayout *>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            auto *item = form->itemAt(row, QFormLayout::FieldRole);
            if (!item)
                continue;
            auto *field = item->widget();
            // Out of the form first, then into its row, then the row in its
            // place: moving it while the form still holds it loops the layout.
            form->removeItem(item);
            QWidget *replacement = nullptr;
            if (field && inherits(field)) {
                delete item; // the layout item only
                replacement = wrapped(field);
            } else {
                // Other fields start at the same line, after the ↺ place.
                auto *indented = new QWidget;
                auto *layout = new QHBoxLayout(indented);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(4);
                layout->addSpacing(revertWidth());
                if (field) {
                    delete item;
                    field->setParent(indented);
                    layout->addWidget(field, 1);
                    layout->addStretch();
                } else if (auto *inner = item->layout()) {
                    inner->setParent(nullptr);
                    layout->addLayout(inner, 1);
                } else {
                    delete item;
                }
                replacement = indented;
            }
            form->setWidget(row, QFormLayout::FieldRole, replacement);
        }
    }
}
