// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "httprequest.h"

#include <QElapsedTimer>
#include <QList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class QTreeWidget;

// A saved request (settings/http-requests.yaml).
struct SavedHttpRequest
{
    QString name;
    HttpRequestSpec spec;
    QString bodyType; // none, text, json, form, file
    QString bodyText; // or the file's path for "file"
};

QList<SavedHttpRequest> loadSavedHttpRequests();
void saveSavedHttpRequests(const QList<SavedHttpRequest> &requests);
// The curl command line that sends the same request.
QString curlCommand(const HttpRequestSpec &spec);

// Network Tools → HTTP Request: any method, headers and body; where the
// connection goes, the TLS name (SNI) and the Host header each settable; the
// answer with its headers, body, timings and certificate; the same request
// many times, a few at once, with timing statistics.
class HttpRequestPage final : public QWidget
{
    Q_OBJECT

public:
    explicit HttpRequestPage(QWidget *parent = nullptr);

    void setUrl(const QString &url);

private:
    HttpRequestSpec currentSpec(QString *problem) const;
    void showSpec(const SavedHttpRequest &request);
    void send();
    void showResponse(const HttpResponse &response);
    void addHeaderRow(const HttpRequestSpec::Header &header);
    void fillSaved(const QString &select);
    void saveCurrent(bool asNew);
    void toggleRepeat();
    void repeatFinished();
    void updateBodyType();

    HttpRequester *m_requester = nullptr;
    QComboBox *m_saved = nullptr;
    QComboBox *m_method = nullptr;
    QLineEdit *m_url = nullptr;
    QPushButton *m_send = nullptr;
    QLineEdit *m_connectTo = nullptr;
    QLineEdit *m_sni = nullptr;
    QLineEdit *m_hostHeader = nullptr;
    QCheckBox *m_verify = nullptr;
    QCheckBox *m_follow = nullptr;
    QSpinBox *m_timeout = nullptr;
    QTableWidget *m_headers = nullptr;
    QComboBox *m_bodyType = nullptr;
    QPlainTextEdit *m_body = nullptr;
    QLineEdit *m_bodyFile = nullptr;
    QPushButton *m_bodyBrowse = nullptr;
    QLabel *m_status = nullptr;
    QTabWidget *m_responseTabs = nullptr;
    QPlainTextEdit *m_responseBody = nullptr;
    QCheckBox *m_pretty = nullptr;
    QTreeWidget *m_responseHeaders = nullptr;
    QTreeWidget *m_timings = nullptr;
    QPlainTextEdit *m_certificate = nullptr;
    QPlainTextEdit *m_raw = nullptr;
    HttpResponse m_last;

    // Repeat
    QSpinBox *m_repeatCount = nullptr;
    QSpinBox *m_repeatParallel = nullptr;
    QPushButton *m_repeatRun = nullptr;
    QLabel *m_repeatSummary = nullptr;
    QTableWidget *m_repeatResults = nullptr;
    HttpRequestSpec m_repeatSpec;
    QList<HttpRequester *> m_repeaters;
    int m_repeatStarted = 0;
    int m_repeatDone = 0;
    QList<qint64> m_repeatTimes;
    QMap<QString, int> m_repeatOutcomes;
    QElapsedTimer m_repeatClock;
    bool m_repeating = false;
};
