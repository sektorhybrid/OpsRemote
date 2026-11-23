// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "rdppage.h"

#include "clipboard.h"
#include "statusindicators.h"
#include "transfers.h"
#include "filedialogs.h"
#include "jumphost.h"
#include "sessionendedpanel.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLocale>
#include <QMimeData>
#include <QProgressBar>
#include <QStandardPaths>
#include <QUrl>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QCursor>
#include <QPainter>
#include <QPixmap>
#include <QFileInfo>
#include <QPushButton>
#include <QScreen>
#include <QTabWidget>
#include <QWindow>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <freerdp/input.h>
#include <freerdp/scancode.h>

namespace {
// The RDP (PC/AT set 1) scancode of a key event, or 0 if unknown.
quint32 rdpScancode(const QKeyEvent *event)
{
    const quint32 native = event->nativeScanCode();
#ifdef Q_OS_WIN
    // Windows reports the set 1 scancode, with 0x100 for extended keys.
    if (native == 0)
        return 0;
    return MAKE_RDP_SCANCODE(native & 0xFF, (native & 0x100) != 0);
#else
    // X11 and Wayland report XKB keycodes: Linux evdev codes plus 8.
    if (native < 8)
        return 0;
    const quint32 evdev = native - 8;
    // evdev 1-88 are the set 1 scancodes themselves (Esc to F12).
    if (evdev >= 1 && evdev <= 88)
        return MAKE_RDP_SCANCODE(evdev, false);
    switch (evdev) {
    case 96: return MAKE_RDP_SCANCODE(0x1C, true); // keypad Enter
    case 97: return MAKE_RDP_SCANCODE(0x1D, true); // right Ctrl
    case 98: return MAKE_RDP_SCANCODE(0x35, true); // keypad /
    case 99: return MAKE_RDP_SCANCODE(0x37, true); // Print Screen
    case 100: return MAKE_RDP_SCANCODE(0x38, true); // right Alt (AltGr)
    case 102: return MAKE_RDP_SCANCODE(0x47, true); // Home
    case 103: return MAKE_RDP_SCANCODE(0x48, true); // Up
    case 104: return MAKE_RDP_SCANCODE(0x49, true); // Page Up
    case 105: return MAKE_RDP_SCANCODE(0x4B, true); // Left
    case 106: return MAKE_RDP_SCANCODE(0x4D, true); // Right
    case 107: return MAKE_RDP_SCANCODE(0x4F, true); // End
    case 108: return MAKE_RDP_SCANCODE(0x50, true); // Down
    case 109: return MAKE_RDP_SCANCODE(0x51, true); // Page Down
    case 110: return MAKE_RDP_SCANCODE(0x52, true); // Insert
    case 111: return MAKE_RDP_SCANCODE(0x53, true); // Delete
    case 117: return MAKE_RDP_SCANCODE(0x59, false); // keypad =
    case 121: return MAKE_RDP_SCANCODE(0x7E, false); // keypad , (Brazilian)
    case 124: return MAKE_RDP_SCANCODE(0x7D, false); // Yen
    case 89: return MAKE_RDP_SCANCODE(0x73, false); // Ro (Japanese)
    case 92: return MAKE_RDP_SCANCODE(0x79, false); // Henkan
    case 93: return MAKE_RDP_SCANCODE(0x70, false); // Katakana/Hiragana
    case 94: return MAKE_RDP_SCANCODE(0x7B, false); // Muhenkan
    case 125: return MAKE_RDP_SCANCODE(0x5B, true); // left Windows
    case 126: return MAKE_RDP_SCANCODE(0x5C, true); // right Windows
    case 127: return MAKE_RDP_SCANCODE(0x5D, true); // Menu
    default: return 0;
    }
#endif
}
}

