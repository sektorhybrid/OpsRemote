// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "filespage.h"

#include "jumphost.h"

#include "sftppanel.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int MaximumLogLines = 5000;
}

FilesPage::FilesPage(const FtpConnectionSettings &settings, bool canSavePassword, QWidget *parent)
    : QWidget(parent), m_ftp(true), m_ftpSettings(settings), m_canSavePassword(canSavePassword)
{
    build(settings.username.isEmpty() ? QStringLiteral("anonymous") : settings.username,
          settings.host, true);
}

FilesPage::FilesPage(const SshConnectionSettings &settings, QWidget *parent)
    : QWidget(parent), m_ftp(false), m_sshSettings(settings)
{
    build(settings.username, settings.host, false);
}

void FilesPage::build(const QString &user, const QString &host, bool log)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *bar = new QWidget(this);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(8, 4, 8, 4);
    m_state = new QLabel(bar);
    m_state->setWordWrap(true);
    m_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_reconnect = new QPushButton(QStringLiteral("Reconnect"), bar);
    m_reconnect->setVisible(false);
    connect(m_reconnect, &QPushButton::clicked, this, &FilesPage::start);
    barLayout->addWidget(m_state, 1);
    barLayout->addWidget(m_reconnect);
    layout->addWidget(bar);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    m_panel = new SftpPanel(splitter);
    m_panel->setTitle(user.isEmpty() ? host : QStringLiteral("%1@%2").arg(user, host));
    m_panel->setDisconnectedText(QStringLiteral("Disconnected."));
    splitter->addWidget(m_panel);
    if (log) {
        m_logButton = new QToolButton(bar);
        m_logButton->setText(QStringLiteral("Protocol Log"));
        m_logButton->setCheckable(true);
        m_logButton->setToolTip(QStringLiteral("Shows the commands and the server's answers "
                                               "(passwords hidden)."));
        barLayout->addWidget(m_logButton);
        m_log = new QPlainTextEdit(splitter);
        m_log->setReadOnly(true);
        m_log->setMaximumBlockCount(MaximumLogLines);
        m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_log->setVisible(false);
        splitter->addWidget(m_log);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 1);
        connect(m_logButton, &QToolButton::toggled, m_log, &QWidget::setVisible);
    }
    layout->addWidget(splitter, 1);

    // After the tab is shown, so a prompt has a window to sit on.
    QMetaObject::invokeMethod(this, &FilesPage::start, Qt::QueuedConnection);
}

FilesPage::~FilesPage()
{
    m_ftpSettings.password.fill(QChar(0));
    m_sshSettings.password.fill(QChar(0));
    for (auto &jump : m_sshSettings.jumps)
        jump.password.fill(QChar(0));
    delete m_ftpSession.data();
    delete m_sshSession.data();
}

void FilesPage::refreshIcons()
{
    m_panel->refreshIcons();
}

void FilesPage::setState(const QString &text, bool error)
{
    m_state->setText(text);
    m_state->setStyleSheet(error ? QStringLiteral("color: #c62828;") : QString());
}

void FilesPage::ended(const QString &reason, bool error)
{
    setState(reason, error);
    m_reconnect->setVisible(true);
    m_panel->setSession(nullptr);
}

void FilesPage::start()
{
    m_reconnect->setVisible(false);
    if (m_ftp)
        startFtp();
    else
        startSsh();
}

void FilesPage::startSsh()
{
    if (m_sshSession) {
        m_panel->setSession(nullptr);
        m_sshSession->disconnect(this);
        m_sshSession->deleteLater();
    }
    if (m_jumpChain)
        m_jumpChain->deleteLater();
    const QString server = m_sshSettings.port == 22
        ? m_sshSettings.host
        : QStringLiteral("%1:%2").arg(m_sshSettings.host).arg(m_sshSettings.port);
    m_sshSession = new SshSession(this);
    setState(QStringLiteral("Connecting to %1…").arg(server), false);
    connect(m_sshSession, &SshSession::statusMessage, this, [this](const QString &message) {
        setState(message, false);
    });
    connect(m_sshSession, &SshSession::hostKeyVerificationRequired, this,
            [this, server](SshHostKeyStatus status, const QString &type, const QString &fingerprint) {
                askSshHostKey(this, m_sshSession, server, status, type, fingerprint);
            });
    connect(m_sshSession, &SshSession::inputRequired, this,
            [this, server](const QString &instruction, const QStringList &prompts,
                           const QList<bool> &echo) {
                askSshLogOn(this, m_sshSession, server, instruction, prompts, echo);
            });
    connect(m_sshSession, &SshSession::sftpReady, this, [this, server] {
        setState(QStringLiteral("Connected to %1 (files over %2).")
                     .arg(server, m_sshSession ? m_sshSession->fileProtocol() : QString()), false);
        emit statusMessage(QStringLiteral("Connected to %1").arg(server));
    });
    connect(m_sshSession, &SshSession::sftpUnavailable, this, [this](const QString &error) {
        setState(error, true);
        m_reconnect->setVisible(true);
    });
    connect(m_sshSession, &SshSession::disconnected, this, &FilesPage::ended);
    m_panel->setSession(m_sshSession);
    if (m_sshSettings.jumps.empty()) {
        m_sshSession->connectToHost(m_sshSettings);
        return;
    }
    // Through jump hosts: the chain's local port stands in for the server.
    m_jumpChain = new JumpChain(this);
    connect(m_jumpChain, &JumpChain::statusMessage, this, [this](const QString &message) {
        setState(message, false);
    });
    connect(m_jumpChain, &JumpChain::ready, this, [this](quint16 localPort) {
        if (!m_sshSession)
            return;
        auto settings = m_sshSettings;
        settings.jumps.clear();
        settings.viaLocalPort = localPort;
        m_sshSession->connectToHost(settings);
        settings.password.fill(QChar(0));
    });
    connect(m_jumpChain, &JumpChain::failed, this, [this](const QString &reason, bool error) {
        if (m_sshSession)
            m_sshSession->disconnect(this);
        ended(reason, error);
    });
    m_jumpChain->open(m_sshSettings.jumps, m_sshSettings.host, m_sshSettings.port);
}

