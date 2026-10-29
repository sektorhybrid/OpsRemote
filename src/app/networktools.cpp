// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "httprequestpage.h"
#include "networktools.h"
#include "ftpclient.h"
#include "ftpoptions.h"
#include "appdirs.h"
#include "availabilitynote.h"
#include "iperf.h"
#include "listenaddress.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTcpSocket>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int ParallelConnections = 64;
constexpr qsizetype MaximumPorts = 65535;
constexpr int MaximumOutputLines = 10000;

// Well-known TCP services, for the scan results.
QString serviceName(quint16 port)
{
    static const QHash<quint16, const char *> names{
        {21, "FTP"}, {22, "SSH"}, {23, "Telnet"}, {25, "SMTP"}, {53, "DNS"}, {69, "TFTP"},
        {80, "HTTP"}, {88, "Kerberos"}, {110, "POP3"}, {111, "RPC"}, {135, "MS RPC"},
        {139, "NetBIOS"}, {143, "IMAP"}, {161, "SNMP"}, {389, "LDAP"}, {443, "HTTPS"},
        {445, "SMB"}, {465, "SMTPS"}, {514, "Syslog"}, {587, "SMTP submission"},
        {636, "LDAPS"}, {873, "rsync"}, {993, "IMAPS"}, {995, "POP3S"}, {1433, "MS SQL"},
        {1521, "Oracle"}, {2049, "NFS"}, {2222, "SSH (alt)"}, {3000, "HTTP (dev)"},
        {3306, "MySQL"}, {3389, "RDP"}, {5000, "HTTP (dev)"}, {5432, "PostgreSQL"},
        {5900, "VNC"}, {5901, "VNC :1"}, {5902, "VNC :2"}, {5985, "WinRM"}, {5986, "WinRM (TLS)"},
        {6379, "Redis"}, {8000, "HTTP (alt)"}, {8080, "HTTP (alt)"}, {8443, "HTTPS (alt)"},
        {9000, "HTTP (alt)"}, {9090, "Web console"}, {9200, "Elasticsearch"}, {11211, "Memcached"},
        {27017, "MongoDB"}};
    const auto name = names.value(port);
    return name ? QString::fromLatin1(name) : QString();
}

// "22,80,443,8000-8100" → the ports, in order, without duplicates; empty on error.
QList<quint16> parsePorts(const QString &text, QString *error)
{
    QList<quint16> ports;
    QSet<quint16> seen;
    for (const auto &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const auto range = part.trimmed().split(QLatin1Char('-'));
        bool okFirst = false, okLast = true;
        const int first = range.value(0).trimmed().toInt(&okFirst);
        const int last = range.size() == 2 ? range.value(1).trimmed().toInt(&okLast) : first;
        if (range.size() > 2 || !okFirst || !okLast || first < 1 || last > 65535 || first > last) {
            *error = QStringLiteral("\"%1\" is not a port or range (1–65535).").arg(part.trimmed());
            return {};
        }
        for (int port = first; port <= last && ports.size() < MaximumPorts; ++port) {
            if (!seen.contains(quint16(port))) {
                seen.insert(quint16(port));
                ports.append(quint16(port));
            }
        }
    }
    if (ports.isEmpty())
        *error = QStringLiteral("Enter ports, for example 22,80,443 or 1-1024.");
    return ports;
}

// The last minute of a bandwidth test, as bars.
class BandwidthGraph final : public QWidget
{
public:
    explicit BandwidthGraph(QWidget *parent) : QWidget(parent) { setMinimumHeight(90); }
    void add(double bitsPerSecond)
    {
        m_values.append(bitsPerSecond);
        if (m_values.size() > 60)
            m_values.removeFirst();
        update();
    }
    void clear()
    {
        m_values.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().base());
        double top = 0;
        for (const double value : std::as_const(m_values))
            top = qMax(top, value);
        if (top <= 0)
            return;
        const double width = double(rect().width()) / 60;
        for (int i = 0; i < m_values.size(); ++i) {
            const double height = (rect().height() - 16) * m_values.at(i) / top;
            painter.fillRect(QRectF(i * width + 1, rect().height() - height, width - 2, height),
                             palette().highlight());
        }
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(rect().adjusted(4, 0, 0, 0), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("peak %1 Mbit/s").arg(top / 1e6, 0, 'f', 0));
    }

private:
    QList<double> m_values;
};

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

}

