// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "vncpage.h"
#include "jumphost.h"
#include "sessionendedpanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
// Keysyms for keys without text (X11 keysymdef.h values).
quint32 specialKeysym(int key, bool keypad)
{
    switch (key) {
    case Qt::Key_Escape: return 0xff1b;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return 0xff09;
    case Qt::Key_Backspace: return 0xff08;
    case Qt::Key_Return: return 0xff0d;
    case Qt::Key_Enter: return keypad ? 0xff8d : 0xff0d;
    case Qt::Key_Insert: return 0xff63;
    case Qt::Key_Delete: return 0xffff;
    case Qt::Key_Pause: return 0xff13;
    case Qt::Key_Print: return 0xff61;
    case Qt::Key_SysReq: return 0xff15;
    case Qt::Key_Home: return 0xff50;
    case Qt::Key_End: return 0xff57;
    case Qt::Key_Left: return 0xff51;
    case Qt::Key_Up: return 0xff52;
    case Qt::Key_Right: return 0xff53;
    case Qt::Key_Down: return 0xff54;
    case Qt::Key_PageUp: return 0xff55;
    case Qt::Key_PageDown: return 0xff56;
    case Qt::Key_Shift: return 0xffe1;
    case Qt::Key_Control: return 0xffe3;
    case Qt::Key_Meta: return 0xffeb; // Super_L
    case Qt::Key_Alt: return 0xffe9;
    case Qt::Key_AltGr: return 0xfe03; // ISO_Level3_Shift
    case Qt::Key_CapsLock: return 0xffe5;
    case Qt::Key_NumLock: return 0xff7f;
    case Qt::Key_ScrollLock: return 0xff14;
    case Qt::Key_Menu: return 0xff67;
    default:
        break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return 0xffbe + quint32(key - Qt::Key_F1);
    return 0;
}

// The keysym of a key event: what the server should see for this key.
quint32 keysymFor(const QKeyEvent *event)
{
    const bool keypad = event->modifiers() & Qt::KeypadModifier;
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    // X11 and Wayland report the keysym itself, layout and dead keys
    // included.
    if (const quint32 native = event->nativeVirtualKey(); native != 0)
        return native;
#endif
    if (const quint32 special = specialKeysym(event->key(), keypad))
        return special;
    const QString text = event->text();
    if (!text.isEmpty() && text.at(0).unicode() >= 0x20 && text.at(0).unicode() != 0x7f) {
        const char32_t code = text.toUcs4().value(0);
        // Latin-1 keysyms are the characters; others are 0x01000000 + code.
        return code < 0x100 ? quint32(code) : 0x01000000u | quint32(code);
    }
    // Ctrl+letter and the like: the key itself.
    const int key = event->key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return (event->modifiers() & Qt::ShiftModifier) ? quint32(key) : quint32(key + 0x20);
    if (key > 0 && key < 0x100)
        return quint32(key);
    return 0;
}
}