RdpPage::RdpPage(const RdpConnectionSettings &settings, QWidget *parent)
    : RemoteScreenPage(parent), m_settings(settings)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled, false);

    buildOverlay(QStringLiteral("rdpOverlay"));
    connect(m_endedPanel, &SessionEndedPanel::reconnectRequested, this, &RdpPage::start);
    connect(m_endedPanel, &SessionEndedPanel::closeRequested, this, &RdpPage::requestClose);

    // Clipboard file copies offer themselves in a bar across the top of the
    // tab (Save to…, Open folder); their progress is in the transfer list.
    m_bar = new QWidget(this);
    m_bar->setAttribute(Qt::WA_StyledBackground);
    m_bar->setObjectName(QStringLiteral("rdpClipboardBar"));
    m_bar->setStyleSheet(QStringLiteral(
        "#rdpClipboardBar { background: palette(window); border-bottom: 1px solid palette(mid); }"));
    auto *barLayout = new QHBoxLayout(m_bar);
    barLayout->setContentsMargins(10, 4, 6, 4);
    m_barText = new QLabel(m_bar);
    m_barText->setTextFormat(Qt::PlainText);
    m_barAction = new QPushButton(m_bar);
    m_barClose = new QPushButton(QStringLiteral("✕"), m_bar);
    m_barClose->setFlat(true);
    m_barClose->setToolTip(QStringLiteral("Close"));
    for (auto *button : {m_barAction, m_barClose})
        button->setFocusPolicy(Qt::NoFocus);
    barLayout->addWidget(m_barText, 1);
    barLayout->addWidget(m_barAction);
    barLayout->addWidget(m_barClose);
    m_bar->hide();
    // The strip turned on or off: the bar hands over, or takes over.
    connect(TabPrompt::hub(), &TabPrompt::changed, this, [this] {
        if (m_barShown && m_bar->isHidden() != TabPrompt::inStrip())
            m_bar->setVisible(!TabPrompt::inStrip());
    });
    m_barTimer = new QTimer(this);
    m_barTimer->setSingleShot(true);
    connect(m_barTimer, &QTimer::timeout, this, &RdpPage::hideBar);
    connect(m_barClose, &QPushButton::clicked, this, &RdpPage::hideBar);

    connect(m_barAction, &QPushButton::clicked, this, [this] {
        switch (m_barActionKind) {
        case BarAction::Save: {
            const auto folder = FileDialogs::existingDirectory(
                this, QStringLiteral("Save Copied Files To"),
                QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
            if (folder.isEmpty())
                return;
            m_savedFolder = folder;
            hideBar();
            const QPointer<RdpPage> page(this);
            m_saveTransfer = TransferList::instance()->start(
                this, QStringLiteral("Files from the remote computer"), false,
                [page] {
                    if (page)
                        page->m_session->cancelRemoteFileSave();
                },
                QStringLiteral("Stop saving the copied files"));
            m_session->saveRemoteFiles(folder);
            break;
        }
        case BarAction::Cancel:
            m_session->cancelRemoteFileSave();
            break;
        case BarAction::OpenFolder:
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_savedFolder));
            hideBar();
            break;
        case BarAction::None:
            break;
        }
    });
    setAcceptDrops(true);

    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(400);
    connect(m_resizeTimer, &QTimer::timeout, this, &RdpPage::requestTabSize);
    // Windows ignores size requests while it is still signing in; ask again
    // a few times until the desktop has the size asked for.
    m_resizeRetryTimer = new QTimer(this);
    m_resizeRetryTimer->setSingleShot(true);
    m_resizeRetryTimer->setInterval(2000);
    connect(m_resizeRetryTimer, &QTimer::timeout, this, [this] {
        if (m_resizeRetries < 5)
            requestTabSize();
    });

    m_session = new RdpSession(this);
    connect(m_session, &RdpSession::resizeSupported, this, [this] {
        m_resizeSupported = true;
        // The tab may have changed size while connecting.
        requestTabSize();
    });
    connect(m_session, &RdpSession::statusMessage, this, [this](const QString &message) {
        showOverlay(message, false);
    });
    // Whatever a paste would paste here: a copy anywhere, or text only
    // selected in OpsRemote (clipboard.h).
    connect(Clipboard::notifier(), &Clipboard::Notifier::newestChanged, this, &RdpPage::offerLocalClipboard);
    connect(m_session, &RdpSession::remoteClipboardText, this, [this](const QString &text) {
        // Copied on the remote computer: this computer can paste it now.
        ++m_ignoreClipboardChanges;
        QApplication::clipboard()->setText(text);
    });
    connect(m_session, &RdpSession::remoteFilesAvailable, this,
            [this](const QStringList &names, quint64 total) {
                if (names.isEmpty())
                    return;
                const auto what = names.size() == 1
                                      ? QStringLiteral("\"%1\"").arg(names.first())
                                      : QStringLiteral("%1 items").arg(names.size());
                showBar(QStringLiteral("Copied on the remote computer: %1 (%2)")
                            .arg(what, QLocale().formattedDataSize(qint64(total))),
                        BarAction::Save, true);
            });
    connect(m_session, &RdpSession::remoteFilesProgress, this,
            [this](quint64 done, quint64 total, const QString &current) {
                TransferList::instance()->update(m_saveTransfer, current.section(QLatin1Char('/'), -1), done, total,
                                                 QStringLiteral("%1 of %2").arg(QLocale().formattedDataSize(qint64(done)),
                                                                                QLocale().formattedDataSize(qint64(total))));
            });
    connect(m_session, &RdpSession::localFilesProgress, this,
            [this](quint64 sent, quint64 total, const QString &current) {
                // Pasting files on the remote computer: one transfer for the paste.
                auto *transfers = TransferList::instance();
                if (total == 0 || sent >= total) {
                    if (m_pasteTransfer) {
                        transfers->finish(m_pasteTransfer, TransferList::State::Done);
                        m_pasteTransfer = 0;
                        m_pasteStallTimer->stop();
                    }
                    return;
                }
                if (!m_pasteTransfer) {
                    const QPointer<RdpPage> page(this);
                    m_pasteTransfer = transfers->start(
                        this, current, true,
                        [page] {
                            if (page)
                                page->endPaste(TransferList::State::Cancelled, {});
                        },
                        QStringLiteral("Stop sending the files (the remote computer ends its copy)"));
                }
                // Windows asks for the files chunk by chunk; when it stops
                // asking before the end, its copy has failed or was cancelled
                // there, and nothing else would tell.
                m_pasteStallTimer->start();
                transfers->update(m_pasteTransfer, current, sent, total,
                                  QStringLiteral("%1 of %2").arg(QLocale().formattedDataSize(qint64(sent)),
                                                                 QLocale().formattedDataSize(qint64(total))));
            });
    m_pasteStallTimer = new QTimer(this);
    m_pasteStallTimer->setSingleShot(true);
    // OPSREMOTE_TEST_PASTE_STALL_MS: shorter, for tests.
    m_pasteStallTimer->setInterval(qEnvironmentVariableIntValue("OPSREMOTE_TEST_PASTE_STALL_MS") > 0
                                       ? qEnvironmentVariableIntValue("OPSREMOTE_TEST_PASTE_STALL_MS")
                                       : 30000);
    connect(m_pasteStallTimer, &QTimer::timeout, this, [this] {
        endPaste(TransferList::State::Failed,
                 QStringLiteral("The remote computer stopped copying the files (nothing asked for 30 seconds)."));
    });
    connect(m_session, &RdpSession::localFilesWithdrawn, this, [this] {
        endPaste(TransferList::State::Failed,
                 QStringLiteral("The copy on the remote computer stopped: it took a clipboard of its own."));
    });
    connect(m_session, &RdpSession::remoteFilesSaved, this,
            [this](const QStringList &paths, const QString &error) {
                // RdpClipboard::cancelSave reports "Cancelled.".
                TransferList::instance()->finish(m_saveTransfer, error.isEmpty() ? TransferList::State::Done
                                                                 : error == QStringLiteral("Cancelled.")
                                                                     ? TransferList::State::Cancelled
                                                                     : TransferList::State::Failed);
                m_saveTransfer = 0;
                if (!error.isEmpty()) {
                    showBar(QStringLiteral("The files were not saved. %1").arg(error), BarAction::None, true);
                    return;
                }
                // Also on this computer's clipboard, ready to paste in a file manager.
                auto *mime = new QMimeData;
                QList<QUrl> urls;
                for (const auto &path : paths)
                    urls.append(QUrl::fromLocalFile(path));
                mime->setUrls(urls);
                ++m_ignoreClipboardChanges;
                QApplication::clipboard()->setMimeData(mime);
                showBar(QStringLiteral("Saved %1 to %2. They are on this computer's clipboard too.")
                            .arg(paths.size() == 1 ? QStringLiteral("1 item")
                                                   : QStringLiteral("%1 items").arg(paths.size()),
                                 QDir::toNativeSeparators(m_savedFolder)),
                        BarAction::OpenFolder, true, 15000);
            });
    connect(m_session, &RdpSession::graphicsMode, this, [this](const QString &mode) {
        m_graphicsMode = mode;
    });
    connect(m_session, &RdpSession::connected, this, [this](int width, int height) {
        m_connected = true;
        qInfo("RDP: connected to %s, %dx%d, %s, scale %.2f", qUtf8Printable(m_settings.host), width,
              height, m_graphicsMode.isEmpty() ? "bitmap updates" : qUtf8Printable(m_graphicsMode),
              m_connectScale);
        offerLocalClipboard();
        m_overlay->hide();
        m_endedPanel->hide();
        setFocus();
        emit statusMessage({}); // connected: an earlier disconnect reason is old news
        update();
    });
    connect(m_session, &RdpSession::frameUpdated, this, &RdpPage::repaintRemoteArea);
    connect(m_session, &RdpSession::desktopResized, this, [this](int width, int height) {
        qInfo("RDP: desktop resized to %dx%d (tab %dx%d, scale %.2f)", width, height, this->width(),
              this->height(), m_connectScale);
        updateCursor();
        update();
    });
    connect(m_session, &RdpSession::pointerChanged, this,
            [this](const QImage &shape, const QPoint &hotSpot) {
                m_pointerShape = shape;
                m_pointerHotSpot = hotSpot;
                m_pointerHidden = false;
                updateCursor();
            });
    connect(m_session, &RdpSession::pointerHidden, this, [this] {
        m_pointerHidden = true;
        updateCursor();
    });
    connect(m_session, &RdpSession::pointerDefault, this, [this] {
        m_pointerShape = QImage();
        m_pointerHidden = false;
        updateCursor();
    });
    connect(m_session, &RdpSession::disconnected, this, [this](const QString &reason, bool error) {
        m_connected = false;
        m_resizeSupported = false;
        m_pressedKeys.clear();
        m_pointerShape = QImage();
        m_pointerHidden = false;
        updateCursor();
        stopJumpChain();
        showOverlay(reason, true, error);
        emit statusMessage(QStringLiteral("%1: %2").arg(m_settings.host, reason));
    });
    connect(m_session, &RdpSession::certificateVerificationRequired, this,
            [this](const QString &host, const QString &subject, const QString &issuer,
                   const QString &fingerprint, bool changed, RdpCertificateDecision *decision) {
                QMessageBox box(this);
                box.setIcon(changed ? QMessageBox::Warning : QMessageBox::Question);
                box.setWindowTitle(QStringLiteral("Remote Desktop Certificate"));
                box.setText(changed ? QStringLiteral("The certificate of %1 has CHANGED.").arg(host)
                                    : QStringLiteral("The identity of %1 can't be verified.").arg(host));
                box.setInformativeText(
                    QStringLiteral("Subject: %1\nIssuer: %2\nFingerprint: %3\n\n%4")
                        .arg(subject, issuer, fingerprint,
                             changed ? QStringLiteral("This can mean the server was reinstalled, or "
                                                      "that someone is intercepting the connection.")
                                     : QStringLiteral("Windows computers usually use a self-signed "
                                                      "certificate. Only connect if this is the "
                                                      "computer you expect.")));
                auto *once = box.addButton(QStringLiteral("Connect Once"), QMessageBox::AcceptRole);
                auto *trust = box.addButton(QStringLiteral("Trust and Connect"), QMessageBox::AcceptRole);
                box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
                box.setDefaultButton(changed ? once : trust);
                box.exec();
                *decision = box.clickedButton() == trust ? RdpCertificateDecision::AcceptAndSave
                            : box.clickedButton() == once ? RdpCertificateDecision::AcceptOnce
                                                          : RdpCertificateDecision::Reject;
            });
    connect(m_session, &RdpSession::credentialsRequired, this,
            [this](QString *username, QString *password, QString *domain, bool gateway,
                   bool *accepted) {
                QDialog dialog(this);
                dialog.setWindowTitle(gateway ? QStringLiteral("Log On to the RD Gateway")
                                              : QStringLiteral("Log On to %1").arg(m_settings.host));
                auto *form = new QFormLayout(&dialog);
                auto *user = new QLineEdit(*username, &dialog);
                auto *secret = new QLineEdit(&dialog);
                secret->setEchoMode(QLineEdit::Password);
                auto *realm = new QLineEdit(*domain, &dialog);
                realm->setPlaceholderText(QStringLiteral("Optional"));
                form->addRow(QStringLiteral("User name:"), user);
                form->addRow(QStringLiteral("Password:"), secret);
                form->addRow(QStringLiteral("Domain:"), realm);
                auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                                     &dialog);
                form->addRow(buttons);
                connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                (user->text().isEmpty() ? user : secret)->setFocus();
                *accepted = dialog.exec() == QDialog::Accepted;
                if (*accepted) {
                    *username = user->text();
                    *password = secret->text();
                    *domain = realm->text();
                }
            });
    showOverlay(QStringLiteral("Connecting to %1…").arg(m_settings.host), false);
}