NetworkToolsWindow::NetworkToolsWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(QStringLiteral("Network Tools"));
    resize(1000, 720);
    auto *layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs);

    // Ping
    auto *pingOptions = new QWidget;
    auto *pingForm = new QHBoxLayout(pingOptions);
    pingForm->setContentsMargins(0, 0, 0, 0);
    m_pingCount = new QSpinBox(pingOptions);
    m_pingCount->setRange(1, 1000);
    m_pingCount->setValue(4);
    m_pingCount->setSuffix(QStringLiteral(" times"));
    pingForm->addWidget(new QLabel(QStringLiteral("Send"), pingOptions));
    pingForm->addWidget(m_pingCount);
    pingForm->addStretch();
    m_tabs->addTab(buildCommandTab(&m_pingHost, &m_pingOutput, &m_pingStart, pingOptions),
                   QStringLiteral("Ping"));
    connect(m_pingStart, &QPushButton::clicked, this, &NetworkToolsWindow::startPing);
    connect(m_pingHost, &QLineEdit::returnPressed, m_pingStart, &QPushButton::click);

    // Traceroute
    m_tabs->addTab(buildCommandTab(&m_traceHost, &m_traceOutput, &m_traceStart, nullptr),
                   QStringLiteral("Traceroute"));
    connect(m_traceStart, &QPushButton::clicked, this, &NetworkToolsWindow::startTraceroute);
    connect(m_traceHost, &QLineEdit::returnPressed, m_traceStart, &QPushButton::click);

    // Port scan
    auto *scanPage = new QWidget;
    auto *scanLayout = new QVBoxLayout(scanPage);
    auto *scanForm = new QFormLayout;
    m_scanHost = new QLineEdit(scanPage);
    m_scanHost->setPlaceholderText(QStringLiteral("host name or IP address"));
    m_scanPorts = new QLineEdit(QStringLiteral("21,22,23,25,53,80,110,135,139,143,443,445,993,995,"
                                               "1433,3306,3389,5432,5900-5902,5985,6379,8080,8443"),
                                scanPage);
    m_scanTimeout = new QSpinBox(scanPage);
    m_scanTimeout->setRange(100, 30000);
    m_scanTimeout->setSingleStep(100);
    m_scanTimeout->setValue(1500);
    m_scanTimeout->setSuffix(QStringLiteral(" ms"));
    scanForm->addRow(QStringLiteral("Host:"), m_scanHost);
    scanForm->addRow(QStringLiteral("Ports:"), m_scanPorts);
    scanForm->addRow(QStringLiteral("Wait per port:"), m_scanTimeout);
    scanLayout->addLayout(scanForm);
    auto *scanRow = new QHBoxLayout;
    m_scanStart = new QPushButton(QStringLiteral("Scan"), scanPage);
    m_scanProgress = new QProgressBar(scanPage);
    m_scanProgress->setTextVisible(false);
    m_scanProgress->setMaximumHeight(10);
    m_scanStatus = new QLabel(scanPage);
    scanRow->addWidget(m_scanStart);
    scanRow->addWidget(m_scanProgress, 1);
    scanLayout->addLayout(scanRow);
    scanLayout->addWidget(m_scanStatus);
    m_scanResults = new QTreeWidget(scanPage);
    m_scanResults->setRootIsDecorated(false);
    m_scanResults->setHeaderLabels({QStringLiteral("Port"), QStringLiteral("State"),
                                    QStringLiteral("Usually")});
    m_scanResults->setSortingEnabled(true);
    m_scanResults->sortByColumn(0, Qt::AscendingOrder);
    scanLayout->addWidget(m_scanResults, 1);
    scanLayout->addWidget(hint(QStringLiteral(
        "Only open ports are listed (a TCP connection could be made). Scan only computers you are "
        "allowed to: on other networks a port scan can count as an attack."), scanPage));
    m_tabs->addTab(scanPage, QStringLiteral("Port scan"));
    connect(m_scanStart, &QPushButton::clicked, this, [this] {
        if (m_scanning)
            stopScan(QStringLiteral("Stopped."));
        else
            startScan();
    });
    connect(m_scanHost, &QLineEdit::returnPressed, m_scanStart, &QPushButton::click);

    m_tabs->addTab(buildBandwidthTab(), QStringLiteral("Bandwidth (iperf3)"));
    m_tabs->addTab(buildDownloadTab(), QStringLiteral("Download"));
    m_http = new HttpRequestPage(m_tabs);
    m_tabs->addTab(m_http, QStringLiteral("HTTP Request"));
}