VncPage::VncPage(const VncPageSettings &settings, QWidget *parent)
    : RemoteScreenPage(parent), m_settings(settings)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled, false);

    m_overlay = new QWidget(this);
    m_overlay->setAttribute(Qt::WA_StyledBackground);
    m_overlay->setObjectName(QStringLiteral("vncOverlay"));
    m_overlay->setStyleSheet(QStringLiteral("#vncOverlay { background: rgba(0, 0, 0, 170); }"));
    auto *overlayLayout = new QVBoxLayout(m_overlay);
    overlayLayout->addStretch();
    m_overlayText = new QLabel(m_overlay);
    m_overlayText->setAlignment(Qt::AlignCenter);
    m_overlayText->setWordWrap(true);
    m_overlayText->setStyleSheet(QStringLiteral("color: white; font-size: 11pt;"));
    overlayLayout->addWidget(m_overlayText);
    overlayLayout->addStretch();
    m_endedPanel = new SessionEndedPanel(SessionEndedPanel::Style::Bar, this);
    m_endedPanel->hide();
    connect(m_endedPanel, &SessionEndedPanel::reconnectRequested, this, [this] {
        m_reconnectAttempts = 0;
        start();
    });
    connect(m_endedPanel, &SessionEndedPanel::closeRequested, this, &VncPage::requestClose);

    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(400);
    connect(m_resizeTimer, &QTimer::timeout, this, &VncPage::requestTabSize);

    m_session = new VncSession(this);
    connect(m_session, &VncSession::statusMessage, this, [this](const QString &message) {
        showOverlay(message, false);
    });
    connect(m_session, &VncSession::connected, this,
            [this](int width, int height, const QString &name) {
                m_connected = true;
                m_wasConnected = true;
                m_reconnectAttempts = 0;
                m_overlay->hide();
                m_endedPanel->hide();
                setFocus();
                offerLocalClipboard();
                qInfo("VNC: connected to %s, %dx%d", qUtf8Printable(host()), width, height);
                emit statusMessage(QStringLiteral("Connected to %1 (%2×%3%4)")
                                       .arg(host())
                                       .arg(width)
                                       .arg(height)
                                       .arg(name.isEmpty() ? QString() : QStringLiteral(", ") + name));
                update();
            });
    connect(m_session, &VncSession::frameUpdated, this, [this](const QRect &area) {
        const QRect target = targetRect();
        QMutexLocker locker(&m_session->frameMutex());
        const QSize frame = m_session->frame().size();
        locker.unlock();
        if (frame.isEmpty() || target.isEmpty())
            return;
        const qreal sx = qreal(target.width()) / frame.width();
        const qreal sy = qreal(target.height()) / frame.height();
        update(QRectF(target.left() + area.left() * sx, target.top() + area.top() * sy,
                      area.width() * sx, area.height() * sy)
                   .toAlignedRect()
                   .adjusted(-1, -1, 1, 1));
    });
    connect(m_session, &VncSession::desktopResized, this, [this](int width, int height) {
        qInfo("VNC: desktop is %dx%d (tab %dx%d)", width, height, this->width(), this->height());
        updateCursor();
        update();
    });
    connect(m_session, &VncSession::resizeSupported, this, [this] {
        m_resizeSupported = true;
        requestTabSize();
    });
    connect(m_session, &VncSession::pointerChanged, this,
            [this](const QImage &shape, const QPoint &hotSpot) {
                m_pointerShape = shape;
                m_pointerHotSpot = hotSpot;
                updateCursor();
            });
    connect(m_session, &VncSession::remoteClipboardText, this, [this](const QString &text) {
        ++m_ignoreClipboardChanges;
        QApplication::clipboard()->setText(text);
    });
    if (m_settings.connection.options.clipboard == 1)
        connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &VncPage::offerLocalClipboard);
    connect(m_session, &VncSession::credentialsRequired, this,
            [this](QString *username, QString *password, bool needUsername, bool *accepted) {
                *accepted = askCredentials(username, password, needUsername);
            });
    connect(m_session, &VncSession::disconnected, this, [this](const QString &reason, bool error) {
        m_connected = false;
        m_resizeSupported = false;
        m_pressedKeys.clear();
        m_buttons = 0;
        m_pointerShape = QImage();
        updateCursor();
        stopTunnel();
        // A wrong saved password is not tried again.
        if (reason.contains(QStringLiteral("auth"), Qt::CaseInsensitive)) {
            m_settings.connection.password.fill(QChar(0));
            m_settings.connection.password.clear();
        }
        emit statusMessage(QStringLiteral("%1: %2").arg(host(), reason));
        constexpr int attempts = 6;
        if (error && m_wasConnected && m_settings.connection.options.autoReconnect == 1
            && m_reconnectAttempts < attempts) {
            ++m_reconnectAttempts;
            showOverlay(QStringLiteral("Connection lost. Reconnecting (attempt %1 of %2)…")
                            .arg(m_reconnectAttempts)
                            .arg(attempts),
                        false);
            QTimer::singleShot(5000, this, [this] {
                if (!m_connected)
                    start();
            });
            return;
        }
        showOverlay(reason, true, error);
    });
    showOverlay(QStringLiteral("Connecting to %1…").arg(host()), false);
}

VncPage::~VncPage()
{
    m_settings.connection.password.fill(QChar(0));
    for (auto &jump : m_settings.jumps)
        jump.password.fill(QChar(0));
    stopTunnel();
    // Closing the tab must not wait for the connection to end.
    m_session->retire();
}

void VncPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_started) {
        m_started = true;
        start();
    }
}

