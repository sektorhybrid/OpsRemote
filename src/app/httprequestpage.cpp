// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "httprequestpage.h"
#include "appdirs.h"
#include "preferences.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QSslCertificateExtension>
#include <QStringDecoder>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <yaml-cpp/yaml.h>

#include <algorithm>

namespace {

QString requestsPath()
{
    return portableFilePath(QStringLiteral("settings/http-requests.yaml"));
}

QString sizeText(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(double(bytes) / 1024, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
}

QString shellQuote(const QString &text)
{
    QString quoted = text;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// The text of a body, or why it is not shown as text.
QString bodyText(const QByteArray &body, const QByteArray &contentType, bool pretty)
{
    if (body.isEmpty())
        return QString();
    if (pretty && contentType.toLower().contains("json")) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(body, &error);
        if (error.error == QJsonParseError::NoError)
            return QString::fromUtf8(document.toJson(QJsonDocument::Indented));
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder.decode(body);
    if (!decoder.hasError() && !body.contains('\0'))
        return text;
    // Binary: the start as hex.
    QString hex;
    const QByteArray start = body.left(4096);
    for (qsizetype offset = 0; offset < start.size(); offset += 16) {
        hex += QStringLiteral("%1  %2\n").arg(offset, 8, 16, QLatin1Char('0'))
                   .arg(QString::fromLatin1(start.mid(offset, 16).toHex(' ')));
    }
    return QStringLiteral("[Binary, %1; the start as hexadecimal. Save Body… keeps all of it.]\n\n%2")
        .arg(sizeText(body.size()), hex);
}

QString certificateText(const HttpResponse &response)
{
    QStringList lines;
    lines << QStringLiteral("Connected to %1 port %2").arg(response.address.toString()).arg(response.port);
    if (!response.tls) {
        lines << QStringLiteral("Plain HTTP: not encrypted.");
        return lines.join(QLatin1Char('\n'));
    }
    lines << QStringLiteral("Name sent (SNI): %1").arg(response.sniSent.isEmpty() ? QStringLiteral("none (an address)")
                                                                                   : response.sniSent);
    if (!response.tlsProtocol.isEmpty())
        lines << QStringLiteral("Protocol: %1, cipher %2").arg(response.tlsProtocol, response.cipher);
    lines << (response.certificateProblems.isEmpty()
                  ? QStringLiteral("Certificate: trusted, and valid for the name.")
                  : QStringLiteral("Certificate NOT trusted: %1").arg(response.certificateProblems.join(QStringLiteral("; "))));
    for (int index = 0; index < response.certificates.size(); ++index) {
        const auto &certificate = response.certificates[index];
        lines << QString() << (index == 0 ? QStringLiteral("Server certificate") : QStringLiteral("Issuer %1").arg(index));
        lines << QStringLiteral("  Subject:  %1").arg(certificate.subjectDisplayName());
        lines << QStringLiteral("  Issuer:   %1").arg(certificate.issuerDisplayName());
        lines << QStringLiteral("  Valid:    %1 to %2")
                     .arg(certificate.effectiveDate().toString(Qt::ISODate), certificate.expiryDate().toString(Qt::ISODate));
        QStringList names;
        const auto alternatives = certificate.subjectAlternativeNames();
        for (auto it = alternatives.cbegin(); it != alternatives.cend(); ++it)
            names << it.value();
        if (!names.isEmpty())
            lines << QStringLiteral("  Names:    %1").arg(names.join(QStringLiteral(", ")));
        lines << QStringLiteral("  SHA-256:  %1").arg(QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex(':')));
    }
    return lines.join(QLatin1Char('\n'));
}

QString percentile(QList<qint64> sorted, double fraction)
{
    if (sorted.isEmpty())
        return QStringLiteral("-");
    const qsizetype index = qMin(sorted.size() - 1, qsizetype(fraction * double(sorted.size() - 1) + 0.5));
    return QStringLiteral("%1 ms").arg(sorted[index]);
}

}

QList<SavedHttpRequest> loadSavedHttpRequests()
{
    QList<SavedHttpRequest> requests;
    try {
        const auto root = YAML::LoadFile(requestsPath().toStdString());
        for (const auto &node : root["requests"]) {
            SavedHttpRequest request;
            const auto text = [&node](const char *key) {
                return node[key] ? QString::fromStdString(node[key].as<std::string>()) : QString();
            };
            request.name = text("name");
            request.spec.method = text("method");
            request.spec.url = text("url");
            request.spec.connectTo = text("connect_to");
            request.spec.sni = text("sni");
            request.spec.hostHeader = text("host_header");
            request.bodyType = text("body_type");
            request.bodyText = text("body");
            request.spec.verifyCertificates = node["verify_certificates"].as<bool>(true);
            request.spec.followRedirects = node["follow_redirects"].as<bool>(true);
            request.spec.timeoutSeconds = node["timeout_seconds"].as<int>(30);
            for (const auto &header : node["headers"]) {
                request.spec.headers.append({header["on"].as<bool>(true),
                                             QString::fromStdString(header["name"].as<std::string>("")),
                                             QString::fromStdString(header["value"].as<std::string>(""))});
            }
            if (!request.name.isEmpty())
                requests.append(request);
        }
    } catch (const YAML::Exception &) {
        // None yet, or unreadable: start empty.
    }
    return requests;
}

void saveSavedHttpRequests(const QList<SavedHttpRequest> &requests)
{
    YAML::Node root(YAML::NodeType::Map);
    YAML::Node list(YAML::NodeType::Sequence);
    for (const auto &request : requests) {
        YAML::Node node(YAML::NodeType::Map);
        node["name"] = request.name.toStdString();
        node["method"] = request.spec.method.toStdString();
        node["url"] = request.spec.url.toStdString();
        if (!request.spec.connectTo.isEmpty())
            node["connect_to"] = request.spec.connectTo.toStdString();
        if (!request.spec.sni.isEmpty())
            node["sni"] = request.spec.sni.toStdString();
        if (!request.spec.hostHeader.isEmpty())
            node["host_header"] = request.spec.hostHeader.toStdString();
        node["verify_certificates"] = request.spec.verifyCertificates;
        node["follow_redirects"] = request.spec.followRedirects;
        node["timeout_seconds"] = request.spec.timeoutSeconds;
        YAML::Node headers(YAML::NodeType::Sequence);
        for (const auto &header : request.spec.headers) {
            YAML::Node entry(YAML::NodeType::Map);
            entry["on"] = header.enabled;
            entry["name"] = header.name.toStdString();
            entry["value"] = header.value.toStdString();
            headers.push_back(entry);
        }
        node["headers"] = headers;
        node["body_type"] = request.bodyType.toStdString();
        if (!request.bodyText.isEmpty())
            node["body"] = request.bodyText.toStdString();
        list.push_back(node);
    }
    root["requests"] = list;
    YAML::Emitter emitter;
    emitter << root;
    QSaveFile file(requestsPath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    QByteArray contents(emitter.c_str());
    contents.append('\n');
    if (file.write(contents) == contents.size())
        file.commit();
    else
        file.cancelWriting();
}

QString curlCommand(const HttpRequestSpec &spec)
{
    QStringList parts{QStringLiteral("curl")};
    const QString method = spec.method.trimmed().toUpper();
    if (method == QStringLiteral("HEAD"))
        parts << QStringLiteral("-I");
    else if (method != QStringLiteral("GET") || !spec.body.isEmpty())
        parts << QStringLiteral("-X") << shellQuote(method);
    QUrl url = QUrl::fromUserInput(spec.url.trimmed());
    const QString originalHost = url.host();
    const int port = url.port(url.scheme() == QStringLiteral("https") ? 443 : 80);
    // curl sends the URL's host as SNI: a different SNI goes in the URL, and
    // the Host header keeps the original.
    if (!spec.sni.trimmed().isEmpty() && url.scheme() == QStringLiteral("https")) {
        url.setHost(spec.sni.trimmed());
        if (spec.hostHeader.trimmed().isEmpty())
            parts << QStringLiteral("-H") << shellQuote(QStringLiteral("Host: %1").arg(originalHost));
    }
    if (!spec.connectTo.trimmed().isEmpty()) {
        QString target = spec.connectTo.trimmed();
        if (target.count(QLatin1Char(':')) != 1 && !target.startsWith(QLatin1Char('[')))
            target += QStringLiteral(":%1").arg(port);
        parts << QStringLiteral("--connect-to") << shellQuote(QStringLiteral("%1:%2:%3").arg(url.host()).arg(port).arg(target));
    }
    if (!spec.hostHeader.trimmed().isEmpty())
        parts << QStringLiteral("-H") << shellQuote(QStringLiteral("Host: %1").arg(spec.hostHeader.trimmed()));
    for (const auto &header : spec.headers) {
        if (header.enabled && !header.name.trimmed().isEmpty())
            parts << QStringLiteral("-H") << shellQuote(QStringLiteral("%1: %2").arg(header.name.trimmed(), header.value));
    }
    if (!spec.body.isEmpty()) {
        if (!spec.contentType.isEmpty())
            parts << QStringLiteral("-H") << shellQuote(QStringLiteral("Content-Type: %1").arg(spec.contentType));
        parts << QStringLiteral("--data-binary") << shellQuote(QString::fromUtf8(spec.body));
    }
    if (!spec.verifyCertificates)
        parts << QStringLiteral("-k");
    if (spec.followRedirects)
        parts << QStringLiteral("-L");
    parts << QStringLiteral("--max-time") << QString::number(spec.timeoutSeconds);
    parts << QStringLiteral("--compressed") << QStringLiteral("-sS") << QStringLiteral("-D -");
    parts << shellQuote(url.toString());
    return parts.join(QLatin1Char(' '));
}

HttpRequestPage::HttpRequestPage(QWidget *parent) : QWidget(parent)
{
    const auto defaults = loadHttpRequestPreferences();
    m_requester = new HttpRequester(this);
    auto *layout = new QVBoxLayout(this);

    // Saved requests
    auto *savedRow = new QHBoxLayout;
    m_saved = new QComboBox(this);
    m_saved->setMinimumContentsLength(24);
    m_saved->setAccessibleName(QStringLiteral("Saved requests"));
    auto *save = new QPushButton(QStringLiteral("Save"), this);
    auto *saveAs = new QPushButton(QStringLiteral("Save As…"), this);
    auto *remove = new QPushButton(QStringLiteral("Delete"), this);
    savedRow->addWidget(new QLabel(QStringLiteral("Saved:"), this));
    savedRow->addWidget(m_saved, 1);
    savedRow->addWidget(save);
    savedRow->addWidget(saveAs);
    savedRow->addWidget(remove);
    layout->addLayout(savedRow);

    // The request line
    auto *requestRow = new QHBoxLayout;
    m_method = new QComboBox(this);
    m_method->setEditable(true);
    m_method->addItems({QStringLiteral("GET"), QStringLiteral("POST"), QStringLiteral("PUT"), QStringLiteral("PATCH"),
                        QStringLiteral("DELETE"), QStringLiteral("HEAD"), QStringLiteral("OPTIONS")});
    m_method->setAccessibleName(QStringLiteral("Method"));
    m_url = new QLineEdit(this);
    m_url->setPlaceholderText(QStringLiteral("https://example.com/path?query"));
    m_url->setAccessibleName(QStringLiteral("URL"));
    m_send = new QPushButton(QStringLiteral("Send"), this);
    m_send->setDefault(true);
    auto *curl = new QPushButton(QStringLiteral("Copy as curl"), this);
    curl->setToolTip(QStringLiteral("The curl command that sends the same request"));
    requestRow->addWidget(m_method);
    requestRow->addWidget(m_url, 1);
    requestRow->addWidget(m_send);
    requestRow->addWidget(curl);
    layout->addLayout(requestRow);

    // Where it goes and how
    auto *connectionRow = new QHBoxLayout;
    m_connectTo = new QLineEdit(this);
    m_connectTo->setPlaceholderText(QStringLiteral("the URL's host"));
    m_connectTo->setToolTip(QStringLiteral(
        "Connect to this host or address (and port) instead of the URL's, like curl --connect-to: for a "
        "server behind a load balancer, or one not yet in DNS."));
    m_sni = new QLineEdit(this);
    m_sni->setPlaceholderText(QStringLiteral("the URL's host"));
    m_sni->setToolTip(QStringLiteral(
        "The name sent in the TLS handshake (SNI), which picks the server's certificate and site; the "
        "certificate is checked against it."));
    m_hostHeader = new QLineEdit(this);
    m_hostHeader->setPlaceholderText(QStringLiteral("the URL's host"));
    m_hostHeader->setToolTip(QStringLiteral("The Host header, which picks the web site on the server."));
    connectionRow->addWidget(new QLabel(QStringLiteral("Connect to:"), this));
    connectionRow->addWidget(m_connectTo, 1);
    connectionRow->addWidget(new QLabel(QStringLiteral("SNI:"), this));
    connectionRow->addWidget(m_sni, 1);
    connectionRow->addWidget(new QLabel(QStringLiteral("Host header:"), this));
    connectionRow->addWidget(m_hostHeader, 1);
    layout->addLayout(connectionRow);
    auto *optionsRow = new QHBoxLayout;
    m_verify = new QCheckBox(QStringLiteral("Check the certificate"), this);
    m_verify->setChecked(defaults.verifyCertificates);
    m_verify->setToolTip(QStringLiteral("Off: the answer comes even from an untrusted certificate; the "
                                        "Certificate tab still says what is wrong with it."));
    m_follow = new QCheckBox(QStringLiteral("Follow redirects"), this);
    m_follow->setChecked(defaults.followRedirects);
    m_timeout = new QSpinBox(this);
    m_timeout->setRange(1, 3600);
    m_timeout->setSuffix(QStringLiteral(" s"));
    m_timeout->setValue(defaults.timeoutSeconds);
    optionsRow->addWidget(m_verify);
    optionsRow->addWidget(m_follow);
    optionsRow->addSpacing(12);
    optionsRow->addWidget(new QLabel(QStringLiteral("Time limit:"), this));
    optionsRow->addWidget(m_timeout);
    optionsRow->addStretch();
    layout->addLayout(optionsRow);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    layout->addWidget(splitter, 1);

    // The request's headers, body, and repeats
    auto *requestTabs = new QTabWidget(splitter);
    auto *headersPage = new QWidget(requestTabs);
    auto *headersLayout = new QVBoxLayout(headersPage);
    m_headers = new QTableWidget(0, 3, headersPage);
    m_headers->setHorizontalHeaderLabels({QStringLiteral("Send"), QStringLiteral("Name"), QStringLiteral("Value")});
    m_headers->horizontalHeader()->setStretchLastSection(true);
    m_headers->verticalHeader()->hide();
    m_headers->setColumnWidth(1, 220);
    auto *headerButtons = new QHBoxLayout;
    auto *addHeader = new QPushButton(QStringLiteral("+ Add"), headersPage);
    auto *removeHeader = new QPushButton(QStringLiteral("Remove"), headersPage);
    auto *headerNote = new QLabel(QStringLiteral(
        "Added unless set here: Host, User-Agent, Accept, Accept-Encoding (gzip, deflate), Content-Length; "
        "each request uses its own connection."), headersPage);
    headerNote->setWordWrap(true);
    headerNote->setForegroundRole(QPalette::PlaceholderText);
    headerButtons->addWidget(addHeader);
    headerButtons->addWidget(removeHeader);
    headerButtons->addWidget(headerNote, 1);
    headersLayout->addWidget(m_headers, 1);
    headersLayout->addLayout(headerButtons);
    requestTabs->addTab(headersPage, QStringLiteral("Headers"));

    auto *bodyPage = new QWidget(requestTabs);
    auto *bodyLayout = new QVBoxLayout(bodyPage);
    auto *bodyRow = new QHBoxLayout;
    m_bodyType = new QComboBox(bodyPage);
    m_bodyType->addItem(QStringLiteral("No body"), QStringLiteral("none"));
    m_bodyType->addItem(QStringLiteral("Text"), QStringLiteral("text"));
    m_bodyType->addItem(QStringLiteral("JSON"), QStringLiteral("json"));
    m_bodyType->addItem(QStringLiteral("Form (name=value per line)"), QStringLiteral("form"));
    m_bodyType->addItem(QStringLiteral("File"), QStringLiteral("file"));
    m_bodyFile = new QLineEdit(bodyPage);
    m_bodyFile->setPlaceholderText(QStringLiteral("File to send"));
    m_bodyBrowse = new QPushButton(QStringLiteral("Choose…"), bodyPage);
    bodyRow->addWidget(m_bodyType);
    bodyRow->addWidget(m_bodyFile, 1);
    bodyRow->addWidget(m_bodyBrowse);
    bodyRow->addStretch();
    m_body = new QPlainTextEdit(bodyPage);
    m_body->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    bodyLayout->addLayout(bodyRow);
    bodyLayout->addWidget(m_body, 1);
    requestTabs->addTab(bodyPage, QStringLiteral("Body"));

    auto *repeatPage = new QWidget(requestTabs);
    auto *repeatLayout = new QVBoxLayout(repeatPage);
    auto *repeatRow = new QHBoxLayout;
    m_repeatCount = new QSpinBox(repeatPage);
    m_repeatCount->setRange(1, 100000);
    m_repeatCount->setValue(20);
    m_repeatCount->setSuffix(QStringLiteral(" times"));
    m_repeatParallel = new QSpinBox(repeatPage);
    m_repeatParallel->setRange(1, 100);
    m_repeatParallel->setValue(4);
    m_repeatParallel->setSuffix(QStringLiteral(" at once"));
    m_repeatRun = new QPushButton(QStringLiteral("Run"), repeatPage);
    repeatRow->addWidget(new QLabel(QStringLiteral("Send the request"), repeatPage));
    repeatRow->addWidget(m_repeatCount);
    repeatRow->addWidget(m_repeatParallel);
    repeatRow->addWidget(m_repeatRun);
    repeatRow->addStretch();
    m_repeatSummary = new QLabel(repeatPage);
    m_repeatSummary->setWordWrap(true);
    m_repeatSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_repeatResults = new QTableWidget(0, 4, repeatPage);
    m_repeatResults->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Result"), QStringLiteral("Time"),
                                                QStringLiteral("Size")});
    m_repeatResults->horizontalHeader()->setStretchLastSection(true);
    m_repeatResults->verticalHeader()->hide();
    m_repeatResults->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto *repeatNote = new QLabel(QStringLiteral(
        "For checking a server you look after. Many requests at once can count as an attack on other people's "
        "servers."), repeatPage);
    repeatNote->setWordWrap(true);
    repeatNote->setForegroundRole(QPalette::PlaceholderText);
    repeatLayout->addLayout(repeatRow);
    repeatLayout->addWidget(m_repeatSummary);
    repeatLayout->addWidget(m_repeatResults, 1);
    repeatLayout->addWidget(repeatNote);
    requestTabs->addTab(repeatPage, QStringLiteral("Repeat"));