QWidget *NetworkToolsWindow::buildDownloadTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;
    m_downloadUrl = new QLineEdit(page);
    m_downloadUrl->setPlaceholderText(QStringLiteral("https://… or http://…"));
    m_downloadFolder = new QComboBox(page);
    m_downloadFolder->addItem(QStringLiteral("My Downloads folder"), QStringLiteral("downloads"));
    m_downloadFolder->addItem(QStringLiteral("The package's shared folder (Local Servers)"), QStringLiteral("shared"));
    m_downloadFolder->addItem(QStringLiteral("Another folder"), QStringLiteral("custom"));
    m_downloadCustom = new QLineEdit(page);
    auto *browse = new QPushButton(QStringLiteral("Browse…"), page);
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(m_downloadFolder);
    folderRow->addWidget(m_downloadCustom, 1);
    folderRow->addWidget(browse);
    m_downloadResume = new QCheckBox(QStringLiteral("Resume a partly downloaded file"), page);
    m_downloadResume->setChecked(true);
    m_downloadInsecure = new QCheckBox(QStringLiteral("Ignore certificate errors (lab equipment with "
                                                      "self-signed certificates only)"), page);
    form->addRow(QStringLiteral("URL:"), m_downloadUrl);
    // HTTPS needs a TLS library (bundled); say so if it cannot be loaded.
    form->addRow(QString(), new AvailabilityNote(
                                [] {
                                    return QSslSocket::supportsSsl()
                                               ? QString()
                                               : QStringLiteral("HTTPS is not available: no TLS "
                                                                "library could be loaded. http:// "
                                                                "still works.");
                                },
                                {}, page));
    form->addRow(QStringLiteral("Save to:"), folderRow);
    form->addRow(QString(), m_downloadResume);
    form->addRow(QString(), m_downloadInsecure);
    layout->addLayout(form);
    auto *row = new QHBoxLayout;
    m_downloadStart = new QPushButton(QStringLiteral("Download"), page);
    m_downloadProgress = new QProgressBar(page);
    row->addWidget(m_downloadStart);
    row->addWidget(m_downloadProgress, 1);
    layout->addLayout(row);
    m_downloadStatus = new QLabel(page);
    m_downloadStatus->setWordWrap(true);
    m_downloadStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_downloadStatus);
    layout->addStretch();
    const auto updateFolder = [this, browse] {
        const bool custom = m_downloadFolder->currentData().toString() == QStringLiteral("custom");
        m_downloadCustom->setEnabled(custom);
        browse->setEnabled(custom);
    };
    connect(m_downloadFolder, &QComboBox::currentIndexChanged, page, updateFolder);
    updateFolder();
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Save Downloads In"),
                                                                 m_downloadCustom->text());
        if (!folder.isEmpty())
            m_downloadCustom->setText(folder);
    });
    connect(m_downloadStart, &QPushButton::clicked, this, &NetworkToolsWindow::toggleDownload);
    connect(m_downloadUrl, &QLineEdit::returnPressed, m_downloadStart, &QPushButton::click);
    return page;
}

void NetworkToolsWindow::showPortScan(const QString &host)
{
    setHost(host);
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (m_tabs->tabText(index) == QStringLiteral("Port scan"))
            m_tabs->setCurrentIndex(index);
    }
    show();
    raise();
    activateWindow();
}

