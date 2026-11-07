// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "clipboard.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextEdit>

namespace Clipboard {

namespace {

MouseClipboardPreferences current;

// When each source last changed, for "newest": a selection made in
// OpsRemote (with its text), the clipboard (any program), and on Linux the
// selection of another program.
struct Memory
{
    QString ownText;
    qint64 ownAt = 0;
    qint64 clipboardAt = 0;
    qint64 otherSelectionAt = 0;
};

Memory &memory()
{
    static Memory value;
    static const bool watching = [] {
        auto *clipboard = QApplication::clipboard();
        QObject::connect(clipboard, &QClipboard::dataChanged, qApp,
                         [] { value.clipboardAt = QDateTime::currentMSecsSinceEpoch(); });
        QObject::connect(clipboard, &QClipboard::selectionChanged, qApp, [clipboard] {
            if (!clipboard->ownsSelection())
                value.otherSelectionAt = QDateTime::currentMSecsSinceEpoch();
        });
        return true;
    }();
    Q_UNUSED(watching);
    return value;
}

} // namespace

const MouseClipboardPreferences &preferences()
{
    return current;
}

void setPreferences(const MouseClipboardPreferences &value)
{
    memory(); // watching from the start
    if (value == current)
        return;
    current = value;
    emit notifier()->changed();
}

Notifier *notifier()
{
    static auto *instance = new Notifier;
    return instance;
}

QString wordCharacters()
{
    return current.doubleClick == QStringLiteral("letters") ? QString() : current.wordCharacters;
}

bool isWordCharacter(QChar c)
{
    return c.isLetterOrNumber() || wordCharacters().contains(c);
}

void selectionFinished(const QString &text)
{
    if (text.isEmpty())
        return;
    auto &remembered = memory();
    remembered.ownText = text;
    remembered.ownAt = QDateTime::currentMSecsSinceEpoch();
    if (current.copyOnSelect)
        QApplication::clipboard()->setText(text);
}

QString textToPaste(bool middleClick)
{
    auto *clipboard = QApplication::clipboard();
    const bool selections = clipboard->supportsSelection();
    if (current.paste == QStringLiteral("clipboard"))
        return clipboard->text();
    if (current.paste == QStringLiteral("system"))
        return clipboard->text(middleClick && selections ? QClipboard::Selection : QClipboard::Clipboard);
    // The newest; copy on select puts a selection on the clipboard too, so
    // either way it is what was picked last.
    const auto &remembered = memory();
    const qint64 other = selections && current.otherProgramsSelection ? remembered.otherSelectionAt : 0;
    if (other > remembered.ownAt && other > remembered.clipboardAt)
        return clipboard->text(QClipboard::Selection);
    if (!remembered.ownText.isEmpty() && remembered.ownAt > remembered.clipboardAt)
        return remembered.ownText;
    return clipboard->text();
}

namespace {

// The field an event belongs to: a line edit, or a text editor (whose
// mouse events go to its viewport).
QWidget *fieldOf(QObject *watched)
{
    if (auto *edit = qobject_cast<QLineEdit *>(watched))
        return edit;
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget)
        return nullptr;
    auto *parent = widget->parentWidget();
    if (qobject_cast<QPlainTextEdit *>(parent) || qobject_cast<QTextEdit *>(parent))
        return static_cast<QAbstractScrollArea *>(parent)->viewport() == widget ? parent : nullptr;
    return nullptr;
}

bool editable(QWidget *field)
{
    if (auto *edit = qobject_cast<QLineEdit *>(field))
        return !edit->isReadOnly() && edit->isEnabled();
    if (auto *edit = qobject_cast<QPlainTextEdit *>(field))
        return !edit->isReadOnly() && edit->isEnabled();
    if (auto *edit = qobject_cast<QTextEdit *>(field))
        return !edit->isReadOnly() && edit->isEnabled();
    return false;
}

// Text that never leaves its field by selecting (passwords).
bool secret(QWidget *field)
{
    auto *edit = qobject_cast<QLineEdit *>(field);
    return edit && edit->echoMode() != QLineEdit::Normal;
}

QString selectedText(QWidget *field)
{
    if (auto *edit = qobject_cast<QLineEdit *>(field))
        return edit->selectedText();
    if (auto *edit = qobject_cast<QPlainTextEdit *>(field))
        return edit->textCursor().selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    if (auto *edit = qobject_cast<QTextEdit *>(field))
        return edit->textCursor().selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return QString();
}

// The word at a position of a line of text: [start, end).
std::pair<int, int> wordAt(const QString &line, int position)
{
    int start = qBound(0, position, int(line.size()));
    int end = start;
    while (start > 0 && isWordCharacter(line.at(start - 1)))
        --start;
    while (end < line.size() && isWordCharacter(line.at(end)))
        ++end;
    return {start, end};
}

// The field's text cursor (text editors) at a point of its viewport.
template <typename Edit>
void selectWord(Edit *edit, const QPoint &point)
{
    auto cursor = edit->cursorForPosition(point);
    const auto block = cursor.block();
    const auto [start, end] = wordAt(block.text(), cursor.positionInBlock());
    if (start == end)
        return;
    cursor.setPosition(block.position() + start);
    cursor.setPosition(block.position() + end, QTextCursor::KeepAnchor);
    edit->setTextCursor(cursor);
}

void insertAt(QWidget *field, const QString &text, const QPoint *point)
{
    if (text.isEmpty())
        return;
    if (auto *edit = qobject_cast<QLineEdit *>(field)) {
        if (point)
            edit->setCursorPosition(edit->cursorPositionAt(*point));
        // One line: the first of several (as Qt's own paste does).
        edit->insert(text.section(QLatin1Char('\n'), 0, 0));
    } else if (auto *edit = qobject_cast<QPlainTextEdit *>(field)) {
        if (point)
            edit->setTextCursor(edit->cursorForPosition(*point));
        edit->insertPlainText(text);
    } else if (auto *edit = qobject_cast<QTextEdit *>(field)) {
        if (point)
            edit->setTextCursor(edit->cursorForPosition(*point));
        edit->insertPlainText(text);
    }
    field->setFocus(Qt::MouseFocusReason);
}

class TextFields final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const auto type = event->type();
        if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease
            && type != QEvent::MouseButtonDblClick && type != QEvent::ContextMenu)
            return false;
        auto *field = fieldOf(watched);
        if (!field || m_menuOpening)
            return false;
        if (type == QEvent::ContextMenu)
            return contextMenu(field, watched, static_cast<QContextMenuEvent *>(event));
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QPoint point = mouse->position().toPoint();
        if (mouse->button() == Qt::MiddleButton) {
            // Qt's own pastes the system selection on release: both are ours.
            if (type == QEvent::MouseButtonPress) {
                if (current.middleClick == QStringLiteral("paste") && editable(field))
                    insertAt(field, textToPaste(true), &point);
                else if (current.middleClick == QStringLiteral("menu"))
                    openMenu(field, watched, mouse->globalPosition().toPoint(), point);
            }
            return true;
        }
        if (mouse->button() != Qt::LeftButton || secret(field))
            return false;
        if (type == QEvent::MouseButtonDblClick) {
            if (auto *edit = qobject_cast<QLineEdit *>(field)) {
                const auto [start, end] = wordAt(edit->text(), edit->cursorPositionAt(point));
                if (start < end)
                    edit->setSelection(start, end - start);
            } else if (auto *edit = qobject_cast<QPlainTextEdit *>(field)) {
                selectWord(edit, point);
            } else if (auto *edit = qobject_cast<QTextEdit *>(field)) {
                selectWord(edit, point);
            }
            selectionFinished(selectedText(field));
            return true;
        }
        if (type == QEvent::MouseButtonRelease)
            selectionFinished(selectedText(field));
        return false;
    }

