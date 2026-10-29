// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>

class QTcpServer;
class QTcpSocket;
class QTimer;

// A bandwidth test that speaks the iperf3 protocol (TCP), so it works with
// any iperf3 3.x on the other side, without an iperf3 program here: a
// control connection with one-byte states and length-prefixed JSON for the
// parameters and results, and data connections that carry the test's bytes.
// Each object lives on a thread of its own (see IperfTest::start).
class IperfTest final : public QObject
{
    Q_OBJECT

public:
    struct Settings
    {
        bool server = false; // listen for iperf3 -c; otherwise connect to host
        QString host;
        quint16 port = 5201;
        int seconds = 10;
        int streams = 1;
        bool reverse = false; // the far side sends (download)
        // Server: where it listens (see ListenAddress); seconds is then the
        // longest test it accepts.
        QString listen = QStringLiteral("all");
    };

    // Runs a test on its own thread; the test deletes itself after
    // finished(), so hold it in a QPointer.
    static IperfTest *start(const Settings &settings);

    // Stops at once (from any thread).
    void stop();

signals:
    void line(const QString &text);
    // Bits per second for each interval (about a second), for the graph.
    void interval(double seconds, double bitsPerSecond);
    void finished(const QString &summary, bool error);

private:
    explicit IperfTest(const Settings &settings);
    void begin();
    // Client.
    void readControl();
    void handleState(qint8 state);
    void openStreams();
    // Server.
    void acceptControl();
    void acceptStream();
    void readServerControl();
    // Both.
    void startRunning();
    void pumpSender(QTcpSocket *socket);
    void tick();
    void sendJson(const QByteArray &json);
    bool readJson(QByteArray *json);
    QByteArray resultsJson() const;
    void fail(const QString &message);
    // Something that is not iperf3 arrived: the client gives up, the server
    // drops that client and waits for the next.
    void protocolError(const QString &message);
    // The control buffer plus what is waiting; false (and protocolError)
    // beyond what iperf3 ever sends at once.
    bool takeControlInput();
    void done(const QString &summary);
    void cleanUp();

    Settings m_settings;
    QByteArray m_cookie;
    QTcpServer *m_listener = nullptr;
    QTcpSocket *m_control = nullptr;
    QList<QTcpSocket *> m_streams;
    QByteArray m_controlBuffer;
    QTimer *m_ticker = nullptr;
    QTimer *m_deadline = nullptr;
    QElapsedTimer m_clock;
    qint64 m_bytes = 0; // sent or received, all streams
    qint64 m_lastBytes = 0;
    double m_lastTick = 0;
    double m_testSeconds = 0;
    bool m_sending = false; // this side sends the data
    bool m_running = false;
    bool m_finished = false;
    int m_stage = 0; // server: 0 cookie, 1 parameters, 2 streams, 3 running, 4 results, 5 done
    QTimer *m_guard = nullptr; // server: the handshake's and the test's time limit
    int m_longestTest = 0; // server: seconds
    QByteArray m_block;
};