void NetworkToolsWindow::toggleDownload()
{
    if (m_download) {
        m_download->abort();
        return;
    }
    if (m_ftpDownload) {
        m_ftpDownload->cancelTransfers();
        finishDownload(QStringLiteral("Stopped."));
        return;
    }
    const QUrl url = QUrl::fromUserInput(m_downloadUrl->text().trimmed());
    const QString scheme = url.scheme().toLower();
    const bool ftp = scheme == QStringLiteral("ftp") || scheme == QStringLiteral("ftps")
                     || scheme == QStringLiteral("ftpes");
    if (!url.isValid() || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https") && !ftp)) {
        m_downloadStatus->setText(QStringLiteral("Enter an http://, https://, ftp://, ftps:// (implicit "
                                                 "TLS) or ftpes:// (explicit TLS) address."));
        return;
    }
    QString folder;
    const QString choice = m_downloadFolder->currentData().toString();
    if (choice == QStringLiteral("shared"))
        folder = portableFilePath(QStringLiteral("shared"));
    else if (choice == QStringLiteral("custom"))
        folder = m_downloadCustom->text().trimmed();
    else
        folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (folder.isEmpty() || !QDir().mkpath(folder)) {
        m_downloadStatus->setText(QStringLiteral("Choose a folder to save in."));
        return;
    }
    if (ftp) {
        startFtpDownload(url, folder);
        return;
    }
    QString name = QFileInfo(url.path()).fileName();
    if (name.isEmpty())
        name = QStringLiteral("download");
    m_downloadFile = new QFile(QDir(folder).filePath(name), this);
    const bool resume = m_downloadResume->isChecked() && m_downloadFile->exists() && m_downloadFile->size() > 0;
    if (!m_downloadFile->open(resume ? QIODevice::Append : QIODevice::WriteOnly)) {
        m_downloadStatus->setText(QStringLiteral("Cannot write %1: %2")
                                      .arg(QDir::toNativeSeparators(m_downloadFile->fileName()),
                                           m_downloadFile->errorString()));
        delete m_downloadFile;
        m_downloadFile = nullptr;
        return;
    }
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OpsRemote"));
    const qint64 already = resume ? m_downloadFile->size() : 0;
    if (already > 0)
        request.setRawHeader("Range", "bytes=" + QByteArray::number(already) + '-');
    m_download = m_network->get(request);
    m_downloadClock.start();
    m_downloadStart->setText(QStringLiteral("Stop"));
    m_downloadProgress->setValue(0);
    m_downloadStatus->setText(QStringLiteral("Downloading to %1…").arg(QDir::toNativeSeparators(m_downloadFile->fileName())));
    if (m_downloadInsecure->isChecked())
        connect(m_download, &QNetworkReply::sslErrors, m_download, [reply = m_download](const QList<QSslError> &) {
            reply->ignoreSslErrors();
        });
    connect(m_download, &QNetworkReply::metaDataChanged, this, [this, already] {
        // A server that ignores the range sends the whole file again.
        const int status = m_download->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (already > 0 && status == 200) {
            m_downloadFile->resize(0);
            m_downloadFile->seek(0);
        }
    });
    connect(m_download, &QNetworkReply::readyRead, this, [this] {
        m_downloadFile->write(m_download->readAll());
    });
    connect(m_download, &QNetworkReply::downloadProgress, this, [this, already](qint64 received, qint64 total) {
        const double seconds = qMax(0.001, m_downloadClock.elapsed() / 1000.0);
        if (total > 0) {
            m_downloadProgress->setMaximum(1000);
            m_downloadProgress->setValue(int(1000 * (already + received) / qMax<qint64>(1, already + total)));
        } else {
            m_downloadProgress->setMaximum(0); // unknown size
        }
        m_downloadStatus->setText(QStringLiteral("%1 MB at %2 MB/s")
                                      .arg((already + received) / 1048576.0, 0, 'f', 1)
                                      .arg(received / 1048576.0 / seconds, 0, 'f', 1));
    });
    connect(m_download, &QNetworkReply::finished, this, [this] {
        m_downloadFile->write(m_download->readAll());
        const auto error = m_download->error();
        const int status = m_download->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (error == QNetworkReply::OperationCanceledError)
            finishDownload(QStringLiteral("Stopped; the part downloaded is kept for resuming."));
        else if (error != QNetworkReply::NoError && status != 416)
            finishDownload(QStringLiteral("Failed: %1").arg(m_download->errorString()));
        else
            finishDownload(QStringLiteral("Saved %1 (%2 MB).")
                               .arg(QDir::toNativeSeparators(m_downloadFile->fileName()))
                               .arg(m_downloadFile->size() / 1048576.0, 0, 'f', 1));
    });
}

