// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QWidget>

class IperfTest;
class QCheckBox;
class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class FtpSession;
class QUrl;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTcpSocket;
class QTimer;
class QTreeWidget;

// Tools → Network Tools: ping and traceroute (the system's own commands, so
// no administrator rights are needed), a TCP port scanner, and a bandwidth
// test that speaks iperf3 (client or server, no iperf3 program needed).
class NetworkToolsWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit NetworkToolsWindow(QWidget *parent);
    ~NetworkToolsWindow() override;

    // Puts a host in every tool's host field (e.g. the selected session's).
    void setHost(const QString &host);
    // Shows the port scanner with this host.
    void showPortScan(const QString &host);

private:
    QWidget *buildCommandTab(QLineEdit **host, QPlainTextEdit **output, QPushButton **start,
                             QWidget *options);
    void runCommand(QPointer<QProcess> &process, QPlainTextEdit *output, QPushButton *start,
                    const QString &program, const QStringList &arguments);
    void startPing();
    void startTraceroute();
    void startScan();
    void stopScan(const QString &message);
    void scanNext();
    void finishPort(QTcpSocket *socket, bool open);
    QWidget *buildBandwidthTab();
    void toggleBandwidth();
    QWidget *buildDownloadTab();
    void toggleDownload();
    void finishDownload(const QString &message);
    void startFtpDownload(const QUrl &url, const QString &folder);

    QTabWidget *m_tabs = nullptr;
    class HttpRequestPage *m_http = nullptr;

    QLineEdit *m_pingHost = nullptr;
    QSpinBox *m_pingCount = nullptr;
    QPlainTextEdit *m_pingOutput = nullptr;
    QPushButton *m_pingStart = nullptr;
    QPointer<QProcess> m_ping;

    QLineEdit *m_traceHost = nullptr;
    QPlainTextEdit *m_traceOutput = nullptr;
    QPushButton *m_traceStart = nullptr;
    QPointer<QProcess> m_trace;

    QLineEdit *m_scanHost = nullptr;
    QLineEdit *m_scanPorts = nullptr;
    QSpinBox *m_scanTimeout = nullptr;
    QPushButton *m_scanStart = nullptr;
    QProgressBar *m_scanProgress = nullptr;
    QLabel *m_scanStatus = nullptr;
    QTreeWidget *m_scanResults = nullptr;
    QHostAddress m_scanAddress;
    QList<quint16> m_scanQueue;
    qsizetype m_scanTotal = 0;
    int m_scanOpen = 0;
    QList<QTcpSocket *> m_scanSockets;
    QElapsedTimer m_scanTimer;
    bool m_scanning = false;

    QComboBox *m_bandwidthMode = nullptr;
    QLineEdit *m_bandwidthHost = nullptr;
    QSpinBox *m_bandwidthPort = nullptr;
    QSpinBox *m_bandwidthSeconds = nullptr;
    QSpinBox *m_bandwidthStreams = nullptr;
    QComboBox *m_bandwidthDirection = nullptr;
    QPushButton *m_bandwidthStart = nullptr;
    QComboBox *m_bandwidthListen = nullptr;
    QPlainTextEdit *m_bandwidthOutput = nullptr;
    QWidget *m_bandwidthGraph = nullptr;
    QPointer<IperfTest> m_bandwidth;

    QLineEdit *m_downloadUrl = nullptr;
    QComboBox *m_downloadFolder = nullptr;
    QLineEdit *m_downloadCustom = nullptr;
    QCheckBox *m_downloadResume = nullptr;
    QCheckBox *m_downloadInsecure = nullptr;
    QPushButton *m_downloadStart = nullptr;
    QProgressBar *m_downloadProgress = nullptr;
    QLabel *m_downloadStatus = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_download;
    QPointer<FtpSession> m_ftpDownload;
    QFile *m_downloadFile = nullptr;
    QElapsedTimer m_downloadClock;
};
