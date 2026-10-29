// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "iperf.h"
#include "listenaddress.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QtEndian>

namespace {

// iperf3's control states (iperf_api.h).
enum : qint8 {
    TestStart = 1,
    TestRunning = 2,
    TestEnd = 4,
    ParamExchange = 9,
    CreateStreams = 10,
    ServerTerminate = 11,
    ClientTerminate = 12,
    ExchangeResults = 13,
    DisplayResults = 14,
    IperfDone = 16,
    AccessDenied = -1,
    ServerError = -2,
};

constexpr int CookieSize = 37; // 36 characters and a NUL
constexpr int BlockSize = 128 * 1024; // iperf3's default for TCP
constexpr quint32 MaximumJson = 1 << 20;
// The server: how long a client may take to say what it wants, and how many
// connections may wait to become a test's streams.
constexpr int HandshakeMs = 10000;
constexpr int MaximumPendingStreams = 8;

// iperf3 numbers its streams 1, 3, 4, 5, ... (a quirk kept for
// compatibility between versions).
int streamId(int index)
{
    return index == 0 ? 1 : index + 2;
}

QString rate(double bitsPerSecond)
{
    if (bitsPerSecond >= 1e9)
        return QStringLiteral("%1 Gbit/s").arg(bitsPerSecond / 1e9, 0, 'f', 2);
    if (bitsPerSecond >= 1e6)
        return QStringLiteral("%1 Mbit/s").arg(bitsPerSecond / 1e6, 0, 'f', 1);
    return QStringLiteral("%1 kbit/s").arg(bitsPerSecond / 1e3, 0, 'f', 0);
}

QString amount(qint64 bytes)
{
    if (bytes >= qint64(1) << 30)
        return QStringLiteral("%1 GB").arg(double(bytes) / (qint64(1) << 30), 0, 'f', 2);
    return QStringLiteral("%1 MB").arg(double(bytes) / (1 << 20), 0, 'f', 1);
}

}

IperfTest *IperfTest::start(const Settings &settings)
{
    auto *thread = new QThread;
    auto *test = new IperfTest(settings);
    test->moveToThread(thread);
    connect(thread, &QThread::started, test, &IperfTest::begin);
    // The test deletes itself when it is over; its thread then ends.
    connect(test, &QObject::destroyed, thread, &QThread::quit);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    return test;
}

IperfTest::IperfTest(const Settings &settings) : m_settings(settings)
{
    m_settings.streams = qBound(1, m_settings.streams, 64);
    m_settings.seconds = qBound(1, m_settings.seconds, 3600);
}

void IperfTest::stop()
{
    QMetaObject::invokeMethod(this, [this] {
        if (m_finished)
            return;
        if (m_control && m_control->state() == QAbstractSocket::ConnectedState) {
            const char state = m_settings.server ? char(ServerTerminate) : char(ClientTerminate);
            m_control->write(&state, 1);
            m_control->flush();
        }
        done(QStringLiteral("Stopped."));
    }, Qt::QueuedConnection);
}