// FTP addresses: the same client as FTP sessions, for one file. An existing
// file is replaced (FTP downloads do not resume).
void NetworkToolsWindow::startFtpDownload(const QUrl &url, const QString &folder)
{
    const QString path = url.path(QUrl::FullyDecoded);
    if (path.isEmpty() || path.endsWith(QLatin1Char('/'))) {
        m_downloadStatus->setText(QStringLiteral("The address needs a file, as in ftp://server/folder/file."));
        return;
    }
    const auto defaults = loadFtpDefaults();
    FtpConnectionSettings settings;
    settings.host = url.host();
    const QString scheme = url.scheme().toLower();
    settings.security = scheme == QStringLiteral("ftps")    ? QStringLiteral("implicit")
                        : scheme == QStringLiteral("ftpes") ? QStringLiteral("explicit")
                                                            : defaults.security;
    settings.port = quint16(url.port(settings.security == QStringLiteral("implicit") ? 990 : 21));
    settings.username = url.userName(QUrl::FullyDecoded);
    settings.password = url.password(QUrl::FullyDecoded);
    settings.passive = defaults.mode != QStringLiteral("active");
    settings.listing = defaults.listing;
    settings.connectTimeout = defaults.connectTimeout;
    settings.keepaliveSeconds = 0;
    settings.knownCertificatesPath = portableFilePath(QStringLiteral("settings/ftp_certificates"));
    if (!settings.username.isEmpty() && settings.password.isEmpty()) {
        bool ok = false;
        settings.password = QInputDialog::getText(this, QStringLiteral("FTP Log-on"),
                                                  QStringLiteral("Password for %1 on %2:")
                                                      .arg(settings.username, settings.host),
                                                  QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
    }
    auto *session = new FtpSession(this);
    m_ftpDownload = session;
    m_downloadClock.start();
    m_downloadStart->setText(QStringLiteral("Stop"));
    m_downloadProgress->setMaximum(1000);
    m_downloadProgress->setValue(0);
    m_downloadStatus->setText(QStringLiteral("Connecting to %1…").arg(settings.host));
    const bool insecure = m_downloadInsecure->isChecked();
    connect(session, &FtpSession::certificateQuestion, this,
            [this, session, insecure](const QString &problems, const QString &fingerprint,
                                      const QString &details, bool) {
                if (insecure) {
                    session->answerCertificate(FtpCertificateDecision::AcceptOnce);
                    return;
                }
                const auto answer = QMessageBox::question(
                    this, QStringLiteral("Server Certificate"),
                    QStringLiteral("The server's certificate is not trusted:\n%1\n\n%2\n\nSHA-256: %3\n\n"
                                   "Download anyway?").arg(problems, details, fingerprint));
                session->answerCertificate(answer == QMessageBox::Yes ? FtpCertificateDecision::AcceptOnce
                                                                      : FtpCertificateDecision::Reject);
            });
    connect(session, &FtpSession::plainTextQuestion, this, [this, session](const QString &message) {
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("No Encryption"),
            QStringLiteral("%1\n\nDownload anyway?").arg(message), QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        session->answerPlainText(answer == QMessageBox::Yes, false);
    });
    connect(session, &FtpSession::loginFailed, this, [this](const QString &message) {
        finishDownload(QStringLiteral("Failed: %1").arg(message));
    });
    connect(session, &FtpSession::disconnected, this, [this](const QString &reason, bool) {
        finishDownload(QStringLiteral("Failed: %1").arg(reason));
    });
    connect(session, &RemoteFileSession::sftpReady, this, [this, session, path, folder] {
        m_downloadStatus->setText(QStringLiteral("Downloading %1…").arg(path));
        session->download({path}, folder, SftpConflictResolution::Overwrite);
    });
    connect(session, &RemoteFileSession::transferProgress, this,
            [this](int, const QString &, quint64 done, quint64 total) {
                const double seconds = qMax(0.001, m_downloadClock.elapsed() / 1000.0);
                m_downloadProgress->setValue(total > 0 ? int(1000 * done / total) : 0);
                m_downloadStatus->setText(QStringLiteral("%1 MB at %2 MB/s")
                                              .arg(done / 1048576.0, 0, 'f', 1)
                                              .arg(done / 1048576.0 / seconds, 0, 'f', 1));
            });
    connect(session, &RemoteFileSession::transferFinished, this,
            [this, path, folder](int, bool, const QString &, int files, int, const QStringList &errors,
                                 bool cancelled) {
                const QString saved = QDir(folder).filePath(QFileInfo(path).fileName());
                if (cancelled)
                    finishDownload(QStringLiteral("Stopped."));
                else if (!errors.isEmpty() || files == 0)
                    finishDownload(QStringLiteral("Failed: %1").arg(errors.isEmpty()
                                                                         ? QStringLiteral("nothing was downloaded")
                                                                         : errors.join(QLatin1Char('\n'))));
                else
                    finishDownload(QStringLiteral("Saved %1 (%2 MB).")
                                       .arg(QDir::toNativeSeparators(saved))
                                       .arg(QFileInfo(saved).size() / 1048576.0, 0, 'f', 1));
            });
    session->connectToHost(settings);
    settings.password.fill(QChar(0));
}

void NetworkToolsWindow::finishDownload(const QString &message)
{
    if (m_ftpDownload) {
        disconnect(m_ftpDownload, nullptr, this, nullptr);
        m_ftpDownload->deleteLater();
        m_ftpDownload = nullptr;
    }
    if (m_downloadFile) {
        m_downloadFile->close();
        m_downloadFile->deleteLater();
        m_downloadFile = nullptr;
    }
    if (m_download)
        m_download->deleteLater();
    m_download = nullptr;
    m_downloadStart->setText(QStringLiteral("Download"));
    m_downloadProgress->setMaximum(1000);
    m_downloadStatus->setText(message);
}

QWidget *NetworkToolsWindow::buildBandwidthTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;
    m_bandwidthMode = new QComboBox(page);
    m_bandwidthMode->addItem(QStringLiteral("Test against an iperf3 server"), QStringLiteral("client"));
    m_bandwidthMode->addItem(QStringLiteral("Be the server (iperf3 -c tests against this computer)"),
                             QStringLiteral("server"));
    m_bandwidthHost = new QLineEdit(page);
    m_bandwidthHost->setPlaceholderText(QStringLiteral("host running iperf3 -s"));
    m_bandwidthPort = new QSpinBox(page);
    m_bandwidthPort->setRange(1, 65535);
    m_bandwidthPort->setValue(5201);
    m_bandwidthSeconds = new QSpinBox(page);
    m_bandwidthSeconds->setRange(1, 3600);
    m_bandwidthSeconds->setValue(10);
    m_bandwidthSeconds->setSuffix(QStringLiteral(" s"));
    m_bandwidthStreams = new QSpinBox(page);
    m_bandwidthStreams->setRange(1, 64);
    m_bandwidthDirection = new QComboBox(page);
    m_bandwidthDirection->addItem(QStringLiteral("Upload (this computer sends)"), false);
    m_bandwidthDirection->addItem(QStringLiteral("Download (the server sends)"), true);
    m_bandwidthListen = ListenAddress::makeBox(ListenAddress::All, page);
    form->addRow(QStringLiteral("Mode:"), m_bandwidthMode);
    form->addRow(QStringLiteral("Host:"), m_bandwidthHost);
    form->addRow(QStringLiteral("Listen on:"), m_bandwidthListen);
    form->addRow(QStringLiteral("Port:"), m_bandwidthPort);
    form->addRow(QStringLiteral("Duration:"), m_bandwidthSeconds);
    form->addRow(QStringLiteral("Parallel streams:"), m_bandwidthStreams);
    form->addRow(QStringLiteral("Direction:"), m_bandwidthDirection);
    layout->addLayout(form);
    m_bandwidthStart = new QPushButton(QStringLiteral("Start"), page);
    layout->addWidget(m_bandwidthStart, 0, Qt::AlignLeft);
    m_bandwidthGraph = new BandwidthGraph(page);
    layout->addWidget(m_bandwidthGraph);
    m_bandwidthOutput = new QPlainTextEdit(page);
    m_bandwidthOutput->setMaximumBlockCount(MaximumOutputLines);
    m_bandwidthOutput->setReadOnly(true);
    m_bandwidthOutput->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(m_bandwidthOutput, 1);
    layout->addWidget(hint(QStringLiteral(
        "Speaks the iperf3 protocol (TCP), so it works with iperf3 on servers, routers, and "
        "phones without an iperf3 program here. As the server, other computers run "
        "iperf3 -c <this computer>; the firewall may ask the first time."), page));
    // The client's fields have no meaning for the server, which follows its
    // clients; there the duration is the longest test it accepts.
    const auto updateMode = [this, form] {
        const bool client = m_bandwidthMode->currentData().toString() == QStringLiteral("client");
        for (QWidget *field : {static_cast<QWidget *>(m_bandwidthHost), static_cast<QWidget *>(m_bandwidthStreams),
                               static_cast<QWidget *>(m_bandwidthDirection)})
            field->setEnabled(client);
        m_bandwidthListen->setEnabled(!client);
        if (auto *label = qobject_cast<QLabel *>(form->labelForField(m_bandwidthSeconds)))
            label->setText(client ? QStringLiteral("Duration:") : QStringLiteral("Longest test:"));
        m_bandwidthSeconds->setToolTip(client ? QString()
                                              : QStringLiteral("Clients asking for a longer test are refused, "
                                                               "and a test is stopped after it."));
    };
    connect(m_bandwidthMode, &QComboBox::currentIndexChanged, page, updateMode);
    updateMode();
    connect(m_bandwidthStart, &QPushButton::clicked, this, &NetworkToolsWindow::toggleBandwidth);
    connect(m_bandwidthHost, &QLineEdit::returnPressed, m_bandwidthStart, &QPushButton::click);
    return page;
}