void FilesPage::startFtp()
{
    if (!m_ftpSettings.username.isEmpty() && m_ftpSettings.password.isEmpty()
        && !askPassword(QString())) {
        ended(QStringLiteral("Not connected: no password given."), true);
        return;
    }
    if (m_ftpSession) {
        m_panel->setSession(nullptr);
        m_ftpSession->disconnect(this);
        m_ftpSession->deleteLater();
    }
    m_ftpSession = new FtpSession(this);
    setState(QStringLiteral("Connecting to %1…").arg(m_ftpSettings.host), false);
    connect(m_ftpSession, &FtpSession::logLine, this, [this](const QString &line) {
        m_log->appendPlainText(line);
    });
    connect(m_ftpSession, &FtpSession::statusMessage, this, [this](const QString &message) {
        setState(message, false);
        emit statusMessage(message);
    });
    connect(m_ftpSession, &FtpSession::connected, this, [this](const QString &protocol) {
        // An earlier status message says when only the log-on is encrypted.
        if (protocol == QStringLiteral("FTPS") && m_state->text().startsWith(QStringLiteral("Connecting")))
            setState(QStringLiteral("Connected to %1 over FTPS (encrypted).").arg(m_ftpSettings.host), false);
        else if (protocol != QStringLiteral("FTPS"))
            setState(QStringLiteral("Connected to %1 over plain FTP: passwords and files travel "
                                    "unencrypted.").arg(m_ftpSettings.host), true);
        emit statusMessage(QStringLiteral("Connected to %1").arg(m_ftpSettings.host));
    });
    connect(m_ftpSession, &FtpSession::disconnected, this, &FilesPage::ended);
    connect(m_ftpSession, &FtpSession::loginFailed, this, [this](const QString &message) {
        if (askPassword(message))
            m_ftpSession->retryLogin(m_ftpSettings.password);
        else
            ended(message, true);
    });
    connect(m_ftpSession, &FtpSession::certificateQuestion, this, &FilesPage::askCertificate);
    connect(m_ftpSession, &FtpSession::plainTextQuestion, this, [this](const QString &message) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral("No Encryption"), message,
                        QMessageBox::NoButton, this);
        box.setInformativeText(QStringLiteral("Anyone on the network path can read the password and "
                                              "the files. Choose \"Require TLS\" in the session's "
                                              "settings to never connect this way."));
        auto *once = box.addButton(QStringLiteral("Connect This Time"), QMessageBox::AcceptRole);
        auto *always = box.addButton(QStringLiteral("Always for This Server"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (m_ftpSession)
            m_ftpSession->answerPlainText(box.clickedButton() == once || box.clickedButton() == always,
                                          box.clickedButton() == always);
    });
    m_panel->setSession(m_ftpSession);
    m_ftpSession->connectToHost(m_ftpSettings);
}

bool FilesPage::askPassword(const QString &problem)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("FTP Log-on"));
    auto *form = new QFormLayout(&dialog);
    if (!problem.isEmpty()) {
        auto *label = new QLabel(problem, &dialog);
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color: #c62828;"));
        form->addRow(label);
    }
    form->addRow(new QLabel(QStringLiteral("Password for %1 on %2:")
                                .arg(m_ftpSettings.username, m_ftpSettings.host), &dialog));
    auto *password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    form->addRow(password);
    QCheckBox *save = nullptr;
    if (m_canSavePassword) {
        save = new QCheckBox(QStringLiteral("Remember the password"), &dialog);
        form->addRow(save);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    m_ftpSettings.password = password->text();
    if (save && save->isChecked())
        emit passwordSaveRequested(m_ftpSettings.password);
    return true;
}

void FilesPage::askCertificate(const QString &problems, const QString &fingerprint,
                             const QString &details, bool changed)
{
    QMessageBox box(changed ? QMessageBox::Warning : QMessageBox::Question,
                    QStringLiteral("Server Certificate"),
                    changed ? QStringLiteral("The certificate of %1 has changed since it was last "
                                             "accepted. This can mean that someone is intercepting "
                                             "the connection.").arg(m_ftpSettings.host)
                            : QStringLiteral("The certificate of %1 is not trusted.").arg(m_ftpSettings.host),
                    QMessageBox::NoButton, this);
    box.setInformativeText(QStringLiteral("%1\n\n%2\n\nSHA-256: %3").arg(problems, details, fingerprint));
    auto *once = box.addButton(QStringLiteral("Accept Once"), QMessageBox::AcceptRole);
    auto *always = box.addButton(QStringLiteral("Accept Always"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (!m_ftpSession)
        return;
    if (box.clickedButton() == once)
        m_ftpSession->answerCertificate(FtpCertificateDecision::AcceptOnce);
    else if (box.clickedButton() == always)
        m_ftpSession->answerCertificate(FtpCertificateDecision::AcceptAndSave);
    else
        m_ftpSession->answerCertificate(FtpCertificateDecision::Reject);
}
