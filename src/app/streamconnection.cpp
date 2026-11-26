// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "streamconnection.h"
#include "tcpkeepalive.h"

#include <QSerialPort>
#include <QTcpSocket>

#ifdef Q_OS_WIN
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace {

// Telnet commands and options.
constexpr quint8 Se = 240;
constexpr quint8 Sb = 250;
constexpr quint8 Will = 251;
constexpr quint8 Wont = 252;
constexpr quint8 Do = 253;
constexpr quint8 Dont = 254;
constexpr quint8 Iac = 255;
constexpr quint8 Echo = 1;
constexpr quint8 SuppressGoAhead = 3;
constexpr quint8 TerminalType = 24;
constexpr quint8 WindowSize = 31;
constexpr quint8 TerminalTypeIs = 0;
constexpr quint8 TerminalTypeSend = 1;

// Rlogin and rsh servers trust a connection only from a reserved port
// (512-1023), which proves an administrator opened it. Windows lets anyone
// bind one; Linux and macOS only an administrator. Returns the socket to
// connect with, or nullptr when a reserved port was required and none could
// be had; note says why any other port was used.
QTcpSocket *sourceSocket(QObject *parent, const QString &mode, QString *note)
{
    if (mode == QStringLiteral("any"))
        return new QTcpSocket(parent);
    for (quint16 port = 1023; port >= 512; --port) {
        auto *socket = new QTcpSocket(parent);
        if (socket->bind(QHostAddress::Any, port))
            return socket;
        const bool denied = socket->error() == QAbstractSocket::SocketAccessError;
        delete socket;
        if (denied)
            break; // not allowed at all: the next port will not be either
    }
    *note = QStringLiteral("This computer allows reserved ports only to an administrator, and many "
                           "servers refuse other ports.");
    // A socket that failed to bind keeps its error: a new one.
    return mode == QStringLiteral("reserved") ? nullptr : new QTcpSocket(parent);
}

// A server that closes at once after a connection from an ordinary port
// most likely wanted a reserved one.
QString refusedReason(const QString &portNote, const QByteArray &message)
{
    // The message may start with the protocol's error byte (1).
    QString reason = QString::fromUtf8(message);
    while (!reason.isEmpty() && reason.front().unicode() < 0x20)
        reason.remove(0, 1);
    reason = reason.trimmed();
    if (reason.isEmpty())
        reason = QStringLiteral("The server refused the connection.");
    if (!portNote.isEmpty())
        reason += QLatin1Char(' ') + portNote;
    return reason;
}

void shutdownSending(QTcpSocket *socket)
{
#ifdef Q_OS_WIN
    ::shutdown(SOCKET(socket->socketDescriptor()), SD_SEND);
#else
    ::shutdown(int(socket->socketDescriptor()), SHUT_WR);
#endif
}

}

// ---- Telnet ----

TelnetConnection::TelnetConnection(const QString &host, quint16 port, const QString &terminalType,
                                   QObject *parent)
    : StreamConnection(parent), m_host(host), m_port(port == 0 ? 23 : port),
      m_terminalType(terminalType.toUpper().toLatin1())
{
}

QString TelnetConnection::description() const
{
    return QStringLiteral("Telnet %1:%2").arg(m_host).arg(m_port);
}

void StreamConnection::watchForDeadNetwork(QTcpSocket *socket)
{
    if (m_deadNetworkSeconds > 0 && !TcpKeepalive::detectDeadNetwork(socket->socketDescriptor(), m_deadNetworkSeconds))
        qInfo("Connection: %s: this system refused keepalive probes; a dead network is noticed later",
              qUtf8Printable(description()));
}

void TelnetConnection::open()
{
    close();
    m_closing = false;
    m_state = State::Data;
    m_windowSizeAgreed = false;
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, [this] { watchForDeadNetwork(m_socket); });
    connect(m_socket, &QTcpSocket::connected, this, &StreamConnection::connected);
    connect(m_socket, &QTcpSocket::readyRead, this, &TelnetConnection::readSocket);
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        if (!m_closing)
            emit disconnected(QStringLiteral("The server closed the connection."), false);
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (m_closing || error == QAbstractSocket::RemoteHostClosedError)
            return;
        m_closing = true;
        emit disconnected(m_socket->errorString(), true);
    });
    m_socket->connectToHost(m_host, m_port);
}

void TelnetConnection::close()
{
    if (!m_socket)
        return;
    m_closing = true;
    m_socket->disconnect(this);
    m_socket->abort();
    m_socket->deleteLater();
    m_socket = nullptr;
}

