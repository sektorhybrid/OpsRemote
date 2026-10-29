// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "tunneleditor.h"

#include "icons.h"
#include "jumphost.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

bool darkPalette(const QWidget *widget)
{
    return widget->palette().color(QPalette::Window).lightness() < 128;
}

QColor levelColor(const QWidget *widget, TunnelCard::Level level)
{
    const bool dark = darkPalette(widget);
    switch (level) {
    case TunnelCard::Level::Ok:
        return dark ? QColor(0x81, 0xc7, 0x84) : QColor(0x1e, 0x6b, 0x24);
    case TunnelCard::Level::Working:
        return dark ? QColor(0xff, 0xb7, 0x4d) : QColor(0x8a, 0x53, 0x00);
    case TunnelCard::Level::Problem:
        return dark ? QColor(0xef, 0x9a, 0x9a) : QColor(0xa0, 0x1c, 0x1c);
    case TunnelCard::Level::None:
        break;
    }
    return widget->palette().color(QPalette::PlaceholderText);
}

// Shown before the status text, so the state does not depend on colour.
QString levelMark(TunnelCard::Level level)
{
    switch (level) {
    case TunnelCard::Level::Ok:
        return QStringLiteral("✓ ");
    case TunnelCard::Level::Working:
        return QStringLiteral("… ");
    case TunnelCard::Level::Problem:
        return QStringLiteral("✕ ");
    case TunnelCard::Level::None:
        break;
    }
    return {};
}

bool listensHere(PortForward::Type type)
{
    return type != PortForward::Type::Remote && type != PortForward::Type::ReverseDynamic;
}

QString listenAddress(const PortForward &forward)
{
    return QStringLiteral("%1:%2")
        .arg(forward.allowLan ? QStringLiteral("this computer") : QStringLiteral("localhost"))
        .arg(forward.listenPort);
}

// This computer's network addresses, for "other computers use …".
QStringList ownAddresses()
{
    QStringList addresses;
    for (const auto &address : QNetworkInterface::allAddresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
            addresses << address.toString();
    }
    return addresses;
}

// A forward in the list: "Local 8080 → localhost:80".
QString forwardLabel(const PortForward &forward)
{
    const QString listens = QStringLiteral("%1 %2%3").arg(PortForward::typeName(forward.type))
                                .arg(forward.listensOnServer() ? QStringLiteral("server:") : QString())
                                .arg(forward.listenPort);
    if (forward.type == PortForward::Type::Socket)
        return QStringLiteral("%1 → %2").arg(listens, forward.targetHost);
    if (forward.hasTarget())
        return QStringLiteral("%1 → %2:%3").arg(listens, forward.targetHost).arg(forward.targetPort);
    return listens;
}

// Rows the list of forwards always shows; more scroll.
constexpr int MaximumForwardRows = 5;

QSpinBox *portBox(quint16 value, QWidget *parent)
{
    auto *box = new QSpinBox(parent);
    box->setRange(1, 65535);
    box->setValue(value > 0 ? value : 8080);
    box->setAccessibleName(QStringLiteral("Port"));
    return box;
}

}

// ---- Cards and links ----

TunnelCard::TunnelCard(const QString &icon, const QString &title, bool optional, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("tunnelCard"));
    setStyleSheet(QStringLiteral("#tunnelCard { border: 1px %1 palette(mid); border-radius: 8px; "
                                 "background: palette(base); }")
                      .arg(optional ? QStringLiteral("dashed") : QStringLiteral("solid")));
    setMinimumWidth(170);
    setMaximumWidth(280);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    auto *header = new QHBoxLayout;
    auto *iconLabel = new QLabel(this);
    iconLabel->setPixmap(themedIcon(icon, QStyle::SP_ComputerIcon).pixmap(18, 18));
    auto *titleLabel = new QLabel(title, this);
    QFont bold = titleLabel->font();
    bold.setBold(true);
    titleLabel->setFont(bold);
    header->addWidget(iconLabel);
    header->addWidget(titleLabel, 1);
    layout->addLayout(header);
    m_body = new QVBoxLayout;
    m_body->setSpacing(6);
    layout->addLayout(m_body);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setVisible(false);
    layout->addWidget(m_status);
}

void TunnelCard::setStatus(Level level, const QString &text)
{
    m_status->setVisible(!text.isEmpty());
    m_status->setText(levelMark(level) + text);
    QPalette palette = m_status->palette();
    palette.setColor(QPalette::WindowText, levelColor(this, level));
    m_status->setPalette(palette);
}

TunnelLink::TunnelLink(const QString &above, const QString &below, bool encrypted, bool leftward,
                       QWidget *parent)
    : QWidget(parent), m_above(above), m_below(below), m_encrypted(encrypted), m_leftward(leftward)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAccessibleName(QStringLiteral("%1 %2").arg(above, below));
}

void TunnelLink::setLevel(TunnelCard::Level level)
{
    m_level = level;
    update();
}

QSize TunnelLink::minimumSizeHint() const
{
    const QFontMetrics metrics(font());
    return {qMax(60, qMax(metrics.horizontalAdvance(m_above), metrics.horizontalAdvance(m_below)) / 2),
            metrics.height() * 3};
}