RdpPage::~RdpPage()
{
    m_settings.password.fill(QChar(0));
    for (auto &jump : m_settings.jumps)
        jump.password.fill(QChar(0));
    stopJumpChain();
    // Closing the tab must not wait for FreeRDP to return.
    m_session->retire();
}

void RdpPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Connect once the tab has its real size, which becomes the remote
    // desktop size.
    if (!m_started) {
        m_started = true;
        start();
    }
}

void RdpPage::start()
{
    // A fixed size ("1920x1080"), or the tab's size for the other modes.
    const auto fixed = m_settings.options.resolution.split(QLatin1Char('x'));
    if (fixed.size() == 2) {
        m_settings.width = fixed[0].toInt();
        m_settings.height = fixed[1].toInt();
    } else {
        m_settings.width = qMax(640, qRound(width() * scale()));
        m_settings.height = qMax(480, qRound(height() * scale()));
    }
    m_settings.scale = scale();
    m_connectScale = m_settings.scale;
    showOverlay(QStringLiteral("Connecting to %1…").arg(m_settings.host), false);
    if (m_settings.jumps.empty()) {
        m_session->connectToHost(m_settings);
        return;
    }

    // Through jump hosts: the chain's local port stands in for the server.
    stopJumpChain();
    m_jumpChain = new JumpChain(this);
    connect(m_jumpChain, &JumpChain::statusMessage, this, [this](const QString &message) {
        showOverlay(message, false);
    });
    connect(m_jumpChain, &JumpChain::ready, this, [this](quint16 localPort) {
        auto settings = m_settings;
        settings.certificateName = QStringLiteral("%1:%2").arg(m_settings.host).arg(m_settings.port);
        settings.host = QStringLiteral("127.0.0.1");
        settings.port = localPort;
        settings.jumps.clear();
        showOverlay(QStringLiteral("Connecting to %1…").arg(m_settings.host), false);
        m_session->connectToHost(settings);
        settings.password.fill(QChar(0));
    });
    connect(m_jumpChain, &JumpChain::failed, this, [this](const QString &reason, bool error) {
        if (m_connected)
            return; // the RDP connection reports its own end
        stopJumpChain();
        showOverlay(reason, true, error);
    });
    // FreeRDP may connect again (another security mode, automatic reconnect).
    m_jumpChain->open(m_settings.jumps, m_settings.host, m_settings.port, false);
}