private:
    // Right-click: the menu, or a paste (Shift does the other), or nothing.
    bool contextMenu(QWidget *field, QObject *watched, QContextMenuEvent *event)
    {
        if (event->reason() != QContextMenuEvent::Mouse)
            return false;
        Q_UNUSED(watched);
        const auto &action = current.rightClick;
        // Shift: the menu, or with the menu set, a paste.
        if (event->modifiers() & Qt::ShiftModifier)
            return action == QStringLiteral("menu") ? pasteHere(field) : false;
        if (action == QStringLiteral("menu"))
            return false;
        if (action == QStringLiteral("paste"))
            return pasteHere(field);
        return true; // nothing
    }

    bool pasteHere(QWidget *field)
    {
        if (editable(field))
            insertAt(field, textToPaste(false), nullptr);
        return true;
    }

    void openMenu(QWidget *field, QObject *watched, const QPoint &global, const QPoint &local)
    {
        Q_UNUSED(field);
        m_menuOpening = true;
        QContextMenuEvent menu(QContextMenuEvent::Mouse, local, global);
        QCoreApplication::sendEvent(watched, &menu);
        m_menuOpening = false;
    }

    bool m_menuOpening = false;
};

} // namespace

void installTextFieldBehaviour()
{
    static auto *filter = new TextFields(qApp);
    qApp->installEventFilter(filter);
}

} // namespace Clipboard