void NetworkToolsWindow::toggleBandwidth()
{
    if (m_bandwidth) {
        m_bandwidth->stop();
        return;
    }
    IperfTest::Settings settings;
    settings.server = m_bandwidthMode->currentData().toString() == QStringLiteral("server");
    settings.host = m_bandwidthHost->text().trimmed();
    settings.port = quint16(m_bandwidthPort->value());
    settings.seconds = m_bandwidthSeconds->value();
    settings.streams = m_bandwidthStreams->value();
    settings.reverse = m_bandwidthDirection->currentData().toBool();
    settings.listen = m_bandwidthListen->currentData().toString();
    if (!settings.server && settings.host.isEmpty()) {
        m_bandwidthOutput->appendPlainText(QStringLiteral("Enter the host running iperf3 -s."));
        return;
    }
    m_bandwidthOutput->clear();
    static_cast<BandwidthGraph *>(m_bandwidthGraph)->clear();
    m_bandwidth = IperfTest::start(settings);
    m_bandwidthStart->setText(QStringLiteral("Stop"));
    connect(m_bandwidth, &IperfTest::line, m_bandwidthOutput, &QPlainTextEdit::appendPlainText);
    connect(m_bandwidth, &IperfTest::interval, this, [this](double, double bits) {
        static_cast<BandwidthGraph *>(m_bandwidthGraph)->add(bits);
    });
    connect(m_bandwidth, &IperfTest::finished, this, [this](const QString &summary, bool error) {
        m_bandwidthOutput->appendPlainText(error ? QStringLiteral("Error: %1").arg(summary) : summary);
        m_bandwidthStart->setText(QStringLiteral("Start"));
    });
}