void RdpPage::stopJumpChain()
{
    if (!m_jumpChain)
        return;
    m_jumpChain->disconnect(this);
    m_jumpChain->deleteLater();
    m_jumpChain = nullptr;
}

void RdpPage::showOverlay(const QString &text, bool ended, bool error)
{
    if (ended) {
        // "Session ended: <why>" becomes a title and a detail line.
        const int colon = text.indexOf(QStringLiteral(": "));
        m_endedPanel->setMessage(colon > 0 ? text.left(colon) : text,
                                 colon > 0 ? text.mid(colon + 2) : QString(), error);
    }
    m_overlayText->setText(text);
    m_overlayText->setVisible(!ended);
    // The clipboard bar goes first: the ended message takes the tab's prompt.
    // A paste there is over too (the reason is the session's).
    if (ended) {
        hideBar();
        endPaste(TransferList::State::Failed, {});
    }
    m_endedPanel->setVisible(ended);
    if (ended) {
        m_endedPanel->setGeometry(0, 0, width(), m_endedPanel->sizeHint().height());
    }
    m_overlay->setGeometry(rect());
    m_overlay->show();
    m_overlay->raise();
    m_endedPanel->raise();
    update();
}

QMutex &RdpPage::frameMutex() const
{
    return m_session->frameMutex();
}

