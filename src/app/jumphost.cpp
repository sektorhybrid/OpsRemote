// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "jumphost.h"

#include <QComboBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QWidget>

bool JumpHost::parseAddress(const QString &spec, QString *user, QString *host, quint16 *port)
{
    QString target = spec.trimmed();
    user->clear();
    *port = 0;
    if (const int at = target.lastIndexOf(QLatin1Char('@')); at >= 0) {
        *user = target.left(at);
        target = target.mid(at + 1);
    }
    // "host:port"; a bare IPv6 address has more colons, "[::1]:22" brackets.
    if (target.startsWith(QLatin1Char('['))) {
        const int close = target.indexOf(QLatin1Char(']'));
        if (close < 0)
            return false;
        const QString rest = target.mid(close + 1);
        if (rest.startsWith(QLatin1Char(':')))
            *port = rest.mid(1).toUShort();
        target = target.mid(1, close - 1);
    } else if (target.count(QLatin1Char(':')) == 1) {
        *port = target.section(QLatin1Char(':'), 1).toUShort();
        target = target.section(QLatin1Char(':'), 0, 0);
    }
    *host = target;
    return !host->isEmpty() && !host->contains(QLatin1Char(' '));
}

JumpChain::JumpChain(QWidget *dialogParent) : QObject(dialogParent), m_dialogParent(dialogParent)
{
}

JumpChain::~JumpChain()
{
    for (auto &hop : m_hops)
        hop.password.fill(QChar(0));
    for (const auto &session : std::as_const(m_sessions)) {
        if (session) {
            session->disconnect(this);
            session->deleteLater();
        }
    }
}

void JumpChain::open(std::vector<SshConnectionSettings> hops, const QString &targetHost,
                     quint16 targetPort, bool singleConnection)
{
    m_singleConnection = singleConnection;
    m_hops = std::move(hops);
    m_targetHost = targetHost;
    m_targetPort = targetPort;
    if (m_hops.empty()) {
        emit failed(QStringLiteral("No jump host."), true);
        return;
    }
    openHop(0, 0);
}

void JumpChain::openHop(size_t index, quint16 viaLocalPort)
{
    auto settings = m_hops[index];
    m_hops[index].password.fill(QChar(0));
    m_hops[index].password.clear();
    const bool last = index + 1 == m_hops.size();
    settings.forwardHost = last ? m_targetHost : m_hops[index + 1].host;
    settings.forwardPort = last ? m_targetPort : m_hops[index + 1].port;
    settings.viaLocalPort = viaLocalPort;
    settings.jumps.clear();
    // The next hop connects once; the target may need more connections.
    settings.forwardOnce = !last || m_singleConnection;
    const QString host = settings.host;

    auto *session = new SshSession(this);
    m_sessions.append(session);
    connect(session, &SshSession::statusMessage, this, [this, host](const QString &message) {
        emit statusMessage(QStringLiteral("Jump host %1: %2").arg(host, message));
    });
    connect(session, &SshSession::hostKeyVerificationRequired, this,
            [this, session, host](SshHostKeyStatus status, const QString &keyType,
                                  const QString &fingerprint) {
                askSshHostKey(m_dialogParent, session, QStringLiteral("jump host %1").arg(host),
                              status, keyType, fingerprint);
            });
    connect(session, &SshSession::inputRequired, this,
            [this, session, host](const QString &instruction, const QStringList &prompts,
                                  const QList<bool> &echo) {
                askSshLogOn(m_dialogParent, session, QStringLiteral("jump host %1").arg(host),
                            instruction, prompts, echo);
            });
    connect(session, &SshSession::forwardReady, this, [this, index, last](quint16 localPort) {
        if (last)
            emit ready(localPort);
        else
            openHop(index + 1, localPort);
    });
    connect(session, &SshSession::disconnected, this,
            [this, host](const QString &reason, bool error) {
                if (std::exchange(m_failed, true))
                    return;
                emit failed(QStringLiteral("Jump host %1: %2").arg(host, reason), error);
            });
    emit statusMessage(QStringLiteral("Connecting to jump host %1…").arg(host));
    session->connectToHost(settings);
    settings.password.fill(QChar(0));
}