void TelnetConnection::write(const QByteArray &data)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    QByteArray out;
    out.reserve(data.size() + 4);
    for (const char c : data) {
        out.append(c);
        if (quint8(c) == Iac)
            out.append(char(Iac)); // a literal 255 is doubled
        else if (c == '\r')
            out.append('\0'); // a bare carriage return is CR NUL
    }
    m_socket->write(out);
}

void TelnetConnection::resize(int columns, int rows)
{
    m_columns = columns;
    m_rows = rows;
    if (m_windowSizeAgreed)
        sendWindowSize();
}

void TelnetConnection::readSocket()
{
    QByteArray text;
    for (const char c : m_socket->readAll()) {
        const quint8 byte = quint8(c);
        switch (m_state) {
        case State::Data:
            if (byte == Iac)
                m_state = State::Iac;
            else
                text.append(c);
            break;
        case State::Iac:
            if (byte == Iac) {
                text.append(c);
                m_state = State::Data;
            } else if (byte == Sb) {
                m_subnegotiation.clear();
                m_state = State::Subnegotiation;
            } else if (byte >= Will && byte <= Dont) {
                m_command = byte;
                m_state = State::Command;
            } else {
                m_state = State::Data; // NOP, GA, and the like
            }
            break;
        case State::Command:
            handleCommand(m_command, byte);
            m_state = State::Data;
            break;
        case State::Subnegotiation:
            if (byte == Iac)
                m_state = State::SubnegotiationIac;
            else if (m_subnegotiation.size() < 1024)
                m_subnegotiation.append(c);
            break;
        case State::SubnegotiationIac:
            if (byte == Se) {
                handleSubnegotiation(m_subnegotiation);
                m_state = State::Data;
            } else {
                m_subnegotiation.append(c);
                m_state = State::Subnegotiation;
            }
            break;
        }
    }
    if (!text.isEmpty())
        emit dataReceived(text);
}

void TelnetConnection::handleCommand(quint8 command, quint8 option)
{
    switch (command) {
    case Will:
        // The server echoes and runs without go-ahead: what a terminal wants.
        sendCommand(option == Echo || option == SuppressGoAhead ? Do : Dont, option);
        break;
    case Do:
        if (option == TerminalType || option == SuppressGoAhead) {
            sendCommand(Will, option);
        } else if (option == WindowSize) {
            sendCommand(Will, option);
            m_windowSizeAgreed = true;
            sendWindowSize();
        } else {
            sendCommand(Wont, option);
        }
        break;
    case Wont:
    case Dont:
        if (option == WindowSize)
            m_windowSizeAgreed = false;
        break;
    }
}

void TelnetConnection::handleSubnegotiation(const QByteArray &data)
{
    if (data.size() >= 2 && quint8(data[0]) == TerminalType && quint8(data[1]) == TerminalTypeSend) {
        QByteArray reply;
        reply.append(char(Iac)).append(char(Sb)).append(char(TerminalType)).append(char(TerminalTypeIs));
        reply.append(m_terminalType);
        reply.append(char(Iac)).append(char(Se));
        m_socket->write(reply);
    }
}

void TelnetConnection::sendCommand(quint8 command, quint8 option)
{
    const char bytes[] = {char(Iac), char(command), char(option)};
    m_socket->write(bytes, sizeof bytes);
}

void TelnetConnection::sendWindowSize()
{
    if (!m_socket)
        return;
    QByteArray reply;
    reply.append(char(Iac)).append(char(Sb)).append(char(WindowSize));
    for (const int value : {m_columns, m_rows}) {
        for (const quint8 byte : {quint8(value >> 8), quint8(value & 0xff)}) {
            reply.append(char(byte));
            if (byte == Iac)
                reply.append(char(Iac));
        }
    }
    reply.append(char(Iac)).append(char(Se));
    m_socket->write(reply);
}

// ---- Rlogin and RSH ----

RServiceConnection::RServiceConnection(const QString &host, quint16 port, const QString &remoteUser,
                                       const RshOptions &options, QObject *parent)
    : StreamConnection(parent), m_host(host), m_port(port), m_remoteUser(remoteUser), m_options(options)
{
}

QString RServiceConnection::localUser() const
{
    return m_options.localUser.isEmpty() ? currentUserName() : m_options.localUser;
}

QString RServiceConnection::remoteUser() const
{
    return m_remoteUser.isEmpty() ? localUser() : m_remoteUser;
}