const QImage &RdpPage::frame() const
{
    return m_session->frame();
}

bool RdpPage::toRemote(const QPointF &position, int *x, int *y) const
{
    const QRect target = targetRect();
    if (target.isEmpty())
        return false;
    QMutexLocker locker(&m_session->frameMutex());
    const QSize frame = m_session->frame().size();
    *x = qBound(0, int((position.x() - target.left()) * frame.width() / target.width()),
                frame.width() - 1);
    *y = qBound(0, int((position.y() - target.top()) * frame.height() / target.height()),
                frame.height() - 1);
    return true;
}

void RdpPage::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    paintFrame(painter, event->rect());
}

bool RdpPage::event(QEvent *event)
{
    if (event->type() == QEvent::Resize && m_bar)
        m_bar->setGeometry(0, 0, width(), m_bar->sizeHint().height());
    return RemoteScreenPage::event(event);
}

void RdpPage::sendKeyEvent(QKeyEvent *event, bool down)
{
    if (!m_connected)
        return;
    if (event->key() == Qt::Key_Pause) {
        if (down)
            m_session->sendPause();
        return;
    }
    const quint32 code = rdpScancode(event);
    if (code != 0) {
        m_session->sendKey(code, down, event->isAutoRepeat());
        if (down)
            m_pressedKeys.insert(code);
        else
            m_pressedKeys.remove(code);
        return;
    }
    // A key without a known scancode: send its text as Unicode.
    for (const QChar character : event->text())
        m_session->sendUnicode(character.unicode(), down);
}