void TunnelLink::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int middle = height() / 2;
    const QColor color = m_level == TunnelCard::Level::None ? palette().color(QPalette::Mid)
                                                            : levelColor(this, m_level);
    QPen pen(color, m_encrypted ? 4 : 3);
    // A problem is dotted, plain TCP dashed: told apart without colour.
    if (m_level == TunnelCard::Level::Problem)
        pen.setStyle(Qt::DotLine);
    else if (!m_encrypted)
        pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    const int start = 4;
    const int end = width() - 4;
    painter.drawLine(start, middle, end, middle);
    // The arrow points the way connections are opened.
    QPainterPath arrow;
    const int tip = m_leftward ? start : end;
    const int back = m_leftward ? tip + 10 : tip - 10;
    arrow.moveTo(tip, middle);
    arrow.lineTo(back, middle - 6);
    arrow.lineTo(back, middle + 6);
    arrow.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(arrow);
    painter.setPen(palette().color(QPalette::PlaceholderText));
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    painter.setFont(small);
    const QRect top(0, 0, width(), middle - 6);
    const QRect bottom(0, middle + 6, width(), height() - middle - 6);
    painter.drawText(top, Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, m_above);
    painter.drawText(bottom, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, m_below);
}

// ---- The editor ----

TunnelEditor::TunnelEditor(TunnelsWindow::SessionChoices sessions, NewEntry::SessionMaker makeSession,
                           TunnelsWindow::ServerDescriber describer, QWidget *parent)
    : QWidget(parent), m_sessions(std::move(sessions)), m_makeSession(std::move(makeSession)),
      m_describer(std::move(describer))
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    auto *top = new QHBoxLayout;
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(QStringLiteral("Name, e.g. Grafana at work"));
    m_name->setAccessibleName(QStringLiteral("Tunnel name"));
    m_name->setMaximumWidth(260);
    top->addWidget(m_name);
    // The kind of the forward shown, as a row of choices.
    auto *kinds = new QButtonGroup(this);
    const QList<QPair<PortForward::Type, QString>> kindTips{
        {PortForward::Type::Local, QStringLiteral("A port on this computer reaches a host behind the server (ssh -L).")},
        {PortForward::Type::Remote, QStringLiteral("A port on the server reaches a host here (ssh -R).")},
        {PortForward::Type::Dynamic, QStringLiteral("A SOCKS proxy here; connections leave from the server (ssh -D).")},
        {PortForward::Type::ReverseDynamic, QStringLiteral("A SOCKS proxy on the server; connections leave from this computer (ssh -R port).")},
        {PortForward::Type::Socket, QStringLiteral("A port here reaches a Unix socket on the server, e.g. Docker's (ssh -L port:/path).")},
        {PortForward::Type::HttpProxy, QStringLiteral("An HTTP proxy here for programs without SOCKS; connections leave from the server.")}};
    auto *kindRow = new QHBoxLayout;
    kindRow->setSpacing(0);
    // The chosen kind stands out in the accent colour.
    const QString kindStyle = QStringLiteral("QToolButton { padding: 4px 10px; } "
                                             "QToolButton:checked { background: palette(highlight); "
                                             "color: palette(highlighted-text); border-radius: 4px; }");
    for (const auto &[type, tip] : kindTips) {
        auto *button = new QToolButton(this);
        button->setText(PortForward::typeName(type));
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setAutoRaise(false);
        button->setStyleSheet(kindStyle);
        button->setProperty("kind", int(type));
        kinds->addButton(button);
        kindRow->addWidget(button);
        m_kinds << button;
        connect(button, &QToolButton::clicked, this, [this, type] {
            if (m_tunnel.forwards.isEmpty() || forward().type == type)
                return;
            forward().type = type;
            if (forward().type == PortForward::Type::Socket && !forward().targetHost.startsWith(QLatin1Char('/')))
                forward().targetHost = QStringLiteral("/var/run/docker.sock");
            if (forward().hasTarget() && forward().type != PortForward::Type::Socket
                && (forward().targetHost.isEmpty() || forward().targetHost.startsWith(QLatin1Char('/'))))
                forward().targetHost = QStringLiteral("localhost");
            if (forward().hasTarget() && forward().type != PortForward::Type::Socket && forward().targetPort == 0)
                forward().targetPort = 80;
            changed(true);
        });
    }
    top->addLayout(kindRow);
    top->addStretch();
    m_autoStart = new QCheckBox(QStringLiteral("Start with OpsRemote"), this);
    m_reconnect = new QCheckBox(QStringLiteral("Reconnect if it drops"), this);
    m_startStop = new QPushButton(this);
    auto *remove = new QToolButton(this);
    remove->setIcon(themedIcon(QStringLiteral("edit-delete"), QStyle::SP_TrashIcon));
    remove->setToolTip(QStringLiteral("Delete this tunnel"));
    remove->setAccessibleName(QStringLiteral("Delete tunnel"));
    top->addWidget(m_autoStart);
    top->addWidget(m_reconnect);
    top->addWidget(m_startStop);
    top->addWidget(remove);
    layout->addLayout(top);

    // The forwards of this tunnel, all in view; the diagram shows the
    // selected one.
    auto *middle = new QHBoxLayout;
    auto *forwardColumn = new QVBoxLayout;
    auto *forwardsTitle = new QLabel(QStringLiteral("Forwards"), this);
    forwardsTitle->setForegroundRole(QPalette::PlaceholderText);
    m_forwardList = new QListWidget(this);
    m_forwardList->setAccessibleName(QStringLiteral("Forwards"));
    m_forwardList->setToolTip(QStringLiteral("The forwards over this SSH connection; the diagram shows the selected one"));
    m_forwardList->setFixedWidth(fontMetrics().horizontalAdvance(QLatin1Char('x')) * 34);
    // Five rows high (see rebuild()).
    m_forwardList->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_forwardList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_forwardList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_forwardList->setTextElideMode(Qt::ElideMiddle);
    m_forwardList->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto *addForwardButton = new QToolButton(this);
    addForwardButton->setText(QStringLiteral("+ Add"));
    addForwardButton->setToolTip(QStringLiteral("Another forward over the same SSH connection"));
    m_removeForward = new QToolButton(this);
    m_removeForward->setText(QStringLiteral("Remove"));
    m_removeForward->setToolTip(QStringLiteral("Remove the selected forward (Delete)"));
    auto *removeAction = new QAction(QStringLiteral("Remove"), m_forwardList);
    removeAction->setShortcut(QKeySequence::Delete);
    removeAction->setShortcutContext(Qt::WidgetShortcut);
    m_forwardList->addAction(removeAction);
    auto *forwardButtons = new QHBoxLayout;
    forwardButtons->addWidget(addForwardButton);
    forwardButtons->addWidget(m_removeForward);
    forwardButtons->addStretch();
    forwardColumn->addWidget(forwardsTitle);
    forwardColumn->addWidget(m_forwardList, 1);
    forwardColumn->addLayout(forwardButtons);
    forwardColumn->addStretch();
    middle->addLayout(forwardColumn);

    m_diagram = new QWidget(this);
    m_diagramLayout = new QHBoxLayout(m_diagram);
    m_diagramLayout->setContentsMargins(0, 4, 0, 4);
    m_diagramLayout->setSpacing(0);
    middle->addWidget(m_diagram, 1);
    layout->addLayout(middle);

    auto *summaryBox = new QFrame(this);
    summaryBox->setObjectName(QStringLiteral("tunnelSummary"));
    summaryBox->setStyleSheet(QStringLiteral("#tunnelSummary { border: 1px solid palette(midlight); "
                                             "border-radius: 8px; background: palette(alternate-base); }"));
    auto *summaryLayout = new QVBoxLayout(summaryBox);
    m_summary = new QLabel(summaryBox);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_share = new QLabel(summaryBox);
    m_share->setWordWrap(true);
    m_share->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_share->setForegroundRole(QPalette::PlaceholderText);
    auto *commandRow = new QHBoxLayout;
    m_command = new QLineEdit(summaryBox);
    m_command->setReadOnly(true);
    m_command->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_command->setAccessibleName(QStringLiteral("Equivalent ssh command"));
    auto *copy = new QPushButton(QStringLiteral("Copy Command"), summaryBox);
    m_open = new QPushButton(summaryBox);
    auto *checks = new QPushButton(QStringLiteral("Run Checks"), summaryBox);
    checks->setToolTip(QStringLiteral("Is the port here free? Does the server answer? Does it reach "
                                      "the destination?"));
    auto *moreButton = new QToolButton(summaryBox);
    moreButton->setText(QStringLiteral("More Options"));
    moreButton->setCheckable(true);
    commandRow->addWidget(m_command, 1);
    commandRow->addWidget(copy);
    commandRow->addWidget(m_open);
    commandRow->addWidget(checks);
    commandRow->addWidget(moreButton);
    summaryLayout->addWidget(m_summary);
    summaryLayout->addWidget(m_share);
    summaryLayout->addLayout(commandRow);
    layout->addWidget(summaryBox);

    m_restartNote = new QLabel(QStringLiteral("Changes apply when the tunnel starts again (Stop, then "
                                              "Start)."), this);
    m_restartNote->setForegroundRole(QPalette::PlaceholderText);
    m_restartNote->setVisible(false);
    layout->addWidget(m_restartNote);

    m_more = new QWidget(this);
    auto *moreLayout = new QHBoxLayout(m_more);
    moreLayout->setContentsMargins(0, 0, 0, 0);
    m_reuseTab = new QCheckBox(QStringLiteral("Use an open tab's connection to the server"), m_more);
    m_reuseTab->setToolTip(QStringLiteral("When a tab of this SSH session is open, the tunnel rides "
                                          "on its connection: no second log-on."));
    m_wakeMac = new QLineEdit(m_more);
    m_wakeMac->setPlaceholderText(QStringLiteral("none"));
    m_wakeMac->setToolTip(QStringLiteral("Wake the server with Wake-on-LAN before connecting "
                                         "(its MAC address, aa:bb:cc:dd:ee:ff)."));
    m_wakeMac->setMaximumWidth(170);
    m_wait = new QSpinBox(m_more);
    m_wait->setRange(0, 600);
    m_wait->setSuffix(QStringLiteral(" s"));
    m_wait->setSpecialValueText(QStringLiteral("don't wait"));
    m_wait->setToolTip(QStringLiteral("Before connecting, wait until the server answers on its SSH "
                                      "port, up to this long."));
    moreLayout->addWidget(m_reuseTab);
    moreLayout->addSpacing(16);
    moreLayout->addWidget(new QLabel(QStringLiteral("Wake-on-LAN MAC:"), m_more));
    moreLayout->addWidget(m_wakeMac);
    moreLayout->addSpacing(16);
    moreLayout->addWidget(new QLabel(QStringLiteral("Wait for the server:"), m_more));
    moreLayout->addWidget(m_wait);
    moreLayout->addStretch();
    m_more->setVisible(false);
    layout->addWidget(m_more);
    layout->addStretch();

    connect(moreButton, &QToolButton::toggled, m_more, &QWidget::setVisible);
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &text) {
        m_tunnel.name = text.trimmed();
        changed(false);
    });
    connect(m_autoStart, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating) {
            m_tunnel.autoStart = on;
            changed(false);
        }
    });
    connect(m_reconnect, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating) {
            m_tunnel.reconnect = on;
            changed(false);
        }
    });
    connect(m_reuseTab, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_updating) {
            m_tunnel.reuseTab = on;
            changed(false);
        }
    });
    connect(m_wakeMac, &QLineEdit::textEdited, this, [this](const QString &text) {
        m_tunnel.wake.mac = text.trimmed();
        changed(false);
    });
    connect(m_wait, &QSpinBox::valueChanged, this, [this](int value) {
        if (!m_updating) {
            m_tunnel.waitSeconds = value;
            changed(false);
        }
    });
    connect(m_startStop, &QPushButton::clicked, this, &TunnelEditor::startStopRequested);
    connect(remove, &QToolButton::clicked, this, &TunnelEditor::deleteRequested);
    connect(checks, &QPushButton::clicked, this, [this] { emit checksRequested(m_forward); });
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_command->text());
    });
    connect(m_open, &QPushButton::clicked, this, [this] {
        if (!m_tunnel.forwards.isEmpty())
            QDesktopServices::openUrl(QUrl(m_open->property("url").toString()));
    });
    connect(m_forwardList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_updating || row < 0 || row == m_forward)
            return;
        m_forward = row;
        rebuild();
    });
    connect(addForwardButton, &QToolButton::clicked, this, &TunnelEditor::addForward);
    connect(m_removeForward, &QToolButton::clicked, this, [this] { removeForward(m_forward); });
    connect(removeAction, &QAction::triggered, this, [this] { removeForward(m_forward); });
}