void RServiceConnection::connectWith(const QString &type, const std::function<QByteArray()> &request,
                                     const QString &end)
{
    close();
    m_closing = false;
    m_started = false;
    m_portNote.clear();
    m_socket = sourceSocket(this, m_options.sourcePort, &m_portNote);
    if (!m_socket) {
        m_closing = true;
        emit disconnected(QStringLiteral("No reserved port could be used. %1 Choose \"Any port\" in the "
                                         "session's %2 settings, or run OpsRemote as administrator.")
                              .arg(m_portNote, type),
                          true);
        return;
    }
    connect(m_socket, &QTcpSocket::connected, this, [this, request] {
        watchForDeadNetwork(m_socket);
        m_socket->write(request());
    });
    connect(m_socket, &QTcpSocket::readyRead, this, [this] { readSocket(); });
    connect(m_socket, &QTcpSocket::disconnected, this, [this, end] {
        if (m_closing)
            return;
        m_closing = true;
        emit disconnected(m_started ? end : refusedReason(m_portNote, {}), !m_started);
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (m_closing || error == QAbstractSocket::RemoteHostClosedError)
            return;
        m_closing = true;
        emit disconnected(m_socket->errorString(), true);
    });
    m_socket->connectToHost(m_host, m_port);
}

QByteArray RServiceConnection::readAccepted(bool *first)
{
    QByteArray data = m_socket->readAll();
    if (first)
        *first = false;
    if (!m_started) {
        if (data.isEmpty())
            return {};
        if (data.front() != '\0') {
            m_closing = true;
            emit disconnected(refusedReason(m_portNote, data), true);
            close();
            return {};
        }
        data.remove(0, 1);
        m_started = true;
        if (first)
            *first = true;
        emit connected();
    }
    return data;
}

void RServiceConnection::close()
{
    if (!m_socket)
        return;
    m_closing = true;
    m_socket->disconnect(this);
    m_socket->abort();
    m_socket->deleteLater();
    m_socket = nullptr;
}

// ---- Rlogin ----

RloginConnection::RloginConnection(const QString &host, quint16 port, const QString &remoteUser,
                                   const RshOptions &options, const QString &terminalType, QObject *parent)
    : RServiceConnection(host, port == 0 ? 513 : port, remoteUser, options, parent),
      m_terminalType(terminalType.toLower().toLatin1())
{
}

void RloginConnection::open()
{
    connectWith(QStringLiteral("Rlogin"), [this] {
        // Client user, server user, terminal type/speed, each ending in NUL.
        QByteArray hello(1, '\0');
        hello += localUser().toUtf8() + '\0';
        hello += remoteUser().toUtf8() + '\0';
        hello += m_terminalType + "/38400" + '\0';
        return hello;
    }, QStringLiteral("The server closed the connection."));
}

void RloginConnection::readSocket()
{
    bool first = false;
    const QByteArray data = readAccepted(&first);
    if (first)
        sendWindowSize();
    if (!data.isEmpty())
        emit dataReceived(data);
}

QString RloginConnection::description() const
{
    return QStringLiteral("Rlogin %1:%2").arg(m_host).arg(m_port);
}

void RloginConnection::write(const QByteArray &data)
{
    if (m_socket && m_started)
        m_socket->write(data);
}

void RloginConnection::resize(int columns, int rows)
{
    m_columns = columns;
    m_rows = rows;
    if (m_started)
        sendWindowSize();
}

void RloginConnection::sendWindowSize()
{
    // The control message: two 0xff, "ss", then rows, columns, and pixel
    // sizes (unknown: 0), each 16 bits, most significant byte first.
    QByteArray message("\xff\xffss", 4);
    for (const int value : {m_rows, m_columns, 0, 0}) {
        message.append(char((value >> 8) & 0xff));
        message.append(char(value & 0xff));
    }
    m_socket->write(message);
}

// ---- RSH ----

RshConnection::RshConnection(const QString &host, quint16 port, const QString &remoteUser,
                             const RshOptions &options, QObject *parent)
    : RServiceConnection(host, port == 0 ? 514 : port, remoteUser, options, parent)
{
}

void RshConnection::open()
{
    close();
    m_line.clear();
    if (m_options.command.trimmed().isEmpty()) {
        m_closing = true;
        emit disconnected(QStringLiteral("This RSH session has no command. Set one in the session's settings, "
                                         "or use an Rlogin session for a shell."),
                          true);
        return;
    }
    connectWith(QStringLiteral("RSH"), [this] {
        // No separate error channel ("0"): errors come with the output.
        QByteArray request("0", 1);
        request += '\0';
        request += localUser().toUtf8() + '\0';
        request += remoteUser().toUtf8() + '\0';
        request += m_options.command.toUtf8() + '\0';
        return request;
    }, QStringLiteral("The command finished."));
}