void RdpPage::keyPressEvent(QKeyEvent *event)
{
    // After the session ended: Enter reconnects, Esc closes the tab.
    if (!m_connected && m_endedPanel->isOffered()) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            start();
        else if (event->key() == Qt::Key_Escape)
            requestClose();
        event->accept();
        return;
    }
    // Ctrl+Alt+Enter enters and leaves full screen, as in mstsc.
    if (handleFullScreenKey(event))
        return;
    sendKeyEvent(event, true);
    event->accept();
}

void RdpPage::keyReleaseEvent(QKeyEvent *event)
{
    sendKeyEvent(event, false);
    event->accept();
}

void RdpPage::releaseAllKeys()
{
    for (const quint32 code : std::as_const(m_pressedKeys))
        m_session->sendKey(code, false, false);
    m_pressedKeys.clear();
}

void RdpPage::focusOutEvent(QFocusEvent *event)
{
    // Keys held while the focus leaves (Alt+Tab) would stay down remotely.
    if (m_connected)
        releaseAllKeys();
    QWidget::focusOutEvent(event);
}

void RdpPage::sendButton(Qt::MouseButton button, bool down, const QPointF &position)
{
    int x = 0;
    int y = 0;
    if (!m_connected || !toRemote(position, &x, &y))
        return;
    const quint16 pressed = down ? PTR_FLAGS_DOWN : 0;
    switch (button) {
    case Qt::LeftButton:
        m_session->sendMouse(PTR_FLAGS_BUTTON1 | pressed, x, y);
        break;
    case Qt::RightButton:
        m_session->sendMouse(PTR_FLAGS_BUTTON2 | pressed, x, y);
        break;
    case Qt::MiddleButton:
        m_session->sendMouse(PTR_FLAGS_BUTTON3 | pressed, x, y);
        break;
    case Qt::BackButton:
        m_session->sendExtendedMouse(PTR_XFLAGS_BUTTON1 | (down ? PTR_XFLAGS_DOWN : 0), x, y);
        break;
    case Qt::ForwardButton:
        m_session->sendExtendedMouse(PTR_XFLAGS_BUTTON2 | (down ? PTR_XFLAGS_DOWN : 0), x, y);
        break;
    default:
        break;
    }
}

void RdpPage::mouseMoveEvent(QMouseEvent *event)
{
    int x = 0;
    int y = 0;
    if (m_connected && toRemote(event->position(), &x, &y))
        m_session->sendMouse(PTR_FLAGS_MOVE, x, y);
    trackFullScreenPointer(event->position());
}

void RdpPage::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    sendButton(event->button(), true, event->position());
}

void RdpPage::mouseReleaseEvent(QMouseEvent *event)
{
    sendButton(event->button(), false, event->position());
}

void RdpPage::wheelEvent(QWheelEvent *event)
{
    int x = 0;
    int y = 0;
    if (!m_connected || !toRemote(event->position(), &x, &y))
        return;
    // RDP wheel steps are 120 per notch like Qt's; send whole steps only.
    m_wheelRemainder += event->angleDelta().y();
    while (qAbs(m_wheelRemainder) >= 1) {
        const int step = qBound(-255, m_wheelRemainder, 255);
        m_wheelRemainder -= step;
        quint16 flags = PTR_FLAGS_WHEEL;
        if (step < 0)
            flags |= PTR_FLAGS_WHEEL_NEGATIVE | ((0x200 + step) & WheelRotationMask);
        else
            flags |= step & WheelRotationMask;
        m_session->sendMouse(flags, x, y);
    }
    event->accept();
}

void RdpPage::requestTabSize()
{
    if (!m_connected || !m_resizeSupported || !isVisible())
        return;
    QMutexLocker locker(&m_session->frameMutex());
    const QSize frame = m_session->frame().size();
    locker.unlock();
    m_connectScale = scale();
    if (isFullScreenDesktop() && spansAllMonitors()) {
        const auto monitors = monitorLayout();
        if (monitors.size() > 1) {
            m_session->requestMonitorLayout(monitors, m_connectScale);
            return;
        }
    }
    const QSize wanted(qMax(200, qRound(width() * m_connectScale)) & ~1,
                       qMax(200, qRound(height() * m_connectScale)) & ~1);
    if (frame == wanted)
        return;
    if (wanted != m_requestedSize) {
        m_requestedSize = wanted;
        m_resizeRetries = 0;
    }
    ++m_resizeRetries;
    qInfo("RDP: requesting %dx%d", wanted.width(), wanted.height());
    m_session->requestDesktopSize(wanted.width(), wanted.height(), m_connectScale);
    m_resizeRetryTimer->start();
}