void TunnelEditor::addForward()
{
    PortForward next;
    next.type = PortForward::Type::Local;
    next.listenPort = 8080;
    for (const auto &existing : std::as_const(m_tunnel.forwards))
        next.listenPort = qMax<quint16>(next.listenPort, quint16(existing.listenPort + 1));
    next.targetHost = QStringLiteral("localhost");
    next.targetPort = 80;
    m_tunnel.forwards.append(next);
    m_forward = int(m_tunnel.forwards.size()) - 1;
    changed(true);
}

void TunnelEditor::removeForward(int index)
{
    // A tunnel keeps at least one forward.
    if (m_tunnel.forwards.size() <= 1 || index < 0 || index >= m_tunnel.forwards.size())
        return;
    m_tunnel.forwards.removeAt(index);
    m_forward = qMin(m_forward, int(m_tunnel.forwards.size()) - 1);
    changed(true);
}

void TunnelEditor::setTunnel(const SavedTunnel &tunnel)
{
    const bool same = tunnel.id == m_tunnel.id;
    m_tunnel = tunnel;
    if (m_tunnel.forwards.isEmpty())
        m_tunnel.forwards.append(PortForward());
    if (!same) {
        m_forward = 0;
        for (auto &text : m_checked)
            text.clear();
    }
    m_forward = qBound(0, m_forward, int(m_tunnel.forwards.size()) - 1);
    rebuild();
}