void IperfTest::begin()
{
    m_block = QByteArray(BlockSize, '\0');
    m_ticker = new QTimer(this);
    m_ticker->setInterval(1000);
    connect(m_ticker, &QTimer::timeout, this, &IperfTest::tick);
    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);

    if (m_settings.server) {
        m_longestTest = m_settings.seconds;
        m_guard = new QTimer(this);
        m_guard->setSingleShot(true);
        connect(m_guard, &QTimer::timeout, this, [this] {
            if (!m_control)
                return;
            if (m_stage >= 3) {
                emit line(QStringLiteral("The test ran past %1 s, the longest this server allows: "
                                         "stopped.").arg(m_longestTest));
                const char state = char(ServerTerminate);
                m_control->write(&state, 1);
                m_control->flush();
            } else {
                emit line(QStringLiteral("The client did not start a test in time."));
            }
            cleanUp();
        });
        QHostAddress address;
        QString error;
        if (!ListenAddress::resolve(m_settings.listen, &address, &error))
            return fail(error);
        m_listener = new QTcpServer(this);
        if (!m_listener->listen(address, m_settings.port))
            return fail(QStringLiteral("Cannot listen on port %1: %2")
                            .arg(m_settings.port)
                            .arg(m_listener->errorString()));
        connect(m_listener, &QTcpServer::newConnection, this, &IperfTest::acceptControl);
        emit line(QStringLiteral("Listening on port %1 (%2) for iperf3 clients (iperf3 -c <this "
                                 "computer> -p %1), tests up to %3 s…")
                      .arg(m_settings.port)
                      .arg(ListenAddress::describe(m_settings.listen))
                      .arg(m_longestTest));
        return;
    }

    // The test's cookie: 36 of iperf3's characters and a NUL.
    static const char characters[] = "abcdefghijklmnopqrstuvwxyz234567";
    for (int i = 0; i < CookieSize - 1; ++i)
        m_cookie.append(characters[QRandomGenerator::global()->bounded(32)]);
    m_cookie.append('\0');
    m_control = new QTcpSocket(this);
    connect(m_control, &QTcpSocket::connected, this, [this] {
        emit line(QStringLiteral("Connected to %1 port %2.").arg(m_settings.host).arg(m_settings.port));
        m_control->write(m_cookie);
    });
    connect(m_control, &QTcpSocket::readyRead, this, &IperfTest::readControl);
    connect(m_control, &QTcpSocket::errorOccurred, this, [this] {
        if (!m_finished)
            fail(QStringLiteral("%1 (is iperf3 -s running there?)").arg(m_control->errorString()));
    });
    emit line(QStringLiteral("Connecting to %1 port %2…").arg(m_settings.host).arg(m_settings.port));
    m_control->connectToHost(m_settings.host, m_settings.port);
}

// ---- Client ----

bool IperfTest::takeControlInput()
{
    // One JSON message (with its length) and a few state bytes at most.
    const qint64 room = qint64(MaximumJson) + 64 - m_controlBuffer.size();
    if (m_control->bytesAvailable() > room) {
        protocolError(QStringLiteral("The other side sent more than iperf3 does."));
        return false;
    }
    m_controlBuffer += m_control->readAll();
    return true;
}

void IperfTest::protocolError(const QString &message)
{
    if (!m_settings.server)
        return fail(message);
    emit line(QStringLiteral("Client dropped: %1").arg(message));
    cleanUp();
}

void IperfTest::readControl()
{
    if (!takeControlInput())
        return;
    while (!m_finished && !m_controlBuffer.isEmpty()) {
        if (m_stage == 4) {
            // The server's results, after ours.
            QByteArray json;
            if (!readJson(&json))
                return;
            const auto results = QJsonDocument::fromJson(json).object();
            qint64 peer = 0;
            for (const auto &stream : results.value(QStringLiteral("streams")).toArray())
                peer += stream.toObject().value(QStringLiteral("bytes")).toVariant().toLongLong();
            emit line(QStringLiteral("%1 counted %2.")
                          .arg(m_sending ? QStringLiteral("The receiver") : QStringLiteral("The sender"),
                               amount(peer)));
            m_stage = 5;
            continue;
        }
        const qint8 state = qint8(m_controlBuffer.at(0));
        m_controlBuffer.remove(0, 1);
        handleState(state);
    }
}