void VncPage::start()
{
    m_connectScale = scale();
    const auto &connection = m_settings.connection;
    if (m_settings.jumps.empty() || connection.options.listen == 1) {
        startVnc(connection.host, connection.port);
        return;
    }
    // Through jump hosts: the chain's local port stands in for the server.
    stopTunnel();
    m_tunnel = new JumpChain(this);
    connect(m_tunnel, &JumpChain::statusMessage, this, [this](const QString &message) {
        showOverlay(message, false);
    });
    connect(m_tunnel, &JumpChain::ready, this, [this](quint16 localPort) {
        startVnc(QStringLiteral("127.0.0.1"), localPort);
    });
    connect(m_tunnel, &JumpChain::failed, this, [this](const QString &reason, bool error) {
        if (m_connected)
            return; // the VNC connection reports its own end
        stopTunnel();
        m_session->disconnectFromHost();
        showOverlay(reason, true, error);
    });
    m_tunnel->open(m_settings.jumps, connection.host, connection.port);
}

void VncPage::startVnc(const QString &targetHost, quint16 port)
{
    auto connection = m_settings.connection;
    if (m_tunnel)
        connection.label = QStringLiteral("%1:%2 through a jump host").arg(connection.host).arg(connection.port);
    connection.host = targetHost;
    connection.port = port;
    m_session->connectToHost(connection);
}

void VncPage::stopTunnel()
{
    if (!m_tunnel)
        return;
    m_tunnel->disconnect(this);
    m_tunnel->deleteLater();
    m_tunnel = nullptr;
}

bool VncPage::askCredentials(QString *username, QString *password, bool needUsername)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Log On to %1").arg(host()));
    auto *form = new QFormLayout(&dialog);
    auto *user = new QLineEdit(*username, &dialog);
    auto *secret = new QLineEdit(&dialog);
    secret->setEchoMode(QLineEdit::Password);
    if (needUsername)
        form->addRow(QStringLiteral("User name:"), user);
    form->addRow(QStringLiteral("Password:"), secret);
    auto *save = new QCheckBox(QStringLiteral("Save the password in the vault"), &dialog);
    save->setVisible(m_settings.canSavePassword);
    form->addRow(QString(), save);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    (needUsername && user->text().isEmpty() ? user : secret)->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *username = user->text();
    *password = secret->text();
    if (save->isChecked() && !password->isEmpty())
        emit passwordSaveRequested(*password);
    return true;
}

void VncPage::showOverlay(const QString &text, bool ended, bool error)
{
    if (ended) {
        const int colon = text.indexOf(QStringLiteral(": "));
        m_endedPanel->setMessage(colon > 0 ? text.left(colon) : text,
                                 colon > 0 ? text.mid(colon + 2) : QString(), error);
        m_endedPanel->setGeometry(0, 0, width(), m_endedPanel->sizeHint().height());
    }
    m_overlayText->setText(text);
    m_overlayText->setVisible(!ended);
    m_endedPanel->setVisible(ended);
    m_overlay->setGeometry(rect());
    m_overlay->show();
    m_overlay->raise();
    m_endedPanel->raise();
    update();
}

qreal VncPage::scale() const
{
    return m_settings.connection.options.highDpi == 1 ? qMax<qreal>(1.0, devicePixelRatioF()) : 1.0;
}

QRect VncPage::targetRect() const
{
    QMutexLocker locker(&m_session->frameMutex());
    const QSize frame = m_session->frame().size();
    if (frame.isEmpty())
        return QRect();
    // Scale down to fit, never up, and centre; one remote pixel per
    // physical pixel on high-DPI screens when asked for.
    QSize size = (QSizeF(frame) / m_connectScale).toSize();
    if (size.width() > width() || size.height() > height())
        size.scale(this->size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
}

bool VncPage::toRemote(const QPointF &position, int *x, int *y) const
{
    const QRect target = targetRect();
    if (target.isEmpty())
        return false;
    QMutexLocker locker(&m_session->frameMutex());
    const QSize frame = m_session->frame().size();
    *x = qBound(0, int((position.x() - target.left()) * frame.width() / target.width()), frame.width() - 1);
    *y = qBound(0, int((position.y() - target.top()) * frame.height() / target.height()), frame.height() - 1);
    return true;
}

QImage VncPage::screenshot() const
{
    QMutexLocker locker(&m_session->frameMutex());
    return m_session->frame().convertToFormat(QImage::Format_RGB32);
}

void VncPage::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), Qt::black);
    const QRect target = targetRect();
    if (target.isEmpty())
        return;
    QMutexLocker locker(&m_session->frameMutex());
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          (QSizeF(target.size()) * devicePixelRatioF()).toSize()
                              != m_session->frame().size());
    painter.drawImage(target, m_session->frame());
}