void askSshHostKey(QWidget *parent, SshSession *session, const QString &server,
                   SshHostKeyStatus status, const QString &keyType, const QString &fingerprint)
{
    QPointer<SshSession> guard(session);
    QMessageBox box(parent);
    const bool changed = status == SshHostKeyStatus::Changed;
    box.setIcon(changed ? QMessageBox::Warning : QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("SSH Host Key"));
    box.setText(changed ? QStringLiteral("The host key of %1 has CHANGED.").arg(server)
                        : QStringLiteral("The authenticity of %1 can't be established.")
                              .arg(server));
    box.setInformativeText(QStringLiteral("%1 key fingerprint:\n%2\n\nOnly connect if this "
                                          "matches the server's published key.")
                               .arg(keyType, fingerprint));
    auto *trust = box.addButton(QStringLiteral("Trust and Connect"), QMessageBox::AcceptRole);
    auto *once = box.addButton(QStringLiteral("Connect Once"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Disconnect"), QMessageBox::RejectRole);
    box.setDefaultButton(changed ? once : trust);
    box.exec();
    if (!guard)
        return;
    guard->answerHostKey(box.clickedButton() == trust  ? SshHostKeyDecision::AcceptAndSave
                         : box.clickedButton() == once ? SshHostKeyDecision::AcceptOnce
                                                       : SshHostKeyDecision::Reject);
}

void askSshLogOn(QWidget *parent, SshSession *session, const QString &server,
                 const QString &instruction, const QStringList &prompts, const QList<bool> &echo)
{
    QPointer<SshSession> guard(session);
    QStringList answers;
    for (qsizetype i = 0; i < prompts.size(); ++i) {
        bool ok = false;
        const QString answer = QInputDialog::getText(
            parent, QStringLiteral("Log On to %1").arg(server),
            instruction.isEmpty() ? prompts[i] : instruction + QLatin1Char('\n') + prompts[i],
            echo.value(i) ? QLineEdit::Normal : QLineEdit::Password, QString(), &ok);
        if (!guard)
            return;
        if (!ok) {
            guard->cancelPrompts();
            return;
        }
        answers.append(answer);
    }
    guard->answerPrompts(answers);
}

JumpHostEditor::JumpHostEditor(const QString &value, const QString &inherited,
                               const QList<QPair<QString, QString>> &sshSessions,
                               const QString &exclude, QWidget *parent)
    : QWidget(parent)
{
    auto *form = new QFormLayout(this);
    m_choice = new QComboBox(this);
    m_choice->setEditable(true);
    m_choice->addItem(QStringLiteral("Inherit (%1)").arg(inherited), QString());
    m_choice->addItem(QStringLiteral("None (connect directly)"), JumpHost::Direct);
    for (const auto &[id, label] : sshSessions) {
        if (id != exclude)
            m_choice->addItem(label, JumpHost::SessionPrefix + id);
    }
    const int known = m_choice->findData(value);
    if (known >= 0)
        m_choice->setCurrentIndex(known);
    else
        m_choice->setEditText(value);
    form->addRow(QStringLiteral("Jump host:"), m_choice);
    auto *help = new QLabel(QStringLiteral(
        "Connect through an SSH server first: one of your saved SSH sessions (which can have a "
        "jump host of its own, for several hops), or type user@host[:port]. The host of the "
        "session is then as seen from the jump host."), this);
    help->setWordWrap(true);
    help->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(help);
}

void JumpHostEditor::offerNew(NewEntry::SessionMaker make, NewEntry::SessionChoices choices)
{
    NewEntry::add(
        m_choice, std::move(make), std::move(choices), [](const QString &label) { return label; },
        [](const QString &id) { return QVariant(JumpHost::SessionPrefix + id); });
}

QString JumpHostEditor::value() const
{
    // A listed choice stores its value; typed text is stored as written.
    const int index = m_choice->currentIndex();
    return index >= 0 && m_choice->itemText(index) == m_choice->currentText()
               ? m_choice->itemData(index).toString()
               : m_choice->currentText().trimmed();
}