void IperfTest::handleState(qint8 state)
{
    switch (state) {
    case ParamExchange: {
        QJsonObject parameters{{QStringLiteral("tcp"), true},
                               {QStringLiteral("omit"), 0},
                               {QStringLiteral("time"), m_settings.seconds},
                               {QStringLiteral("num"), 0},
                               {QStringLiteral("blockcount"), 0},
                               {QStringLiteral("parallel"), m_settings.streams},
                               {QStringLiteral("len"), BlockSize},
                               {QStringLiteral("pacing_timer"), 1000},
                               {QStringLiteral("client_version"), QStringLiteral("3.22")}};
        if (m_settings.reverse)
            parameters.insert(QStringLiteral("reverse"), true);
        sendJson(QJsonDocument(parameters).toJson(QJsonDocument::Compact));
        break;
    }
    case CreateStreams:
        openStreams();
        break;
    case TestStart:
        break;
    case TestRunning:
        m_sending = !m_settings.reverse;
        startRunning();
        break;
    case ExchangeResults:
        sendJson(resultsJson());
        m_stage = 4;
        break;
    case DisplayResults: {
        const char state = char(IperfDone);
        m_control->write(&state, 1);
        m_control->flush();
        const double seconds = m_testSeconds > 0 ? m_testSeconds : m_settings.seconds;
        done(QStringLiteral("%1 %2 in %3 s: %4 (%5 stream%6)")
                 .arg(m_sending ? QStringLiteral("Sent") : QStringLiteral("Received"), amount(m_bytes))
                 .arg(seconds, 0, 'f', 1)
                 .arg(rate(m_bytes * 8.0 / seconds))
                 .arg(m_settings.streams)
                 .arg(m_settings.streams == 1 ? QString() : QStringLiteral("s")));
        break;
    }
    case ServerTerminate:
        fail(QStringLiteral("The server ended the test."));
        break;
    case AccessDenied:
        fail(QStringLiteral("The server is busy with another test (try again shortly)."));
        break;
    case ServerError:
        fail(QStringLiteral("The server reported an error."));
        break;
    default:
        break;
    }
}

void IperfTest::openStreams()
{
    for (int i = 0; i < m_settings.streams; ++i) {
        auto *socket = new QTcpSocket(this);
        socket->setProperty("bytes", qint64(0));
        connect(socket, &QTcpSocket::connected, this, [this, socket] { socket->write(m_cookie); });
        m_streams.append(socket);
        socket->connectToHost(m_settings.host, m_settings.port);
    }
}

// ---- Server ----

void IperfTest::acceptControl()
{
    while (m_listener->hasPendingConnections()) {
        QTcpSocket *socket = m_listener->nextPendingConnection();
        socket->setParent(this);
        if (!m_control) {
            m_control = socket;
            m_stage = 0;
            m_controlBuffer.clear();
            m_guard->start(HandshakeMs);
            emit line(QStringLiteral("Test from %1.")
                          .arg(socket->peerAddress().toString().remove(QStringLiteral("::ffff:"))));
            connect(socket, &QTcpSocket::readyRead, this, &IperfTest::readServerControl);
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                if (socket == m_control && m_stage < 5) {
                    emit line(QStringLiteral("The client left before the end."));
                    cleanUp();
                }
            });
        } else if (m_stage == 2
                   && findChildren<QTcpSocket *>(Qt::FindDirectChildrenOnly).size()
                          <= m_settings.streams + MaximumPendingStreams + 1) {
            // A data connection of the current test: its cookie first,
            // within the handshake time.
            QTimer::singleShot(HandshakeMs, socket, [socket] {
                if (!socket->property("accepted").toBool())
                    socket->abort();
            });
            socket->setProperty("bytes", qint64(0));
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                if (socket->property("accepted").toBool())
                    return; // counted by the running test
                if (socket->bytesAvailable() < CookieSize)
                    return;
                if (socket->read(CookieSize) != m_cookie) {
                    socket->abort();
                    return;
                }
                socket->setProperty("accepted", true);
                m_streams.append(socket);
                if (m_streams.size() == m_settings.streams) {
                    const char states[] = {char(TestStart), char(TestRunning)};
                    m_control->write(states, 2);
                    m_sending = m_settings.reverse; // the client receives
                    startRunning();
                }
            });
        } else {
            // Busy: as iperf3 does.
            const char state = char(AccessDenied);
            socket->write(&state, 1);
            socket->disconnectFromHost();
        }
    }
}