void TunnelEditor::changed(bool structure)
{
    if (m_updating)
        return;
    if (m_runner && m_runner->state() != TunnelRunner::State::Stopped
        && m_runner->state() != TunnelRunner::State::Failed)
        m_restartNote->setVisible(true);
    emit edited(m_tunnel);
    if (structure)
        rebuild();
    else
        updateTexts();
}

void TunnelEditor::rebuild()
{
    m_updating = true;
    m_name->setText(m_tunnel.name);
    m_autoStart->setChecked(m_tunnel.autoStart);
    m_reconnect->setChecked(m_tunnel.reconnect);
    m_reuseTab->setChecked(m_tunnel.reuseTab);
    m_reuseTab->setEnabled(m_tunnel.server.startsWith(JumpHost::SessionPrefix));
    m_wakeMac->setText(m_tunnel.wake.mac);
    m_wait->setValue(m_tunnel.waitSeconds);
    // Rows are updated in place: this may run while the list is still
    // handling a click on one of them.
    while (m_forwardList->count() > m_tunnel.forwards.size())
        delete m_forwardList->takeItem(m_forwardList->count() - 1);
    while (m_forwardList->count() < m_tunnel.forwards.size())
        m_forwardList->addItem(QString());
    for (int i = 0; i < m_tunnel.forwards.size(); ++i) {
        auto *item = m_forwardList->item(i);
        item->setText(forwardLabel(m_tunnel.forwards[i]));
        item->setToolTip(item->text());
    }
    m_forwardList->setCurrentRow(m_forward);
    m_removeForward->setEnabled(m_tunnel.forwards.size() > 1);
    const int row = m_forwardList->sizeHintForRow(0) > 0 ? m_forwardList->sizeHintForRow(0)
                                                         : fontMetrics().height() + 4;
    // Always five rows, empty ones too (a steady layout); more scroll.
    m_forwardList->setFixedHeight(MaximumForwardRows * row
                                  + 2 * m_forwardList->frameWidth());
    for (auto *button : std::as_const(m_kinds))
        button->setChecked(button->property("kind").toInt() == int(forward().type));
    buildDiagram();
    m_updating = false;
    updateTexts();
    setRunner(m_runner);
}

