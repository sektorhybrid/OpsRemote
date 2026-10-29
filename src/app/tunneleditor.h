// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "tunnels.h"

#include <QFrame>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

// One stop of the diagram: a card with a title, its fields, and a status
// line (✓ ok, … working, ✕ problem), shown with a shape as well as a colour.
class TunnelCard final : public QFrame
{
    Q_OBJECT

public:
    enum class Level { None, Ok, Working, Problem };

    TunnelCard(const QString &icon, const QString &title, bool optional, QWidget *parent);
    QVBoxLayout *body() const { return m_body; }
    void setStatus(Level level, const QString &text);

private:
    QVBoxLayout *m_body = nullptr;
    QLabel *m_status = nullptr;
};

// The line between two cards: solid for SSH (encrypted), dashed for plain
// TCP; arrows show which way connections are made; coloured by state.
class TunnelLink final : public QWidget
{
    Q_OBJECT

public:
    TunnelLink(const QString &above, const QString &below, bool encrypted, bool leftward,
               QWidget *parent);
    void setLevel(TunnelCard::Level level);
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_above;
    QString m_below;
    bool m_encrypted = true;
    bool m_leftward = false;
    TunnelCard::Level m_level = TunnelCard::Level::None;
};

// The selected tunnel, edited on its diagram. Every change is reported
// with edited() at once (the window saves it).
class TunnelEditor final : public QWidget
{
    Q_OBJECT

public:
    TunnelEditor(TunnelsWindow::SessionChoices sessions, NewEntry::SessionMaker makeSession,
                 TunnelsWindow::ServerDescriber describer, QWidget *parent = nullptr);

    void setTunnel(const SavedTunnel &tunnel);
    SavedTunnel tunnel() const { return m_tunnel; }
    // The running state, shown on the cards and the Start/Stop button.
    void setRunner(const TunnelRunner *runner);
    // Results of "Run checks", for the forward shown.
    void setCheck(int card, TunnelCard::Level level, const QString &text);
    int currentForward() const { return m_forward; }
    enum Card { Here = 0, Server = 1, Target = 2 };

signals:
    void edited(const SavedTunnel &tunnel);
    void startStopRequested();
    void deleteRequested();
    void checksRequested(int forward);
    void scanRequested(const QString &host);

private:
    void rebuild();
    void buildDiagram();
    void changed(bool structure);
    void updateTexts();
    void addForward();
    void removeForward(int index);
    PortForward &forward() { return m_tunnel.forwards[m_forward]; }
    QWidget *listenFields(bool onServer);
    QWidget *serverFields();

    TunnelsWindow::SessionChoices m_sessions;
    NewEntry::SessionMaker m_makeSession;
    TunnelsWindow::ServerDescriber m_describer;
    SavedTunnel m_tunnel;
    int m_forward = 0;
    QPointer<const TunnelRunner> m_runner;

    QLineEdit *m_name = nullptr;
    QListWidget *m_forwardList = nullptr;
    QToolButton *m_removeForward = nullptr;
    QList<QToolButton *> m_kinds;
    QCheckBox *m_autoStart = nullptr;
    QCheckBox *m_reconnect = nullptr;
    QPushButton *m_startStop = nullptr;
    QWidget *m_diagram = nullptr;
    QHBoxLayout *m_diagramLayout = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_share = nullptr;
    QLineEdit *m_command = nullptr;
    QPushButton *m_open = nullptr;
    QLabel *m_restartNote = nullptr;
    QWidget *m_more = nullptr;
    QCheckBox *m_reuseTab = nullptr;
    QLineEdit *m_wakeMac = nullptr;
    QSpinBox *m_wait = nullptr;
    QPushButton *m_scan = nullptr;
    QList<TunnelCard *> m_cards; // Here, Server, Target (+ hops)
    QList<TunnelLink *> m_links;
    QList<TunnelCard *> m_hopCards;
    QString m_checked[3];
    TunnelCard::Level m_checkLevel[3] = {};
    bool m_updating = false;
};
