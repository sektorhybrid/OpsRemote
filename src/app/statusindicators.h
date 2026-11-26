// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QAbstractButton>
#include <QPoint>
#include <functional>
#include <optional>

class QFrame;
class QPushButton;
class QVBoxLayout;

// Messages for the whole application (not about one tab): kept until
// cleared, newest first. Confirmations ("Saved session: X") are not sent
// here at all; only what the user should be able to read again.
class MessageLog final : public QObject
{
    Q_OBJECT
public:
    enum class Level { Notice, Warning };
    struct Entry
    {
        QDateTime time;
        QString text;
        Level level = Level::Notice;
        bool read = false;
    };

    using QObject::QObject;
    void add(const QString &text, Level level = Level::Notice, bool read = false);
    // The message added at time, read (put away) now.
    void markRead(const QDateTime &time);
    const QList<Entry> &entries() const { return m_entries; }
    int unreadCount() const;
    void markAllRead();
    void clear();

signals:
    void changed();

private:
    QList<Entry> m_entries;
};

// What a tab asks the user while it waits for an answer ("Disconnected …"
// with Reconnect and Close Tab, a clipboard offer with Save to…). Kept with
// the page, as its MessageLog; the tab strip shows it, pinned above the
// messages, with its buttons. With the strip off (inStrip() false) the page
// shows its own bar instead.
class TabPrompt final : public QObject
{
    Q_OBJECT
public:
    struct Action
    {
        QString label;
        QString tip;
        std::function<void()> run;
    };

    // page's prompt, made on first use.
    static TabPrompt *of(QWidget *page);
    // Whether prompts show in the tab strip; changed() of hub() follows.
    static bool inStrip();
    static void setInStrip(bool inStrip);
    // Tells about every prompt's change, and the strip's coming and going.
    static TabPrompt *hub();

    // dismiss: run by the ✕ (none: no ✕). A choice (with actions) has no ✕:
    // it stays until answered, so its options are not lost. owner: who may
    // clear it (a tab's own state, the default, or another part such as the
    // offer to save a password, which the tab's connecting must not clear).
    void set(const QString &text, bool warning, const QList<Action> &actions,
             const std::function<void()> &dismiss = {}, const QString &owner = {});
    // Clears the prompt if owner set it.
    void clear(const QString &owner = {});
    bool isActive() const { return !m_text.isEmpty(); }
    QString text() const { return m_text; }
    bool isWarning() const { return m_warning; }
    const QList<Action> &actions() const { return m_actions; }
    bool canDismiss() const { return m_dismiss && m_actions.isEmpty(); }
    void run(int action);
    void dismiss();

signals:
    void changed();

private:
    using QObject::QObject;
    QString m_text;
    bool m_warning = false;
    QList<Action> m_actions;
    std::function<void()> m_dismiss;
    QString m_owner;
};

// A status-bar button of a fixed width, so nothing beside it moves when its
// text changes: a quiet text when idle, the accent colour (as the tab
// activity dot) when there is something to see. Drawn here, not by the
// style, so every look shows the same thing; longer texts are shortened,
// the whole text in the tooltip.
class StatusIndicator final : public QAbstractButton
{
    Q_OBJECT
public:
    // widestText: the longest text it will show, which sets its width.
    StatusIndicator(const QIcon &icon, const QString &widestText, QWidget *parent = nullptr);
    void setState(const QString &text, bool active);
    QString stateText() const { return m_text; }
    QSize sizeHint() const override;
    // A height of its row's (0: its own).
    void setRowHeight(int height);
    // Shown on hover while the whole text fits (else the text is).
    void setTip(const QString &tip);

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void updateSize();
    QString m_text;
    QString m_widestText;
    bool m_active = false;
    bool m_hover = false;
    int m_rowHeight = 0;
    QString m_tip;
};

// The messages indicator and the list it opens.
class MessagesIndicator final : public QObject
{
    Q_OBJECT
public:
    MessagesIndicator(MessageLog *log, QWidget *parent);
    StatusIndicator *button() const { return m_button; }

private:
    void update();
    void showList();
    MessageLog *m_log;
    StatusIndicator *m_button;
};

// The first row of the side panels (a search box, the Files title, the
// session list's header) and the tab strip beside them share one height and
// start the same distance below their tab bars, so they read as one line.
constexpr int PanelRowTop = 4;
int panelRowHeight(const QWidget *context);

// Whether the Messages and Transfers lists let the window show through
// (Preferences → Appearance → Looks); solid by default.
bool seeThroughLists();
void setSeeThroughLists(bool seeThrough);