void RshConnection::readSocket()
{
    const QByteArray data = readAccepted();
    // No terminal on the server: bare line feeds become new lines here.
    QByteArray shown;
    shown.reserve(data.size() + data.size() / 16);
    for (const char c : data) {
        if (c == '\n' && !m_lastWasCr)
            shown.append('\r');
        shown.append(c);
        m_lastWasCr = c == '\r';
    }
    if (!shown.isEmpty())
        emit dataReceived(shown);
}

QString RshConnection::description() const
{
    return QStringLiteral("RSH %1:%2").arg(m_host).arg(m_port);
}

void RshConnection::write(const QByteArray &data)
{
    if (!m_socket || !m_started)
        return;
    QByteArray echo;
    for (const char c : data) {
        switch (c) {
        case '\r':
        case '\n':
            m_socket->write(m_line + '\n');
            m_line.clear();
            echo += "\r\n";
            break;
        case '\x7f':
        case '\b':
            if (!m_line.isEmpty()) {
                // A whole UTF-8 character.
                do
                    m_line.chop(1);
                while (!m_line.isEmpty() && (quint8(m_line.back()) & 0xc0) == 0x80);
                echo += "\b \b";
            }
            break;
        case '\x04': // Ctrl+D: the end of the input
            if (!m_line.isEmpty())
                m_socket->write(m_line);
            m_line.clear();
            m_socket->flush();
            shutdownSending(m_socket);
            echo += "^D\r\n";
            break;
        case '\x03': // Ctrl+C: rsh has no way to send a signal here, so stop
            echo += "^C\r\n";
            emit dataReceived(echo);
            m_closing = true;
            close();
            emit disconnected(QStringLiteral("Stopped."), false);
            return;
        default:
            if (quint8(c) >= 0x20 || c == '\t') {
                m_line.append(c);
                echo.append(c);
            }
        }
    }
    if (!echo.isEmpty())
        emit dataReceived(echo);
}

// ---- Serial ----

SerialConnection::SerialConnection(const QString &portName, const SerialOptions &options,
                                   QObject *parent)
    : StreamConnection(parent), m_portName(portName), m_options(options)
{
}

QString SerialConnection::description() const
{
    return QStringLiteral("Serial %1, %2 baud").arg(m_portName).arg(m_options.baudRate);
}

void SerialConnection::open()
{
    close();
    m_port = new QSerialPort(m_portName, this);
    m_port->setBaudRate(m_options.baudRate);
    m_port->setDataBits(QSerialPort::DataBits(m_options.dataBits));
    m_port->setParity(m_options.parity == QStringLiteral("even")    ? QSerialPort::EvenParity
                      : m_options.parity == QStringLiteral("odd")   ? QSerialPort::OddParity
                      : m_options.parity == QStringLiteral("mark")  ? QSerialPort::MarkParity
                      : m_options.parity == QStringLiteral("space") ? QSerialPort::SpaceParity
                                                                    : QSerialPort::NoParity);
    m_port->setStopBits(m_options.stopBits == QStringLiteral("2")     ? QSerialPort::TwoStop
                        : m_options.stopBits == QStringLiteral("1.5") ? QSerialPort::OneAndHalfStop
                                                                      : QSerialPort::OneStop);
    m_port->setFlowControl(m_options.flowControl == QStringLiteral("hardware")
                               ? QSerialPort::HardwareControl
                           : m_options.flowControl == QStringLiteral("software")
                               ? QSerialPort::SoftwareControl
                               : QSerialPort::NoFlowControl);
    connect(m_port, &QSerialPort::readyRead, this, [this] { emit dataReceived(m_port->readAll()); });
    if (!m_port->open(QIODevice::ReadWrite)) {
        const QString reason = m_port->errorString();
        close();
        emit disconnected(QStringLiteral("Cannot open %1: %2").arg(m_portName, reason), true);
        return;
    }
    connect(m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        // A USB adapter pulled out, for example.
        if (error == QSerialPort::NoError || error == QSerialPort::TimeoutError || !m_port)
            return;
        const QString reason = m_port->errorString();
        close();
        emit disconnected(reason, true);
    });
    emit connected();
}

void SerialConnection::write(const QByteArray &data)
{
    if (m_port && m_port->isOpen())
        m_port->write(data);
}

void SerialConnection::close()
{
    if (!m_port)
        return;
    m_port->disconnect(this);
    m_port->close();
    m_port->deleteLater();
    m_port = nullptr;
}