bool VncPage::event(QEvent *event)
{
    // While connected, every key goes to the remote computer.
    if (m_connected && event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event->type() == QEvent::DevicePixelRatioChange) {
        if (m_resizeSupported)
            m_resizeTimer->start();
        updateCursor();
    }
#endif
    if (event->type() == QEvent::Resize) {
        if (m_overlay)
            m_overlay->setGeometry(rect());
        if (m_endedPanel)
            m_endedPanel->setGeometry(0, 0, width(), m_endedPanel->sizeHint().height());
        if (m_resizeSupported)
            m_resizeTimer->start();
        updateCursor();
    }
    return QWidget::event(event);
}

void VncPage::keyPressEvent(QKeyEvent *event)
{
    if (!m_connected && m_endedPanel->isVisible()) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            m_reconnectAttempts = 0;
            start();
        } else if (event->key() == Qt::Key_Escape) {
            requestClose();
        }
        event->accept();
        return;
    }
    if (handleFullScreenKey(event)) {
        releaseAllKeys();
        return;
    }
    event->accept();
    if (!m_connected)
        return;
    const quint32 keysym = keysymFor(event);
    if (keysym == 0)
        return;
    const quint32 id = event->nativeScanCode() ? event->nativeScanCode() : quint32(event->key());
    if (event->isAutoRepeat() && m_pressedKeys.contains(id)) {
        // Repeats press the same key again.
        m_session->sendKey(m_pressedKeys.value(id), true);
        return;
    }
    m_pressedKeys.insert(id, keysym);
    m_session->sendKey(keysym, true);
}

void VncPage::keyReleaseEvent(QKeyEvent *event)
{
    event->accept();
    if (!m_connected || event->isAutoRepeat())
        return;
    const quint32 id = event->nativeScanCode() ? event->nativeScanCode() : quint32(event->key());
    const quint32 keysym = m_pressedKeys.take(id);
    if (keysym)
        m_session->sendKey(keysym, false);
}

void VncPage::releaseAllKeys()
{
    for (const quint32 keysym : std::as_const(m_pressedKeys))
        m_session->sendKey(keysym, false);
    m_pressedKeys.clear();
}

void VncPage::refreshScreen()
{
    if (!m_connected)
        return;
    m_lastRefresh.start();
    m_session->requestFullUpdate();
}

// Coming back to the tab (a dialog closed, another tab or window was in
// front): some servers leave stale areas, so ask for the whole screen.
void VncPage::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    if (!m_lastRefresh.isValid() || m_lastRefresh.elapsed() > 1000)
        refreshScreen();
}

void VncPage::focusOutEvent(QFocusEvent *event)
{
    // Keys held while the focus leaves (Alt+Tab) would stay down remotely.
    if (m_connected)
        releaseAllKeys();
    QWidget::focusOutEvent(event);
}

void VncPage::sendPointer(const QPointF &position)
{
    int x = 0;
    int y = 0;
    if (m_connected && toRemote(position, &x, &y))
        m_session->sendPointer(x, y, m_buttons);
}

namespace {
int buttonBit(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton: return 4;
    case Qt::BackButton: return 128;
    case Qt::ForwardButton: return 256;
    default: return 0;
    }
}
}

void VncPage::mouseMoveEvent(QMouseEvent *event)
{
    sendPointer(event->position());
    trackFullScreenPointer(event->position());
}

void VncPage::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    m_buttons |= buttonBit(event->button());
    sendPointer(event->position());
}

void VncPage::mouseReleaseEvent(QMouseEvent *event)
{
    m_buttons &= ~buttonBit(event->button());
    sendPointer(event->position());
}