NetworkToolsWindow::~NetworkToolsWindow()
{
    // A running ping or traceroute ends with the window.
    for (QProcess *process : {m_ping.data(), m_trace.data()}) {
        if (process) {
            process->disconnect();
            process->kill();
            process->waitForFinished(1000);
        }
    }
    if (m_scanning)
        stopScan(QString());
    if (m_bandwidth)
        m_bandwidth->stop();
    if (m_download) {
        m_download->disconnect(this);
        m_download->abort();
    }
}

void NetworkToolsWindow::setHost(const QString &host)
{
    if (host.isEmpty())
        return;
    if (!m_bandwidth)
        m_bandwidthHost->setText(host);
    for (auto *edit : {m_pingHost, m_traceHost, m_scanHost}) {
        if (!(m_scanning && edit == m_scanHost))
            edit->setText(host);
    }
}

QWidget *NetworkToolsWindow::buildCommandTab(QLineEdit **host, QPlainTextEdit **output,
                                             QPushButton **start, QWidget *options)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *row = new QHBoxLayout;
    *host = new QLineEdit(page);
    (*host)->setPlaceholderText(QStringLiteral("host name or IP address"));
    *start = new QPushButton(QStringLiteral("Start"), page);
    row->addWidget(new QLabel(QStringLiteral("Host:"), page));
    row->addWidget(*host, 1);
    row->addWidget(*start);
    layout->addLayout(row);
    if (options) {
        options->setParent(page);
        layout->addWidget(options);
    }
    *output = new QPlainTextEdit(page);
    (*output)->setReadOnly(true);
    // A long ping keeps its last lines only.
    (*output)->setMaximumBlockCount(MaximumOutputLines);
    (*output)->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(*output, 1);
    return page;
}

// Runs a system command, showing its output as it comes; the button stops it.
void NetworkToolsWindow::runCommand(QPointer<QProcess> &process, QPlainTextEdit *output,
                                    QPushButton *start, const QString &program,
                                    const QStringList &arguments)
{
    if (process) {
        process->kill();
        return;
    }
    output->clear();
    output->appendPlainText(QStringLiteral("$ %1 %2\n").arg(program, arguments.join(QLatin1Char(' '))));
    auto *child = new QProcess(this);
    process = child;
    child->setProcessChannelMode(QProcess::MergedChannels);
    connect(child, &QProcess::readyRead, output, [child, output] {
        output->moveCursor(QTextCursor::End);
        output->insertPlainText(QString::fromLocal8Bit(child->readAll()));
        output->verticalScrollBar()->setValue(output->verticalScrollBar()->maximum());
    });
    // Tied to the output, so nothing arrives once the window is gone.
    connect(child, &QProcess::finished, output, [child, output, start](int code, QProcess::ExitStatus status) {
        output->appendPlainText(status == QProcess::CrashExit ? QStringLiteral("\nStopped.")
                                                              : QStringLiteral("\nFinished (exit code %1).").arg(code));
        start->setText(QStringLiteral("Start"));
        child->deleteLater();
    });
    connect(child, &QProcess::errorOccurred, output, [child, output, start, program](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        output->appendPlainText(QStringLiteral("Could not start %1.").arg(program));
        start->setText(QStringLiteral("Start"));
        child->deleteLater();
    });
    start->setText(QStringLiteral("Stop"));
    child->start(program, arguments);
}

void NetworkToolsWindow::startPing()
{
    const QString host = m_pingHost->text().trimmed();
    if (host.isEmpty() && !m_ping)
        return;
#ifdef Q_OS_WIN
    const QStringList arguments{QStringLiteral("-n"), QString::number(m_pingCount->value()), host};
#else
    const QStringList arguments{QStringLiteral("-c"), QString::number(m_pingCount->value()), host};
#endif
    runCommand(m_ping, m_pingOutput, m_pingStart, QStringLiteral("ping"), arguments);
}

