// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "preferences.h"

#include <QDialog>
#include <QHash>
#include <QHostAddress>
#include <QPointer>

class QFile;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTcpServer;
class QTcpSocket;
class QUdpSocket;

// The folder a server shares: the package's shared/ folder, or one chosen.
QString localServerFolder(const LocalServerSettings &settings);
// A path inside root for a requested name, or empty when it would leave root
// (also through a link). forWriting also refuses a link or a folder as the
// file itself. Another program changing the folder between this check and
// the write is not guarded against: the shared folder is the user's own.
QString localServerPath(const QString &root, const QString &requested, bool forWriting = false);

// A TFTP server (RFC 1350) with the block size and transfer size options
// (RFC 2347, 2348, 2349), for firmware and configuration files of network
// equipment. Each transfer has a socket of its own, as TFTP wants.
class TftpServer final : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~TftpServer() override;

    bool start(const LocalServerSettings &settings, QString *error);
    void stop();
    bool isRunning() const;

signals:
    void logged(const QString &line);

private:
    struct Transfer;
    void readRequests();
    void startTransfer(const QHostAddress &peer, quint16 port, bool upload, const QString &name,
                       const QHash<QString, QString> &options);
    void sendBlock(Transfer *transfer);
    void handleTransferPacket(Transfer *transfer);
    void finish(Transfer *transfer, const QString &result);

    QUdpSocket *m_socket = nullptr;
    QString m_root;
    bool m_uploads = false;
    qint64 m_maxUpload = 0; // bytes
    QList<Transfer *> m_transfers;
};

// A small HTTP server: files, folder listings, and (when allowed) uploads
// with a browser form (multipart/form-data) or PUT (curl -T).
class HttpFileServer final : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~HttpFileServer() override;

    bool start(const LocalServerSettings &settings, QString *error);
    void stop();
    bool isRunning() const;

signals:
    void logged(const QString &line);

private:
    struct Client;
    void accept();
    void readClient(Client *client);
    void respond(Client *client);
    void sendFile(Client *client, const QString &path);
    void sendListing(Client *client, const QString &path, const QString &urlPath);
    void sendSimple(Client *client, int status, const QString &text);
    void finishUpload(Client *client);
    void drop(Client *client);

    QTcpServer *m_server = nullptr;
    QString m_root;
    bool m_uploads = false;
    qint64 m_maxUpload = 0; // bytes
    QList<Client *> m_clients;
};

// Tools → Local Servers: starts and stops both servers, with their settings
// and a log. The servers run only while it is open.
class LocalServersWindow final : public QDialog
{
    Q_OBJECT

public:
    explicit LocalServersWindow(QWidget *parent = nullptr);

    TftpServer *tftp() const { return m_tftp; }
    HttpFileServer *http() const { return m_http; }

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    QWidget *buildCard(bool tftp);
    void toggle(bool tftp);
    void refresh();

    TftpServer *m_tftp = nullptr;
    HttpFileServer *m_http = nullptr;
    QPushButton *m_tftpButton = nullptr;
    QPushButton *m_httpButton = nullptr;
    QLabel *m_tftpStatus = nullptr;
    QLabel *m_httpStatus = nullptr;
    QPlainTextEdit *m_log = nullptr;
};
