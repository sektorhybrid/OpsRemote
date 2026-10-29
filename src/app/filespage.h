// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "ftpclient.h"
#include "sshsession.h"

#include <QPointer>
#include <QWidget>

class JumpChain;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class SftpPanel;

// A file session tab without a terminal: FTP/FTPS, or SFTP or SCP over SSH.
// The file browser (the same as for SSH terminals) fills it, with the
// connection's state and a Reconnect button; FTP adds the protocol log.
class FilesPage final : public QWidget
{
    Q_OBJECT

public:
    // canSavePassword: offer to keep a typed password (in the vault or the
    // system store).
    FilesPage(const FtpConnectionSettings &settings, bool canSavePassword, QWidget *parent = nullptr);
    // settings.fileTransfer chooses SFTP or SCP; no shell is opened.
    FilesPage(const SshConnectionSettings &settings, QWidget *parent = nullptr);
    ~FilesPage() override;

    QString host() const { return m_ftp ? m_ftpSettings.host : m_sshSettings.host; }
    SftpPanel *panel() const { return m_panel; }
    void refreshIcons();

signals:
    void statusMessage(const QString &message);
    void passwordSaveRequested(const QString &password);

private:
    void build(const QString &user, const QString &host, bool log);
    void start();
    void startFtp();
    void startSsh();
    void ended(const QString &reason, bool error);
    // Asks for the FTP password; false if cancelled.
    bool askPassword(const QString &problem);
    void askCertificate(const QString &problems, const QString &fingerprint, const QString &details,
                        bool changed);
    void setState(const QString &text, bool error);

    bool m_ftp = true;
    FtpConnectionSettings m_ftpSettings;
    SshConnectionSettings m_sshSettings;
    bool m_canSavePassword = false;
    QPointer<FtpSession> m_ftpSession;
    QPointer<SshSession> m_sshSession;
    QPointer<JumpChain> m_jumpChain;
    SftpPanel *m_panel = nullptr;
    QLabel *m_state = nullptr;
    QPushButton *m_reconnect = nullptr;
    QToolButton *m_logButton = nullptr;
    QPlainTextEdit *m_log = nullptr;
};