void IperfTest::readServerControl()
{
    if (!takeControlInput())
        return;
    while (!m_controlBuffer.isEmpty()) {
        if (m_stage == 0) {
            if (m_controlBuffer.size() < CookieSize)
                return;
            m_cookie = m_controlBuffer.left(CookieSize);
            m_controlBuffer.remove(0, CookieSize);
            const char state = char(ParamExchange);
            m_control->write(&state, 1);
            m_stage = 1;
        } else if (m_stage == 1) {
            QByteArray json;
            if (!readJson(&json))
                return;
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(json, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return protocolError(QStringLiteral("its test parameters are not valid JSON."));
            const auto parameters = document.object();
            const auto refuse = [this](const QString &reason) {
                emit line(QStringLiteral("Refused: %1").arg(reason));
                const char state = char(AccessDenied);
                m_control->write(&state, 1);
                m_control->flush();
                cleanUp();
            };
            if (!parameters.value(QStringLiteral("tcp")).toBool())
                return refuse(QStringLiteral("only TCP tests are supported."));
            const auto number = [&parameters](const char *key, int fallback) {
                const auto value = parameters.value(QLatin1String(key));
                return value.isUndefined() ? fallback : value.isDouble() ? value.toInt(-1) : -1;
            };
            const int time = number("time", 10);
            const int parallel = number("parallel", 1);
            const int length = number("len", BlockSize);
            if (time < 0 || parallel < 1 || parallel > 64 || length < 1)
                return protocolError(QStringLiteral("its test parameters are out of range."));
            if (time > m_longestTest)
                return refuse(QStringLiteral("a %1 s test is longer than the %2 s this server allows.")
                                  .arg(time).arg(m_longestTest));
            m_settings.seconds = qMax(1, time);
            m_settings.streams = parallel;
            m_settings.reverse = parameters.value(QStringLiteral("reverse")).toBool();
            // The test may run its time plus a little for the streams.
            m_guard->start((m_settings.seconds + 15) * 1000);
            m_block = QByteArray(qBound(1024, length, 1 << 20), '\0');
            emit line(QStringLiteral("%1 s, %2 stream(s), %3.")
                          .arg(m_settings.seconds)
                          .arg(m_settings.streams)
                          .arg(m_settings.reverse ? QStringLiteral("this computer sends")
                                                  : QStringLiteral("this computer receives")));
            const char state = char(CreateStreams);
            m_control->write(&state, 1);
            m_stage = 2;
        } else if (m_stage == 2 || m_stage == 3) {
            const qint8 state = qint8(m_controlBuffer.at(0));
            m_controlBuffer.remove(0, 1);
            if (state == TestEnd) {
                m_running = false;
                m_ticker->stop();
                m_testSeconds = m_clock.elapsed() / 1000.0;
                const char reply = char(ExchangeResults);
                m_control->write(&reply, 1);
                m_stage = 4;
            } else if (state == ClientTerminate) {
                emit line(QStringLiteral("The client stopped the test."));
                return cleanUp();
            }
        } else if (m_stage == 4) {
            QByteArray json;
            if (!readJson(&json))
                return;
            sendJson(resultsJson());
            const char state = char(DisplayResults);
            m_control->write(&state, 1);
            m_stage = 5;
        } else {
            const qint8 state = qint8(m_controlBuffer.at(0));
            m_controlBuffer.remove(0, 1);
            if (state == IperfDone) {
                const double seconds = m_testSeconds > 0 ? m_testSeconds : 1;
                emit line(QStringLiteral("%1 %2 in %3 s: %4.")
                              .arg(m_sending ? QStringLiteral("Sent") : QStringLiteral("Received"),
                                   amount(m_bytes))
                              .arg(seconds, 0, 'f', 1)
                              .arg(rate(m_bytes * 8.0 / seconds)));
                emit line(QStringLiteral("Waiting for the next test…"));
                return cleanUp();
            }
        }
    }
}

// ---- Both ----

void IperfTest::startRunning()
{
    m_running = true;
    m_stage = 3;
    m_bytes = m_lastBytes = 0;
    m_lastTick = 0;
    m_clock.start();
    m_ticker->start();
    for (auto *socket : std::as_const(m_streams)) {
        if (m_sending) {
            connect(socket, &QTcpSocket::bytesWritten, this, [this, socket](qint64 written) {
                m_bytes += written;
                socket->setProperty("bytes", socket->property("bytes").toLongLong() + written);
                pumpSender(socket);
            });
            pumpSender(socket);
        } else {
            const auto drain = [this, socket] {
                const qint64 read = socket->skip(socket->bytesAvailable());
                if (read > 0 && m_running) {
                    m_bytes += read;
                    socket->setProperty("bytes", socket->property("bytes").toLongLong() + read);
                }
            };
            connect(socket, &QTcpSocket::readyRead, this, drain);
            drain();
        }
    }
    // The client ends the test after its time.
    if (!m_settings.server) {
        disconnect(m_deadline, nullptr, this, nullptr);
        connect(m_deadline, &QTimer::timeout, this, [this] {
            m_running = false;
            m_ticker->stop();
            m_testSeconds = m_clock.elapsed() / 1000.0;
            tick();
            const char state = char(TestEnd);
            m_control->write(&state, 1);
        });
        m_deadline->start(m_settings.seconds * 1000);
    }
}

void IperfTest::pumpSender(QTcpSocket *socket)
{
    while (m_running && socket->bytesToWrite() < 4 * qint64(m_block.size()))
        socket->write(m_block);
}

void IperfTest::tick()
{
    const double now = m_clock.elapsed() / 1000.0;
    const double span = now - m_lastTick;
    if (span < 0.05)
        return;
    const double bits = (m_bytes - m_lastBytes) * 8.0 / span;
    emit line(QStringLiteral("%1–%2 s   %3   %4")
                  .arg(m_lastTick, 5, 'f', 1)
                  .arg(now, 5, 'f', 1)
                  .arg(amount(m_bytes - m_lastBytes), 9)
                  .arg(rate(bits)));
    emit interval(now, bits);
    m_lastTick = now;
    m_lastBytes = m_bytes;
}

void IperfTest::sendJson(const QByteArray &json)
{
    char size[4];
    qToBigEndian(quint32(json.size()), size);
    m_control->write(size, 4);
    m_control->write(json);
}

bool IperfTest::readJson(QByteArray *json)
{
    if (m_controlBuffer.size() < 4)
        return false;
    const quint32 size = qFromBigEndian<quint32>(m_controlBuffer.constData());
    if (size == 0 || size > MaximumJson) {
        protocolError(QStringLiteral("The other side sent something that is not iperf3."));
        return false;
    }
    if (quint32(m_controlBuffer.size()) < 4 + size)
        return false;
    *json = m_controlBuffer.mid(4, qsizetype(size));
    m_controlBuffer.remove(0, qsizetype(4 + size));
    return true;
}

QByteArray IperfTest::resultsJson() const
{
    QJsonArray streams;
    const double seconds = m_testSeconds > 0 ? m_testSeconds : m_settings.seconds;
    for (int i = 0; i < m_streams.size(); ++i) {
        streams.append(QJsonObject{{QStringLiteral("id"), streamId(i)},
                                   {QStringLiteral("bytes"), m_streams.at(i)->property("bytes").toLongLong()},
                                   {QStringLiteral("retransmits"), -1},
                                   {QStringLiteral("jitter"), 0},
                                   {QStringLiteral("errors"), 0},
                                   {QStringLiteral("omitted_errors"), 0},
                                   {QStringLiteral("packets"), 0},
                                   {QStringLiteral("omitted_packets"), 0},
                                   {QStringLiteral("start_time"), 0},
                                   {QStringLiteral("end_time"), seconds}});
    }
    const QJsonObject results{{QStringLiteral("cpu_util_total"), 0},
                              {QStringLiteral("cpu_util_user"), 0},
                              {QStringLiteral("cpu_util_system"), 0},
                              {QStringLiteral("sender_has_retransmits"), -1},
                              {QStringLiteral("streams"), streams}};
    return QJsonDocument(results).toJson(QJsonDocument::Compact);
}

void IperfTest::fail(const QString &message)
{
    if (m_finished)
        return;
    m_finished = true;
    m_running = false;
    emit finished(message, true);
    deleteLater();
}

void IperfTest::done(const QString &summary)
{
    if (m_finished)
        return;
    m_finished = true;
    m_running = false;
    emit finished(summary, false);
    deleteLater();
}

// The server forgets a test and waits for the next.
void IperfTest::cleanUp()
{
    m_running = false;
    m_ticker->stop();
    if (m_guard)
        m_guard->stop();
    for (auto *socket : std::as_const(m_streams)) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_streams.clear();
    if (m_control) {
        m_control->disconnect(this);
        m_control->disconnectFromHost();
        m_control->deleteLater();
        m_control = nullptr;
    }
    m_controlBuffer.clear();
    m_bytes = m_lastBytes = 0;
    m_testSeconds = 0;
    m_stage = 0;
}