    // The answer
    auto *responsePage = new QWidget(splitter);
    auto *responseLayout = new QVBoxLayout(responsePage);
    responseLayout->setContentsMargins(0, 0, 0, 0);
    m_status = new QLabel(QStringLiteral("Send a request to see the answer."), responsePage);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setWordWrap(true);
    responseLayout->addWidget(m_status);
    m_responseTabs = new QTabWidget(responsePage);
    auto *bodyView = new QWidget(m_responseTabs);
    auto *bodyViewLayout = new QVBoxLayout(bodyView);
    auto *bodyViewRow = new QHBoxLayout;
    m_pretty = new QCheckBox(QStringLiteral("Format JSON"), bodyView);
    m_pretty->setChecked(true);
    auto *saveBody = new QPushButton(QStringLiteral("Save Body…"), bodyView);
    bodyViewRow->addWidget(m_pretty);
    bodyViewRow->addStretch();
    bodyViewRow->addWidget(saveBody);
    m_responseBody = new QPlainTextEdit(bodyView);
    m_responseBody->setReadOnly(true);
    m_responseBody->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    bodyViewLayout->addLayout(bodyViewRow);
    bodyViewLayout->addWidget(m_responseBody, 1);
    m_responseTabs->addTab(bodyView, QStringLiteral("Body"));
    m_responseHeaders = new QTreeWidget(m_responseTabs);
    m_responseHeaders->setRootIsDecorated(false);
    m_responseHeaders->setHeaderLabels({QStringLiteral("Header"), QStringLiteral("Value")});
    m_responseHeaders->setColumnWidth(0, 220);
    m_responseTabs->addTab(m_responseHeaders, QStringLiteral("Headers"));
    m_timings = new QTreeWidget(m_responseTabs);
    m_timings->setRootIsDecorated(false);
    m_timings->setHeaderLabels({QStringLiteral("Step"), QStringLiteral("Time"), QString()});
    m_timings->setColumnWidth(0, 220);
    m_timings->setColumnWidth(1, 100);
    m_responseTabs->addTab(m_timings, QStringLiteral("Timings"));
    m_certificate = new QPlainTextEdit(m_responseTabs);
    m_certificate->setReadOnly(true);
    m_certificate->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_responseTabs->addTab(m_certificate, QStringLiteral("Connection and certificate"));
    m_raw = new QPlainTextEdit(m_responseTabs);
    m_raw->setReadOnly(true);
    m_raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_responseTabs->addTab(m_raw, QStringLiteral("Raw"));
    responseLayout->addWidget(m_responseTabs, 1);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    connect(m_send, &QPushButton::clicked, this, [this] {
        if (m_requester->busy())
            m_requester->cancel();
        else
            send();
    });
    connect(m_url, &QLineEdit::returnPressed, m_send, &QPushButton::click);
    connect(m_requester, &HttpRequester::finished, this, &HttpRequestPage::showResponse);
    connect(curl, &QPushButton::clicked, this, [this] {
        QString problem;
        const auto spec = currentSpec(&problem);
        if (!problem.isEmpty()) {
            m_status->setText(problem);
            return;
        }
        QApplication::clipboard()->setText(curlCommand(spec));
        m_status->setText(QStringLiteral("The curl command is on the clipboard."));
    });
    connect(addHeader, &QPushButton::clicked, this, [this] {
        addHeaderRow({true, QString(), QString()});
        m_headers->editItem(m_headers->item(m_headers->rowCount() - 1, 1));
    });
    connect(removeHeader, &QPushButton::clicked, this, [this] {
        if (m_headers->currentRow() >= 0)
            m_headers->removeRow(m_headers->currentRow());
    });
    connect(m_bodyType, &QComboBox::currentIndexChanged, this, &HttpRequestPage::updateBodyType);
    connect(m_bodyBrowse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("File to Send"));
        if (!path.isEmpty())
            m_bodyFile->setText(path);
    });
    connect(m_pretty, &QCheckBox::toggled, this, [this] {
        m_responseBody->setPlainText(bodyText(m_last.body, m_last.header("Content-Type"), m_pretty->isChecked()));
    });
    connect(saveBody, &QPushButton::clicked, this, [this] {
        if (m_last.body.isEmpty())
            return;
        QString name = QUrl(m_last.url).fileName();
        if (name.isEmpty())
            name = QStringLiteral("response");
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save Body"), name);
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(m_last.body) != m_last.body.size() || !file.commit())
            QMessageBox::warning(this, QStringLiteral("Save Body"),
                                 QStringLiteral("Could not save %1: %2").arg(path, file.errorString()));
    });
    connect(m_repeatRun, &QPushButton::clicked, this, &HttpRequestPage::toggleRepeat);
    connect(m_saved, &QComboBox::activated, this, [this](int index) {
        const auto requests = loadSavedHttpRequests();
        const QString name = m_saved->itemData(index).toString();
        for (const auto &request : requests) {
            if (request.name == name)
                showSpec(request);
        }
    });
    connect(save, &QPushButton::clicked, this, [this] { saveCurrent(false); });
    connect(saveAs, &QPushButton::clicked, this, [this] { saveCurrent(true); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const QString name = m_saved->currentData().toString();
        if (name.isEmpty())
            return;
        auto requests = loadSavedHttpRequests();
        requests.erase(std::remove_if(requests.begin(), requests.end(),
                                      [&](const SavedHttpRequest &r) { return r.name == name; }),
                       requests.end());
        saveSavedHttpRequests(requests);
        fillSaved(QString());
    });

    fillSaved(QString());
    updateBodyType();
}