QWidget *TunnelEditor::listenFields(bool onServer)
{
    auto *row = new QWidget(this);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *where = new QComboBox(row);
    where->addItem(onServer ? QStringLiteral("the server only") : QStringLiteral("this computer only"), false);
    where->addItem(onServer ? QStringLiteral("its whole network") : QStringLiteral("all interfaces (LAN)"), true);
    where->setCurrentIndex(forward().allowLan ? 1 : 0);
    where->setToolTip(onServer ? QStringLiteral("Its whole network needs GatewayPorts on the server.")
                               : QStringLiteral("All interfaces lets other computers on your network use it."));
    where->setAccessibleName(QStringLiteral("Who may connect"));
    auto *port = portBox(forward().listenPort, row);
    layout->addWidget(where, 1);
    layout->addWidget(new QLabel(QStringLiteral(":"), row));
    layout->addWidget(port);
    connect(where, &QComboBox::activated, this, [this, where] {
        forward().allowLan = where->currentData().toBool();
        changed(false);
    });
    connect(port, &QSpinBox::valueChanged, this, [this](int value) {
        if (m_updating)
            return;
        forward().listenPort = quint16(value);
        changed(false);
    });
    return row;
}

QWidget *TunnelEditor::serverFields()
{
    auto *box = new QWidget(this);
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *choice = new QComboBox(box);
    choice->setAccessibleName(QStringLiteral("SSH server"));
    for (const auto &[id, label] : m_sessions())
        choice->addItem(QStringLiteral("Session: %1").arg(label), JumpHost::SessionPrefix + id);
    if (m_makeSession)
        NewEntry::add(
            choice, m_makeSession, m_sessions,
            [](const QString &label) { return QStringLiteral("Session: %1").arg(label); },
            [](const QString &id) { return QVariant(JumpHost::SessionPrefix + id); });
    choice->addItem(QStringLiteral("Other server…"), QString());
    const int known = m_tunnel.server.startsWith(JumpHost::SessionPrefix) ? choice->findData(m_tunnel.server) : -1;
    choice->setCurrentIndex(known >= 0 ? known : choice->count() - 1);
    auto *other = new QLineEdit(m_tunnel.server.startsWith(JumpHost::SessionPrefix) ? QString() : m_tunnel.server, box);
    other->setPlaceholderText(QStringLiteral("user@host[:port]"));
    other->setAccessibleName(QStringLiteral("Other SSH server"));
    other->setVisible(known < 0);
    layout->addWidget(choice);
    layout->addWidget(other);
    if (known < 0) {
        auto *addHop = new QPushButton(QStringLiteral("+ Jump Host"), box);
        addHop->setToolTip(QStringLiteral("Reach this server through another SSH server first"));
        layout->addWidget(addHop);
        connect(addHop, &QPushButton::clicked, this, [this] {
            m_tunnel.via.append(QString());
            changed(true);
        });
    }
    connect(choice, &QComboBox::activated, this, [this, choice, other](int index) {
        if (NewEntry::isNew(choice, index))
            return;
        const QString value = choice->currentData().toString();
        m_tunnel.server = value.isEmpty() ? other->text().trimmed() : value;
        if (!value.isEmpty())
            m_tunnel.via.clear();
        changed(true);
    });
    connect(other, &QLineEdit::textEdited, this, [this](const QString &text) {
        m_tunnel.server = text.trimmed();
        changed(false);
    });
    return box;
}