void VncPage::wheelEvent(QWheelEvent *event)
{
    int x = 0;
    int y = 0;
    event->accept();
    if (!m_connected || !toRemote(event->position(), &x, &y))
        return;
    // One press and release of the wheel "button" per notch (120 units).
    m_wheelRemainder += event->angleDelta().y();
    while (qAbs(m_wheelRemainder) >= 120) {
        const int bit = m_wheelRemainder > 0 ? 8 : 16;
        m_wheelRemainder += m_wheelRemainder > 0 ? -120 : 120;
        m_session->sendPointer(x, y, m_buttons | bit);
        m_session->sendPointer(x, y, m_buttons);
    }
    const int horizontal = event->angleDelta().x();
    if (qAbs(horizontal) >= 120) {
        const int bit = horizontal > 0 ? 32 : 64;
        m_session->sendPointer(x, y, m_buttons | bit);
        m_session->sendPointer(x, y, m_buttons);
    }
}

void VncPage::requestTabSize()
{
    if (!m_connected || !m_resizeSupported || !isVisible()
        || m_settings.connection.options.resolution != QStringLiteral("follow"))
        return;
    m_connectScale = scale();
    const QSize wanted(qMax(64, qRound(width() * m_connectScale)),
                       qMax(64, qRound(height() * m_connectScale)));
    QMutexLocker locker(&m_session->frameMutex());
    const QSize frame = m_session->frame().size();
    locker.unlock();
    if (frame == wanted)
        return;
    qInfo("VNC: requesting %dx%d", wanted.width(), wanted.height());
    m_session->requestDesktopSize(wanted.width(), wanted.height());
}

// The server's pointer shape as the local cursor, scaled like the desktop.
void VncPage::updateCursor()
{
    if (!m_connected) {
        unsetCursor();
        return;
    }
    if (m_pointerShape.isNull()) {
        // Without a shape from the server (or with it switched off), the
        // server draws the pointer into the picture: a small dot shows
        // where the local mouse is.
        if (m_settings.connection.options.remoteCursor != 1) {
            QPixmap dot(5, 5);
            dot.fill(Qt::transparent);
            QPainter painter(&dot);
            painter.setPen(Qt::black);
            painter.setBrush(Qt::white);
            painter.drawEllipse(0, 0, 4, 4);
            painter.end();
            setCursor(QCursor(dot, 2, 2));
        } else {
            unsetCursor();
        }
        return;
    }
    const QRect target = targetRect();
    QMutexLocker locker(&m_session->frameMutex());
    const QSize frame = m_session->frame().size();
    locker.unlock();
    const qreal scale = frame.isEmpty() || target.isEmpty() ? 1.0 / m_connectScale
                                                           : qreal(target.width()) / frame.width();
    const qreal ratio = devicePixelRatioF();
    const QSize size = (QSizeF(m_pointerShape.size()) * scale * ratio).toSize().expandedTo(QSize(1, 1));
    QPixmap pixmap = QPixmap::fromImage(size == m_pointerShape.size()
                                            ? m_pointerShape
                                            : m_pointerShape.scaled(size, Qt::IgnoreAspectRatio,
                                                                    Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(ratio);
    setCursor(QCursor(pixmap, int(m_pointerHotSpot.x() * scale), int(m_pointerHotSpot.y() * scale)));
}

void VncPage::offerLocalClipboard()
{
    if (m_ignoreClipboardChanges > 0) {
        --m_ignoreClipboardChanges;
        return;
    }
    if (!m_connected || m_settings.connection.options.clipboard != 1)
        return;
    const auto *mime = QApplication::clipboard()->mimeData();
    if (mime && mime->hasText())
        m_session->setLocalClipboard(mime->text());
}

bool VncPage::canSpanMonitors() const
{
    return m_settings.connection.options.allMonitors == 1;
}

void VncPage::fullScreenToggled()
{
    releaseAllKeys();
    m_resizeTimer->start();
    update();
}

void VncPage::sendKey(Key key, bool down)
{
    quint32 keysym = 0;
    switch (key) {
    case Key::Control: keysym = 0xffe3; break;
    case Key::Alt: keysym = 0xffe9; break;
    case Key::Delete: keysym = 0xffff; break;
    case Key::Backspace: keysym = 0xff08; break;
    case Key::Escape: keysym = 0xff1b; break;
    case Key::Tab: keysym = 0xff09; break;
    case Key::Print: keysym = 0xff61; break;
    case Key::Windows: keysym = 0xffeb; break;
    }
    m_session->sendKey(keysym, down);
}