void HttpRequestPage::setUrl(const QString &url)
{
    m_url->setText(url);
}

void HttpRequestPage::addHeaderRow(const HttpRequestSpec::Header &header)
{
    const int row = m_headers->rowCount();
    m_headers->insertRow(row);
    auto *on = new QTableWidgetItem;
    on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    on->setCheckState(header.enabled ? Qt::Checked : Qt::Unchecked);
    m_headers->setItem(row, 0, on);
    m_headers->setItem(row, 1, new QTableWidgetItem(header.name));
    m_headers->setItem(row, 2, new QTableWidgetItem(header.value));
}

void HttpRequestPage::updateBodyType()
{
    const QString type = m_bodyType->currentData().toString();
    m_body->setVisible(type != QStringLiteral("none") && type != QStringLiteral("file"));
    m_bodyFile->setVisible(type == QStringLiteral("file"));
    m_bodyBrowse->setVisible(type == QStringLiteral("file"));
    m_body->setPlaceholderText(type == QStringLiteral("json")   ? QStringLiteral("{\"name\": \"value\"}")
                               : type == QStringLiteral("form") ? QStringLiteral("name=value\nother=more")
                                                                : QString());
}

HttpRequestSpec HttpRequestPage::currentSpec(QString *problem) const
{
    const auto defaults = loadHttpRequestPreferences();
    HttpRequestSpec spec;
    spec.method = m_method->currentText().trimmed().toUpper();
    spec.url = m_url->text().trimmed();
    spec.connectTo = m_connectTo->text().trimmed();
    spec.sni = m_sni->text().trimmed();
    spec.hostHeader = m_hostHeader->text().trimmed();
    spec.verifyCertificates = m_verify->isChecked();
    spec.followRedirects = m_follow->isChecked();
    spec.maxRedirects = defaults.maxRedirects;
    spec.timeoutSeconds = m_timeout->value();
    spec.maxBodyBytes = qint64(defaults.maxBodyMb) * 1024 * 1024;
    spec.userAgent = defaults.userAgent;
    for (int row = 0; row < m_headers->rowCount(); ++row) {
        const auto *name = m_headers->item(row, 1);
        if (!name || name->text().trimmed().isEmpty())
            continue;
        spec.headers.append({m_headers->item(row, 0)->checkState() == Qt::Checked, name->text().trimmed(),
                             m_headers->item(row, 2) ? m_headers->item(row, 2)->text() : QString()});
    }
    const QString type = m_bodyType->currentData().toString();
    if (type == QStringLiteral("text")) {
        spec.body = m_body->toPlainText().toUtf8();
        spec.contentType = QStringLiteral("text/plain; charset=utf-8");
    } else if (type == QStringLiteral("json")) {
        spec.body = m_body->toPlainText().toUtf8();
        spec.contentType = QStringLiteral("application/json");
        QJsonParseError error;
        QJsonDocument::fromJson(spec.body, &error);
        if (!spec.body.trimmed().isEmpty() && error.error != QJsonParseError::NoError)
            *problem = QStringLiteral("The JSON body is not valid: %1 (at character %2).")
                           .arg(error.errorString())
                           .arg(error.offset);
    } else if (type == QStringLiteral("form")) {
        QUrlQuery form;
        for (const auto &line : m_body->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            form.addQueryItem(line.section(QLatin1Char('='), 0, 0).trimmed(), line.section(QLatin1Char('='), 1));
        spec.body = form.toString(QUrl::FullyEncoded).toUtf8();
        spec.contentType = QStringLiteral("application/x-www-form-urlencoded");
    } else if (type == QStringLiteral("file")) {
        QFile file(m_bodyFile->text().trimmed());
        if (file.size() > 512LL * 1024 * 1024)
            *problem = QStringLiteral("The file is larger than 512 MB.");
        else if (!file.open(QIODevice::ReadOnly))
            *problem = QStringLiteral("Cannot read %1: %2").arg(file.fileName(), file.errorString());
        else
            spec.body = file.readAll();
        spec.contentType = QStringLiteral("application/octet-stream");
    }
    if (spec.url.isEmpty())
        *problem = QStringLiteral("Type the URL first.");
    static const QRegularExpression token(QStringLiteral("^[A-Z0-9!#$%&'*+.^_`|~-]+$"));
    if (!token.match(spec.method).hasMatch())
        *problem = QStringLiteral("\"%1\" is not a method name.").arg(spec.method);
    return spec;
}

void HttpRequestPage::send()
{
    QString problem;
    const auto spec = currentSpec(&problem);
    if (!problem.isEmpty()) {
        m_status->setText(problem);
        return;
    }
    m_status->setText(QStringLiteral("Sending…"));
    m_send->setText(QStringLiteral("Stop"));
    m_requester->send(spec);
}

void HttpRequestPage::showResponse(const HttpResponse &response)
{
    m_send->setText(QStringLiteral("Send"));
    m_last = response;
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    QString colour;
    QString text;
    if (!response.error.isEmpty() && response.status == 0) {
        colour = dark ? QStringLiteral("#f27a6e") : QStringLiteral("#b3261e");
        text = QStringLiteral("✕ %1").arg(response.error.toHtmlEscaped());
    } else {
        colour = response.status < 300   ? (dark ? QStringLiteral("#6cc67a") : QStringLiteral("#1e7b34"))
                 : response.status < 400 ? (dark ? QStringLiteral("#e0b050") : QStringLiteral("#8a5a00"))
                                         : (dark ? QStringLiteral("#f27a6e") : QStringLiteral("#b3261e"));
        text = QStringLiteral("%1 %2 %3 · %4 ms · %5%6")
                   .arg(response.status < 400 ? QStringLiteral("✓") : QStringLiteral("✕"))
                   .arg(response.status)
                   .arg(response.reason.toHtmlEscaped())
                   .arg(response.totalMs)
                   .arg(sizeText(response.body.size()))
                   .arg(response.contentEncoding.isEmpty()
                            ? QString()
                            : QStringLiteral(" (%1 %2)").arg(sizeText(response.bodyBytesOnWire), response.contentEncoding));
        if (!response.redirects.isEmpty())
            text += QStringLiteral(" · after %1 redirect(s), from %2").arg(response.redirects.size()).arg(response.url.toHtmlEscaped());
        if (response.bodyTruncated)
            text += QStringLiteral(" · body cut at the size limit (Preferences)");
        if (!response.error.isEmpty())
            text += QStringLiteral("<br>%1").arg(response.error.toHtmlEscaped());
        if (response.tls && !response.certificateProblems.isEmpty())
            text += QStringLiteral("<br>⚠ The certificate is not trusted: %1").arg(response.certificateProblems.join(QStringLiteral("; ")).toHtmlEscaped());
    }
    m_status->setText(QStringLiteral("<span style=\"color:%1\"><b>%2</b></span>").arg(colour, text));

    m_responseBody->setPlainText(bodyText(response.body, response.header("Content-Type"), m_pretty->isChecked()));
    m_responseHeaders->clear();
    for (const auto &[name, value] : response.headers)
        new QTreeWidgetItem(m_responseHeaders, {QString::fromLatin1(name), QString::fromUtf8(value)});
    m_timings->clear();
    const qint64 total = qMax<qint64>(1, response.totalMs);
    const qint64 setup = qMax<qint64>(0, response.dnsMs) + qMax<qint64>(0, response.connectMs) + qMax<qint64>(0, response.tlsMs);
    const auto addStep = [this, total](const QString &name, qint64 ms) {
        if (ms < 0)
            return;
        const int bar = int(qBound<qint64>(0, ms * 40 / total, qint64(40)));
        new QTreeWidgetItem(m_timings, {name, QStringLiteral("%1 ms").arg(ms), QString(bar, QChar(0x2588))});
    };
    addStep(QStringLiteral("Name lookup (DNS)"), response.dnsMs);
    addStep(QStringLiteral("Connect (TCP)"), response.connectMs);
    addStep(QStringLiteral("TLS handshake"), response.tlsMs);
    if (response.firstByteMs >= 0)
        addStep(QStringLiteral("Waiting for the answer"), qMax<qint64>(0, response.firstByteMs - setup));
    if (response.firstByteMs >= 0)
        addStep(QStringLiteral("Receiving"), qMax<qint64>(0, response.totalMs - response.firstByteMs));
    addStep(QStringLiteral("Total"), response.totalMs);
    m_certificate->setPlainText(response.address.isNull() ? response.error : certificateText(response));
    QString raw = QString::fromUtf8(response.requestHead);
    raw += QStringLiteral("\n") + QString::fromUtf8(response.responseHead);
    m_raw->setPlainText(raw);
}

void HttpRequestPage::showSpec(const SavedHttpRequest &request)
{
    const auto &spec = request.spec;
    m_method->setCurrentText(spec.method.isEmpty() ? QStringLiteral("GET") : spec.method);
    m_url->setText(spec.url);
    m_connectTo->setText(spec.connectTo);
    m_sni->setText(spec.sni);
    m_hostHeader->setText(spec.hostHeader);
    m_verify->setChecked(spec.verifyCertificates);
    m_follow->setChecked(spec.followRedirects);
    m_timeout->setValue(spec.timeoutSeconds);
    m_headers->setRowCount(0);
    for (const auto &header : spec.headers)
        addHeaderRow(header);
    const int type = m_bodyType->findData(request.bodyType.isEmpty() ? QStringLiteral("none") : request.bodyType);
    m_bodyType->setCurrentIndex(qMax(0, type));
    if (request.bodyType == QStringLiteral("file"))
        m_bodyFile->setText(request.bodyText);
    else
        m_body->setPlainText(request.bodyText);
}

void HttpRequestPage::fillSaved(const QString &select)
{
    m_saved->clear();
    m_saved->addItem(QStringLiteral("(not saved)"), QString());
    for (const auto &request : loadSavedHttpRequests())
        m_saved->addItem(request.name, request.name);
    m_saved->setCurrentIndex(qMax(0, m_saved->findData(select)));
}

void HttpRequestPage::saveCurrent(bool asNew)
{
    QString name = m_saved->currentData().toString();
    if (asNew || name.isEmpty()) {
        bool ok = false;
        name = QInputDialog::getText(this, QStringLiteral("Save Request"), QStringLiteral("Name:"), QLineEdit::Normal,
                                     QUrl(m_url->text().trimmed()).host(), &ok)
                   .trimmed();
        if (!ok || name.isEmpty())
            return;
    }
    QString problem;
    SavedHttpRequest request;
    request.name = name;
    request.spec = currentSpec(&problem);
    request.spec.body.clear(); // kept as typed below
    request.bodyType = m_bodyType->currentData().toString();
    request.bodyText = request.bodyType == QStringLiteral("file") ? m_bodyFile->text().trimmed()
                                                                   : m_body->toPlainText();
    auto requests = loadSavedHttpRequests();
    bool replaced = false;
    for (auto &existing : requests) {
        if (existing.name == name) {
            existing = request;
            replaced = true;
        }
    }
    if (!replaced)
        requests.append(request);
    saveSavedHttpRequests(requests);
    fillSaved(name);
}

void HttpRequestPage::toggleRepeat()
{
    if (m_repeating) {
        for (auto *requester : std::as_const(m_repeaters))
            requester->cancel();
        return;
    }
    QString problem;
    m_repeatSpec = currentSpec(&problem);
    if (!problem.isEmpty()) {
        m_repeatSummary->setText(problem);
        return;
    }
    m_repeating = true;
    m_repeatStarted = 0;
    m_repeatDone = 0;
    m_repeatTimes.clear();
    m_repeatOutcomes.clear();
    m_repeatResults->setRowCount(0);
    m_repeatRun->setText(QStringLiteral("Stop"));
    m_repeatClock.start();
    qDeleteAll(m_repeaters);
    m_repeaters.clear();
    const int parallel = qMin(m_repeatParallel->value(), m_repeatCount->value());
    for (int i = 0; i < parallel; ++i) {
        auto *requester = new HttpRequester(this);
        m_repeaters.append(requester);
        connect(requester, &HttpRequester::finished, this, [this, requester](const HttpResponse &response) {
            const int row = m_repeatResults->rowCount();
            m_repeatResults->insertRow(row);
            const QString outcome = response.status > 0 ? QStringLiteral("%1 %2").arg(response.status).arg(response.reason)
                                                        : response.error;
            m_repeatResults->setItem(row, 0, new QTableWidgetItem(QString::number(row + 1)));
            m_repeatResults->setItem(row, 1, new QTableWidgetItem(outcome));
            m_repeatResults->setItem(row, 2, new QTableWidgetItem(QStringLiteral("%1 ms").arg(response.totalMs)));
            m_repeatResults->setItem(row, 3, new QTableWidgetItem(sizeText(response.body.size())));
            ++m_repeatDone;
            ++m_repeatOutcomes[response.status > 0 ? QString::number(response.status) : QStringLiteral("failed")];
            if (response.status > 0)
                m_repeatTimes.append(response.totalMs);
            const bool stopped = response.error == QStringLiteral("Stopped.");
            if (!stopped && m_repeatStarted < m_repeatCount->value()) {
                ++m_repeatStarted;
                requester->send(m_repeatSpec);
            }
            repeatFinished();
        });
    }
    for (auto *requester : std::as_const(m_repeaters)) {
        ++m_repeatStarted;
        requester->send(m_repeatSpec);
    }
}

void HttpRequestPage::repeatFinished()
{
    bool anyBusy = false;
    for (auto *requester : std::as_const(m_repeaters))
        anyBusy = anyBusy || requester->busy();
    QList<qint64> sorted = m_repeatTimes;
    std::sort(sorted.begin(), sorted.end());
    qint64 sum = 0;
    for (const qint64 value : std::as_const(sorted))
        sum += value;
    QStringList outcomes;
    for (auto it = m_repeatOutcomes.cbegin(); it != m_repeatOutcomes.cend(); ++it)
        outcomes << QStringLiteral("%1 × %2").arg(it.value()).arg(it.key());
    const double seconds = qMax<qint64>(1, m_repeatClock.elapsed()) / 1000.0;
    m_repeatSummary->setText(
        QStringLiteral("%1 of %2 done in %3 (%4 per second) · %5<br>Time: fastest %6, average %7, median %8, "
                       "p90 %9, p99 %10, slowest %11")
            .arg(m_repeatDone)
            .arg(m_repeatCount->value())
            .arg(seconds < 1 ? QStringLiteral("%1 ms").arg(m_repeatClock.elapsed())
                             : QStringLiteral("%1 s").arg(seconds, 0, 'f', 1))
            .arg(m_repeatDone / seconds, 0, 'f', 1)
            .arg(outcomes.join(QStringLiteral(", ")))
            .arg(sorted.isEmpty() ? QStringLiteral("-") : QStringLiteral("%1 ms").arg(sorted.first()))
            .arg(sorted.isEmpty() ? QStringLiteral("-") : QStringLiteral("%1 ms").arg(sum / sorted.size()))
            .arg(percentile(sorted, 0.5), percentile(sorted, 0.9), percentile(sorted, 0.99))
            .arg(sorted.isEmpty() ? QStringLiteral("-") : QStringLiteral("%1 ms").arg(sorted.last())));
    if (!anyBusy) {
        m_repeating = false;
        m_repeatRun->setText(QStringLiteral("Run"));
    }
}