void TunnelEditor::buildDiagram()
{
    // The old cards go later: the change may come from a widget on one of
    // them (the server choice, + Jump Host), still inside its own signal.
    while (auto *item = m_diagramLayout->takeAt(0)) {
        if (auto *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_cards.clear();
    m_links.clear();
    m_hopCards.clear();
    const auto type = forward().type;
    const bool here = listensHere(type);
    const TunnelServerInfo server = m_describer(m_tunnel.server, m_tunnel.via);
    const auto addLink = [this](const QString &above, const QString &below, bool encrypted, bool leftward) {
        auto *link = new TunnelLink(above, below, encrypted, leftward, m_diagram);
        m_diagramLayout->addWidget(link, 1);
        m_links << link;
        return link;
    };

    // This computer's end.
    TunnelCard *first = nullptr;
    if (here) {
        first = new TunnelCard(QStringLiteral("computer"), QStringLiteral("This computer"), false, m_diagram);
        auto *what = new QLabel(type == PortForward::Type::Dynamic   ? QStringLiteral("SOCKS proxy for your programs on")
                                : type == PortForward::Type::HttpProxy ? QStringLiteral("HTTP proxy for your programs on")
                                                                       : QStringLiteral("Your programs connect to"),
                                first);
        what->setForegroundRole(QPalette::PlaceholderText);
        first->body()->addWidget(what);
        first->body()->addWidget(listenFields(false));
    } else if (type == PortForward::Type::Remote) {
        first = new TunnelCard(QStringLiteral("computer"), QStringLiteral("Service here"), false, m_diagram);
        auto *what = new QLabel(QStringLiteral("Connections are handed to"), first);
        what->setForegroundRole(QPalette::PlaceholderText);
        first->body()->addWidget(what);
        auto *row = new QHBoxLayout;
        auto *host = new QLineEdit(forward().targetHost, first);
        host->setPlaceholderText(QStringLiteral("localhost"));
        host->setAccessibleName(QStringLiteral("Host here"));
        auto *port = portBox(forward().targetPort, first);
        row->addWidget(host, 1);
        row->addWidget(new QLabel(QStringLiteral(":"), first));
        row->addWidget(port);
        first->body()->addLayout(row);
        connect(host, &QLineEdit::textEdited, this, [this](const QString &text) {
            forward().targetHost = text.trimmed();
            changed(false);
        });
        connect(port, &QSpinBox::valueChanged, this, [this](int value) {
            if (!m_updating) {
                forward().targetPort = quint16(value);
                changed(false);
            }
        });
    } else {
        first = new TunnelCard(QStringLiteral("network-workgroup"), QStringLiteral("This computer's networks"),
                               true, m_diagram);
        auto *what = new QLabel(QStringLiteral("Any address the programs ask for, as this computer sees it"), first);
        what->setWordWrap(true);
        first->body()->addWidget(what);
    }
    m_diagramLayout->addWidget(first);
    m_cards << first;

    // The jump hosts, then the server.
    const QStringList hops = server.fromSession ? server.jumps : m_tunnel.via;
    for (int index = 0; index < hops.size(); ++index) {
        addLink(QStringLiteral("SSH, encrypted"), QString(), true, !here);
        auto *hop = new TunnelCard(QStringLiteral("security-high"),
                                   server.fromSession ? QStringLiteral("Jump host (the session's)")
                                                      : QStringLiteral("Jump host"),
                                   true, m_diagram);
        if (server.fromSession) {
            hop->body()->addWidget(new QLabel(hops[index], hop));
        } else {
            auto *row = new QHBoxLayout;
            auto *edit = new QLineEdit(hops[index], hop);
            edit->setPlaceholderText(QStringLiteral("user@host[:port]"));
            edit->setAccessibleName(QStringLiteral("Jump host"));
            auto *remove = new QToolButton(hop);
            remove->setText(QStringLiteral("×"));
            remove->setToolTip(QStringLiteral("Remove this jump host"));
            remove->setAccessibleName(QStringLiteral("Remove jump host"));
            row->addWidget(edit, 1);
            row->addWidget(remove);
            hop->body()->addLayout(row);
            connect(edit, &QLineEdit::textEdited, this, [this, index](const QString &text) {
                if (index < m_tunnel.via.size()) {
                    m_tunnel.via[index] = text.trimmed();
                    changed(false);
                }
            });
            connect(remove, &QToolButton::clicked, this, [this, index] {
                if (index < m_tunnel.via.size()) {
                    m_tunnel.via.removeAt(index);
                    changed(true);
                }
            });
        }
        m_diagramLayout->addWidget(hop);
        m_hopCards << hop;
    }
    addLink(QStringLiteral("SSH, encrypted"), QString(), true, !here);
    auto *serverCard = new TunnelCard(QStringLiteral("network-server"), QStringLiteral("SSH server"), false, m_diagram);
    serverCard->body()->addWidget(serverFields());
    if (!here) {
        auto *what = new QLabel(type == PortForward::Type::ReverseDynamic ? QStringLiteral("SOCKS proxy there on")
                                                                          : QStringLiteral("Opens a port there"),
                                serverCard);
        what->setForegroundRole(QPalette::PlaceholderText);
        serverCard->body()->addWidget(what);
        serverCard->body()->addWidget(listenFields(true));
    }
    m_scan = new QPushButton(QStringLiteral("Scan Its Ports…"), serverCard);
    m_scan->setVisible(false);
    serverCard->body()->addWidget(m_scan);
    connect(m_scan, &QPushButton::clicked, this, [this] {
        const auto info = m_describer(m_tunnel.server, m_tunnel.via);
        if (info.valid)
            emit scanRequested(info.host);
    });
    m_diagramLayout->addWidget(serverCard);
    m_cards << serverCard;

    // The far end.
    TunnelCard *last = nullptr;
    switch (type) {
    case PortForward::Type::Local: {
        addLink(QStringLiteral("plain TCP"), QStringLiteral("inside its network"), false, false);
        last = new TunnelCard(QStringLiteral("network-server"), QStringLiteral("Destination"), false, m_diagram);
        auto *row = new QHBoxLayout;
        auto *host = new QLineEdit(forward().targetHost, last);
        host->setPlaceholderText(QStringLiteral("as the server sees it"));
        host->setAccessibleName(QStringLiteral("Destination host"));
        auto *port = portBox(forward().targetPort, last);
        row->addWidget(host, 1);
        row->addWidget(new QLabel(QStringLiteral(":"), last));
        row->addWidget(port);
        last->body()->addLayout(row);
        connect(host, &QLineEdit::textEdited, this, [this](const QString &text) {
            forward().targetHost = text.trimmed();
            changed(false);
        });
        connect(port, &QSpinBox::valueChanged, this, [this](int value) {
            if (!m_updating) {
                forward().targetPort = quint16(value);
                changed(false);
            }
        });
        break;
    }
    case PortForward::Type::Socket: {
        addLink(QStringLiteral("Unix socket"), QStringLiteral("on the server"), false, false);
        last = new TunnelCard(QStringLiteral("network-server"), QStringLiteral("Socket on the server"), false, m_diagram);
        auto *path = new QLineEdit(forward().targetHost, last);
        path->setPlaceholderText(QStringLiteral("/var/run/docker.sock"));
        path->setAccessibleName(QStringLiteral("Socket path"));
        last->body()->addWidget(path);
        connect(path, &QLineEdit::textEdited, this, [this](const QString &text) {
            forward().targetHost = text.trimmed();
            changed(false);
        });
        break;
    }
    case PortForward::Type::Dynamic:
    case PortForward::Type::HttpProxy:
        addLink(QStringLiteral("to any address"), QStringLiteral("the program asks for"), false, false);
        last = new TunnelCard(QStringLiteral("network-workgroup"), QStringLiteral("The server's networks"), true, m_diagram);
        last->body()->addWidget(new QLabel(QStringLiteral("Any host and port, as the server sees it"), last));
        break;
    case PortForward::Type::Remote:
    case PortForward::Type::ReverseDynamic:
        addLink(QStringLiteral("programs there"), QStringLiteral("connect"), false, true);
        last = new TunnelCard(QStringLiteral("system-users"), QStringLiteral("Programs on the server"), true, m_diagram);
        last->body()->addWidget(new QLabel(type == PortForward::Type::ReverseDynamic
                                               ? QStringLiteral("Use the SOCKS proxy there")
                                               : QStringLiteral("Open the port there"),
                                           last));
        break;
    }
    m_diagramLayout->addWidget(last);
    m_cards << last;
    // Results of an earlier "Run checks" stay until the next.
    for (int card = 0; card < 3; ++card) {
        if (!m_checked[card].isEmpty())
            m_cards[card]->setStatus(m_checkLevel[card], m_checked[card]);
    }
}

void TunnelEditor::updateTexts()
{
    if (m_tunnel.forwards.isEmpty())
        return;
    const auto &shown = m_tunnel.forwards[m_forward];
    if (auto *item = m_forwardList->item(m_forward)) {
        item->setText(forwardLabel(shown));
        item->setToolTip(item->text());
    }
    const TunnelServerInfo server = m_describer(m_tunnel.server, m_tunnel.via);
    const QString serverName = server.valid ? server.host : QStringLiteral("the SSH server");
    const QStringList hops = server.fromSession ? server.jumps : m_tunnel.via;
    QString through;
    if (!hops.isEmpty()) {
        QStringList names;
        for (const auto &hop : hops)
            names << hop.section(QLatin1Char('@'), -1).section(QLatin1Char(':'), 0, 0);
        through = QStringLiteral(", through %1").arg(names.join(QStringLiteral(" and ")));
    }
    const QString mine = QStringLiteral("<b>%1</b>").arg(listenAddress(shown).toHtmlEscaped());
    QString text;
    switch (shown.type) {
    case PortForward::Type::Local:
        text = QStringLiteral("Programs on this computer open %1 and reach <b>%2:%3</b> as <b>%4</b> sees it%5, "
                              "encrypted as far as %4.")
                   .arg(mine, shown.targetHost.toHtmlEscaped()).arg(shown.targetPort)
                   .arg(serverName.toHtmlEscaped(), through.toHtmlEscaped());
        break;
    case PortForward::Type::Socket:
        text = QStringLiteral("Programs on this computer open %1 and reach the socket <b>%2</b> on <b>%3</b>%4.")
                   .arg(mine, shown.targetHost.toHtmlEscaped(), serverName.toHtmlEscaped(), through.toHtmlEscaped());
        break;
    case PortForward::Type::Dynamic:
        text = QStringLiteral("Set your programs' SOCKS5 proxy to %1: their connections leave from <b>%2</b>%3, "
                              "encrypted as far as there.")
                   .arg(mine, serverName.toHtmlEscaped(), through.toHtmlEscaped());
        break;
    case PortForward::Type::HttpProxy:
        text = QStringLiteral("Set your programs' HTTP proxy to %1 (also for HTTPS): their connections leave from "
                              "<b>%2</b>%3.")
                   .arg(mine, serverName.toHtmlEscaped(), through.toHtmlEscaped());
        break;
    case PortForward::Type::Remote:
        text = QStringLiteral("Programs on <b>%1</b> open <b>%2:%3</b> there and reach <b>%4:%5</b> on this computer%6.")
                   .arg(serverName.toHtmlEscaped(),
                        shown.allowLan ? QStringLiteral("its address") : QStringLiteral("localhost"))
                   .arg(shown.listenPort).arg(shown.targetHost.toHtmlEscaped()).arg(shown.targetPort)
                   .arg(through.toHtmlEscaped());
        break;
    case PortForward::Type::ReverseDynamic:
        text = QStringLiteral("Programs on <b>%1</b> set their SOCKS5 proxy to <b>%2:%3</b> there: their connections "
                              "leave from this computer%4. Needs OpenSSH 7.6 or later there.")
                   .arg(serverName.toHtmlEscaped(),
                        shown.allowLan ? QStringLiteral("its address") : QStringLiteral("localhost"))
                   .arg(shown.listenPort).arg(through.toHtmlEscaped());
        break;
    }
    m_summary->setText(text);
    // Sharing: the addresses other computers use.
    const QStringList addresses = ownAddresses();
    m_share->setVisible(shown.allowLan && listensHere(shown.type) && !addresses.isEmpty());
    if (m_share->isVisible()) {
        QStringList targets;
        for (const auto &address : addresses)
            targets << QStringLiteral("%1:%2").arg(address).arg(shown.listenPort);
        m_share->setText(QStringLiteral("Other computers on your network use: %1").arg(targets.join(QStringLiteral(", "))));
    }
    m_command->setText(tunnelCommand(m_tunnel, server));
    m_command->setCursorPosition(0);
    // Open: a web page through a local forward.
    const bool web = shown.type == PortForward::Type::Local;
    const bool secure = shown.targetPort == 443 || shown.targetPort == 8443;
    const QString url = QStringLiteral("%1://localhost:%2").arg(secure ? QStringLiteral("https") : QStringLiteral("http"))
                            .arg(shown.listenPort);
    m_open->setVisible(web);
    m_open->setText(QStringLiteral("Open %1").arg(url));
    m_open->setProperty("url", url);
}

void TunnelEditor::setRunner(const TunnelRunner *runner)
{
    m_runner = runner;
    const auto state = runner ? runner->state() : TunnelRunner::State::Stopped;
    const bool active = state != TunnelRunner::State::Stopped && state != TunnelRunner::State::Failed;
    m_startStop->setText(active ? QStringLiteral("■ Stop") : QStringLiteral("▶ Start"));
    m_open->setEnabled(state == TunnelRunner::State::Running);
    if (!active)
        m_restartNote->setVisible(false);
    if (m_cards.size() < 3 || m_tunnel.forwards.isEmpty())
        return;
    const auto &shown = m_tunnel.forwards[m_forward];
    const bool here = listensHere(shown.type);
    // The server's state; the hops share it.
    TunnelCard::Level serverLevel = TunnelCard::Level::None;
    QString serverText;
    switch (state) {
    case TunnelRunner::State::Stopped:
        break;
    case TunnelRunner::State::Waking:
        serverLevel = TunnelCard::Level::Working;
        serverText = runner->message();
        break;
    case TunnelRunner::State::Connecting:
        serverLevel = TunnelCard::Level::Working;
        serverText = QStringLiteral("Connecting…");
        break;
    case TunnelRunner::State::Retrying:
        serverLevel = TunnelCard::Level::Problem;
        serverText = QStringLiteral("%1 Trying again in %2 s.").arg(runner->message()).arg(runner->secondsToRetry());
        break;
    case TunnelRunner::State::Failed:
        serverLevel = TunnelCard::Level::Problem;
        serverText = runner->message();
        break;
    case TunnelRunner::State::Running:
        serverLevel = TunnelCard::Level::Ok;
        serverText = runner->usesTab() ? QStringLiteral("Connected (an open tab's connection)")
                                       : QStringLiteral("Connected");
        break;
    }
    const auto forwardStates = runner ? runner->forwards() : QList<TunnelRunner::ForwardState>();
    const auto forwardState = m_forward < forwardStates.size() ? forwardStates[m_forward] : TunnelRunner::ForwardState();
    // Stopped, a check's result stays shown.
    if (state != TunnelRunner::State::Stopped || m_checked[Server].isEmpty())
        m_cards[Server]->setStatus(serverLevel, serverText);
    for (auto *hop : std::as_const(m_hopCards))
        hop->setStatus(serverLevel == TunnelCard::Level::Ok ? TunnelCard::Level::Ok : TunnelCard::Level::None,
                       serverLevel == TunnelCard::Level::Ok ? QStringLiteral("Passed through") : QString());
    // Where it listens: here, or on the server.
    TunnelCard *listener = here ? m_cards[Here] : m_cards[Server];
    if (state == TunnelRunner::State::Running) {
        const QString detail = forwardState.listening
            ? QStringLiteral("Listening · %1 connection%2").arg(forwardState.connections)
                  .arg(forwardState.connections == 1 ? QString() : QStringLiteral("s"))
            : forwardState.message;
        if (here)
            listener->setStatus(forwardState.listening ? TunnelCard::Level::Ok : TunnelCard::Level::Problem, detail);
        else if (!forwardState.listening)
            listener->setStatus(TunnelCard::Level::Problem, forwardState.message);
    } else if (here && m_checked[Here].isEmpty()) {
        m_cards[Here]->setStatus(TunnelCard::Level::None, QString());
    }
    // A refused or unreachable server: offer to look at its ports.
    const QString message = runner ? runner->message() : QString();
    m_scan->setVisible(state == TunnelRunner::State::Failed || state == TunnelRunner::State::Retrying
                       ? message.contains(QStringLiteral("refused"), Qt::CaseInsensitive)
                             || message.contains(QStringLiteral("timed out"), Qt::CaseInsensitive)
                             || message.contains(QStringLiteral("connect"), Qt::CaseInsensitive)
                       : false);
    const TunnelCard::Level linkLevel = state == TunnelRunner::State::Running ? TunnelCard::Level::Ok
                                        : serverLevel == TunnelCard::Level::Problem ? TunnelCard::Level::Problem
                                        : serverLevel;
    for (int index = 0; index < m_links.size(); ++index) {
        // The last link (to the destination or the server's programs) is
        // known only from a check.
        if (index == m_links.size() - 1)
            m_links[index]->setLevel(m_checkLevel[Target] != TunnelCard::Level::None && !m_checked[Target].isEmpty()
                                         ? m_checkLevel[Target]
                                         : TunnelCard::Level::None);
        else
            m_links[index]->setLevel(linkLevel);
    }
}

void TunnelEditor::setCheck(int card, TunnelCard::Level level, const QString &text)
{
    if (card < 0 || card > 2)
        return;
    m_checked[card] = text;
    m_checkLevel[card] = level;
    if (card < m_cards.size())
        m_cards[card]->setStatus(level, text);
    setRunner(m_runner);
    if (card < m_cards.size() && !text.isEmpty())
        m_cards[card]->setStatus(level, text);
}