// A list of messages above (or below) anchor, the newest first: new ones in
// the text colour, read ones grey; Clear all calls clearAll.
void showMessageList(QWidget *anchor, const QString &title, const QList<MessageLog::Entry> &entries,
                     const std::function<void()> &clearAll, bool below);

// The band between the tab bar and the tab (or below the tab): the tab's newest message (with
// the pane it is about, in a split tab), ✕ to put it away, a click for the
// tab's earlier messages. Always the same height, empty when there is none.
class TabMessageStrip final : public QWidget
{
    Q_OBJECT
public:
    explicit TabMessageStrip(QWidget *parent = nullptr);
    void showEntry(const MessageLog::Entry &entry, const QString &pane);
    void clear();
    // The tab's newest running transfer on the right, "+N" for the others;
    // ended: its state ("done", "failed") instead of its progress.
    void showTransfer(const QString &name, bool upload, int percent, int more, const QString &cancelHint,
                      const QString &ended = {});
    void clearTransfer();
    // A tab's prompt, pinned in place of the message: its text, its buttons
    // (labels and tooltips) and, with closable, the ✕. more: other panes'
    // prompts, as "+N".
    void showPrompt(const QString &text, bool warning, const QString &pane,
                    const QList<QPair<QString, QString>> &buttons, bool closable, int more);
    void clearPrompt();
    // Without the Messages part at the right (a pane's bar): its parts end at
    // the edge, and a click on the message opens nothing.
    void setCountShown(bool shown);
    // The tab's messages not yet read, on the Messages part at the right;
    // below 0: no tab, so no Messages part either (the strip stays, empty).
    void setMessageCount(int unread);
    // A pane that can be dragged to other places: a handle (dots) at the
    // left end, in a slot of its own, and the open hand over the empty parts.
    void setMovable(bool movable);
    // Dragged only by the handle (not the whole bar); a click on the handle
    // asks for the Move menu (handleClicked).
    void setMoveStyle(bool handleOnly, bool handleMenu);
    // The transfer part keeps its place between dividers even when idle
    // (nothing shifts when a transfer starts), or shows only while transferring.
    void setFixedParts(bool fixed);

signals:
    void dismissRequested();
    void historyRequested();
    void transfersRequested();
    void cancelTransferRequested();
    void promptActionRequested(int action);
    void promptDismissRequested();
    // Pressed and moved: the pane is being dragged (see PaneDrag).
    void dragRequested();
    // The handle was clicked (setMoveStyle handleMenu): global is where the
    // menu goes.
    void handleClicked(const QPoint &global);

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QRect closeRect() const;
    void placeButtons();
    // Left of the ✕ (or of the buttons): where the text ends.
    int textRight() const;
    bool showsClose() const { return !m_entry.text.isEmpty() && (!m_prompt || m_promptClosable); }
    QRect transferRect() const;
    QRect transferCancelRect() const;
    QRect handleRect() const;
    bool transferPart() const { return m_transfer.shown || m_fixedParts; }
    // Where a press drags the pane (the handle, or any part that is not clicked).
    bool drags(const QPoint &at) const;
    void setPointer(const QPoint &at);
    // The part under at that does something when clicked (no hand there).
    bool clickable(const QPoint &at) const;
    MessageLog::Entry m_entry;
    struct
    {
        QString name;
        bool upload = true;
        int percent = 0;
        int more = 0;
        QString cancelHint;
        bool shown = false;
        QString ended;
    } m_transfer;
    bool m_hoverCancel = false;
    QString m_messageTip;
    QString m_pane;
    bool m_hoverClose = false;
    bool m_prompt = false;
    bool m_promptClosable = false;
    int m_promptMore = 0;
    QList<QPushButton *> m_buttons;
    StatusIndicator *m_messages = nullptr;
    bool m_countShown = true;
    bool m_movable = false;
    bool m_hoverHandle = false;
    bool m_fixedParts = false;
    bool m_handleOnly = false;
    bool m_handleMenu = false;
    bool m_pressedOnHandle = false;
    std::optional<QPoint> m_pressedAt; // to drag the pane from
    // Where the right-hand parts (✕, transfer) end: before the Messages part.
    int partsRight() const;
};

// The list of transfers (running first, then ended ones) above or below
// anchor, kept up to date while open: progress, speed, ✕ to cancel, and
// Clear done. include picks which owners' transfers show (null: all);
// whereOf names an owner (the tab's title, a window's).
void showTransferList(QWidget *anchor, const std::function<bool(QWidget *)> &include,
                      const std::function<QString(QWidget *)> &whereOf, bool below);