void NetworkToolsWindow::startTraceroute()
{
    const QString host = m_traceHost->text().trimmed();
    if (host.isEmpty() && !m_trace)
        return;
#ifdef Q_OS_WIN
    runCommand(m_trace, m_traceOutput, m_traceStart, QStringLiteral("tracert"),
               {QStringLiteral("-d"), host});
#else
    // traceroute is not always installed; tracepath (iputils) usually is.
    if (!m_trace && QStandardPaths::findExecutable(QStringLiteral("traceroute")).isEmpty()) {
        if (QStandardPaths::findExecutable(QStringLiteral("tracepath")).isEmpty()) {
            m_traceOutput->setPlainText(QStringLiteral("Neither traceroute nor tracepath is "
                                                       "installed on this computer."));
            return;
        }
        runCommand(m_trace, m_traceOutput, m_traceStart, QStringLiteral("tracepath"),
                   {QStringLiteral("-n"), host});
        return;
    }
    runCommand(m_trace, m_traceOutput, m_traceStart, QStringLiteral("traceroute"),
               {QStringLiteral("-n"), host});
#endif
}

void NetworkToolsWindow::startScan()
{
    const QString host = m_scanHost->text().trimmed();
    QString error;
    const auto ports = parsePorts(m_scanPorts->text(), &error);
    if (host.isEmpty() || ports.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Port Scan"),
                             host.isEmpty() ? QStringLiteral("Enter a host.") : error);
        return;
    }
    // One address for every port, so a name is not looked up thousands of times.
    const QHostInfo info = QHostInfo::fromName(host);
    if (info.addresses().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Port Scan"),
                             QStringLiteral("Could not find %1: %2").arg(host, info.errorString()));
        return;
    }
    m_scanAddress = info.addresses().constFirst();
    for (const auto &address : info.addresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            m_scanAddress = address; // IPv4 first, as most services listen there
            break;
        }
    }
    m_scanQueue = ports.mid(0, MaximumPorts);
    m_scanTotal = m_scanQueue.size();
    m_scanOpen = 0;
    m_scanResults->clear();
    m_scanProgress->setRange(0, int(m_scanTotal));
    m_scanProgress->setValue(0);
    m_scanning = true;
    m_scanTimer.start();
    m_scanStart->setText(QStringLiteral("Stop"));
    m_scanStatus->setText(QStringLiteral("Scanning %1 (%2)…").arg(host, m_scanAddress.toString()));
    for (int i = 0; i < ParallelConnections && !m_scanQueue.isEmpty(); ++i)
        scanNext();
}

void NetworkToolsWindow::scanNext()
{
    if (!m_scanning)
        return;
    if (m_scanQueue.isEmpty()) {
        if (m_scanSockets.isEmpty())
            stopScan(QString());
        return;
    }
    const quint16 port = m_scanQueue.takeFirst();
    auto *socket = new QTcpSocket(this);
    socket->setProperty("port", port);
    m_scanSockets.append(socket);
    auto *timer = new QTimer(socket);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, socket] { finishPort(socket, false); });
    connect(socket, &QTcpSocket::connected, this, [this, socket] { finishPort(socket, true); });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket] { finishPort(socket, false); });
    timer->start(m_scanTimeout->value());
    socket->connectToHost(m_scanAddress, port);
}

void NetworkToolsWindow::finishPort(QTcpSocket *socket, bool open)
{
    if (!m_scanSockets.removeOne(socket))
        return; // already finished (timeout and error can both arrive)
    const auto port = quint16(socket->property("port").toUInt());
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    if (open) {
        ++m_scanOpen;
        auto *item = new QTreeWidgetItem;
        item->setData(0, Qt::DisplayRole, port);
        item->setText(1, QStringLiteral("open"));
        item->setText(2, serviceName(port));
        m_scanResults->addTopLevelItem(item);
    }
    m_scanProgress->setValue(int(m_scanTotal - m_scanQueue.size() - m_scanSockets.size()));
    scanNext();
}

void NetworkToolsWindow::stopScan(const QString &message)
{
    m_scanning = false;
    for (auto *socket : std::exchange(m_scanSockets, {})) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_scanQueue.clear();
    m_scanStart->setText(QStringLiteral("Scan"));
    const QString summary = QStringLiteral("%1 open of %2 ports scanned in %3 s.")
                                .arg(m_scanOpen)
                                .arg(m_scanProgress->value())
                                .arg(m_scanTimer.elapsed() / 1000.0, 0, 'f', 1);
    m_scanStatus->setText(message.isEmpty() ? summary : message + QLatin1Char(' ') + summary);
}