qreal RdpPage::scale() const
{
    return m_settings.options.highDpi == 1 ? qMax<qreal>(1.0, devicePixelRatioF()) : 1.0;
}

// The local monitors this full-screen window covers, in remote pixels
// relative to the primary one (which comes first and is at 0,0).
QList<QRect> RdpPage::monitorLayout() const
{
    // Testing: OPSREMOTE_TEST_MONITORS=N splits the window into N
    // monitors side by side, where the test display has only one.
    if (const int fake = testMonitorCount(); fake > 1) {
        const int width = qRound(window()->width() * m_connectScale) / fake & ~1;
        const int height = qRound(window()->height() * m_connectScale);
        QList<QRect> monitors;
        for (int i = 0; i < fake; ++i)
            monitors.append(QRect(i * width, 0, width, height));
        return monitors;
    }
    QList<QScreen *> screens;
    for (auto *screen : QGuiApplication::screens()) {
        if (screen->geometry().intersects(window()->geometry()))
            screens.append(screen);
    }
    auto *primary = QGuiApplication::primaryScreen();
    if (!screens.contains(primary))
        primary = screens.isEmpty() ? nullptr : screens.first();
    if (!primary)
        return {};
    screens.removeAll(primary);
    screens.prepend(primary);
    const qreal ratio = m_connectScale;
    const QPoint origin = primary->geometry().topLeft();
    QList<QRect> monitors;
    for (auto *screen : std::as_const(screens)) {
        const QRect area = screen->geometry();
        monitors.append(QRect(qRound((area.left() - origin.x()) * ratio),
                              qRound((area.top() - origin.y()) * ratio),
                              qRound(area.width() * ratio), qRound(area.height() * ratio)));
    }
    return monitors;
}

// cursor, scaled like the desktop when the desktop is shown smaller.
void RdpPage::updateCursor()
{
    if (m_pointerHidden) {
        setCursor(Qt::BlankCursor);
        return;
    }
    if (m_pointerShape.isNull()) {
        unsetCursor();
        return;
    }
    showPointerShape();
}

// What a clipboard holds, to tell a real new copy from a repeated notice.
static QString clipboardSignature(const QMimeData *mime)
{
    QStringList parts{mime->text()};
    for (const auto &url : mime->urls())
        parts.append(url.toString());
    return parts.join(QLatin1Char('\n'));
}

// Offers what this computer's clipboard holds to the remote computer:
// local files (if files are shared), otherwise text.
void RdpPage::offerLocalClipboard()
{
    if (m_ignoreClipboardChanges > 0) {
        --m_ignoreClipboardChanges;
        return;
    }
    if (!m_connected)
        return;
    // Null with nothing copied yet: text only selected in OpsRemote is
    // offered all the same.
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (m_dropOffered && mime) {
        const auto now = clipboardSignature(mime);
        if (!m_dropNoticeSeen || now == m_clipboardAtDrop) {
            // Not a new copy: the dropped files stay offered.
            m_dropNoticeSeen = true;
            m_clipboardAtDrop = now;
            return;
        }
        m_dropOffered = false;
    }
    QStringList files;
    if (mime && m_settings.options.clipboardFiles == 1 && mime->hasUrls()) {
        for (const auto &url : mime->urls()) {
            if (url.isLocalFile())
                files.append(url.toLocalFile());
        }
    }
    // Other files than those being pasted there: that paste cannot go on.
    if (m_pasteTransfer && files != m_offeredFiles)
        endPaste(TransferList::State::Failed,
                 QStringLiteral("The copy on the remote computer stopped: a new copy here replaced the files."));
    // A selection made after the files were copied is the newest: its text.
    const QString text = Clipboard::textToPaste(false);
    if (!files.isEmpty() && mime && text != mime->text())
        files.clear();
    m_offeredFiles = files;
    m_session->setLocalClipboard(files.isEmpty() ? text : QString(), files);
}

void RdpPage::dragEnterEvent(QDragEnterEvent *event)
{
    if (!m_connected || m_settings.options.clipboardFiles != 1 || !event->mimeData()->hasUrls())
        return;
    for (const auto &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            event->acceptProposedAction();
            return;
        }
    }
}

// RDP has no drag and drop: dropped files go onto the remote clipboard, to
// be pasted in the remote window of the user's choice.
void RdpPage::dropEvent(QDropEvent *event)
{
    QStringList files;
    for (const auto &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            files.append(url.toLocalFile());
    }
    if (files.isEmpty())
        return;
    event->acceptProposedAction();
    if (m_pasteTransfer && files != m_offeredFiles)
        endPaste(TransferList::State::Failed,
                 QStringLiteral("The copy on the remote computer stopped: files dropped here replaced the files."));
    m_offeredFiles = files;
    m_session->setLocalClipboard(QString(), files);
    if (const auto *mime = QApplication::clipboard()->mimeData()) {
        m_clipboardAtDrop = clipboardSignature(mime);
        m_dropOffered = true;
        m_dropNoticeSeen = false;
    }
    setFocus();
    showBar(QStringLiteral("%1 on the remote clipboard: press Ctrl+V in a remote folder to paste.")
                .arg(files.size() == 1 ? QStringLiteral("\"%1\" is").arg(QFileInfo(files.first()).fileName())
                                       : QStringLiteral("%1 items are").arg(files.size())),
            BarAction::None, true, 12000);
}

// The paste of this computer's files on the remote computer is over before
// its end: cancelled here (its further requests are refused, so Windows ends
// its copy), or it stopped there. reason goes to the tab's messages.
void RdpPage::endPaste(TransferList::State state, const QString &reason)
{
    if (!m_pasteTransfer)
        return;
    m_pasteStallTimer->stop();
    if (state == TransferList::State::Cancelled)
        m_session->stopLocalFileTransfer();
    TransferList::instance()->finish(std::exchange(m_pasteTransfer, 0), state);
    if (!reason.isEmpty())
        emit statusMessage(reason);
}

void RdpPage::showBar(const QString &text, BarAction action, bool closable, int hideAfterMs)
{
    m_barText->setText(text);
    m_barActionKind = action;
    m_barAction->setVisible(action != BarAction::None);
    m_barAction->setText(action == BarAction::Save         ? QStringLiteral("Save to…")
                         : action == BarAction::Cancel     ? QStringLiteral("Cancel")
                         : action == BarAction::OpenFolder ? QStringLiteral("Open folder")
                                                           : QString());
    m_barClose->setVisible(closable);
    m_bar->setGeometry(0, 0, width(), m_bar->sizeHint().height());
    // In the tab strip when it is on, else across the top of the tab.
    QList<TabPrompt::Action> actions;
    if (action != BarAction::None)
        actions.append({m_barAction->text(), QString(), [this] { m_barAction->click(); }});
    // A choice has no ✕ in the strip: leaving the files is one of its answers.
    if (action == BarAction::Save)
        actions.append({QStringLiteral("Not now"), QStringLiteral("Leave the copied files on the remote computer"),
                        [this] { hideBar(); }});
    TabPrompt::of(this)->set(text, false, actions, closable ? std::function<void()>([this] { hideBar(); })
                                                            : std::function<void()>());
    m_barShown = true;
    m_bar->setVisible(!TabPrompt::inStrip());
    m_bar->raise();
    if (hideAfterMs > 0)
        m_barTimer->start(hideAfterMs);
    else
        m_barTimer->stop();
}

void RdpPage::hideBar()
{
    m_barTimer->stop();
    m_bar->hide();
    if (m_barShown)
        TabPrompt::of(this)->clear();
    m_barShown = false;
}

bool RdpPage::canSpanMonitors() const
{
    return m_settings.options.allMonitors == 1 && m_resizeSupported && m_session->maxMonitors() > 1;
}

void RdpPage::fullScreenToggled()
{
    releaseAllKeys();
    m_resizeTimer->start();
}

QImage RdpPage::screenshot() const
{
    QMutexLocker locker(&m_session->frameMutex());
    return m_session->frame().copy();
}

void RdpPage::sendKey(Key key, bool down)
{
    quint32 code = 0;
    switch (key) {
    case Key::Control: code = MAKE_RDP_SCANCODE(0x1D, false); break;
    case Key::Alt: code = MAKE_RDP_SCANCODE(0x38, false); break;
    case Key::Delete: code = MAKE_RDP_SCANCODE(0x53, true); break;
    case Key::Backspace: code = MAKE_RDP_SCANCODE(0x0E, false); break;
    case Key::Escape: code = MAKE_RDP_SCANCODE(0x01, false); break;
    case Key::Tab: code = MAKE_RDP_SCANCODE(0x0F, false); break;
    case Key::Print: code = MAKE_RDP_SCANCODE(0x37, true); break;
    case Key::Windows: code = MAKE_RDP_SCANCODE(0x5B, true); break;
    }
    m_session->sendKey(code, down, false);
}
