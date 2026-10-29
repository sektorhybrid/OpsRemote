// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#pragma once

#include "rshoptions.h"
#include "serialoptions.h"

#include <QByteArray>
#include <QObject>
#include <QString>

class QSerialPort;
class QTcpSocket;

// A plain byte stream shown in a terminal tab: Telnet or a serial line.
// TerminalPage drives it like an SSH shell (see TerminalPage's stream
// constructor).
class StreamConnection : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void open() = 0;
    virtual void write(const QByteArray &data) = 0;
    virtual void resize(int columns, int rows)
    {
        Q_UNUSED(columns);
        Q_UNUSED(rows);
    }
    virtual void close() = 0;
    // "Telnet 10.0.0.1:23", "Serial /dev/ttyUSB0".
    virtual QString description() const = 0;
    // Whether the terminal says "Connecting…" when it starts (a local
    // shell prints its own greeting).
    virtual bool announcesStart() const { return true; }

signals:
    void connected();
    void dataReceived(const QByteArray &data);
    void disconnected(const QString &reason, bool error);
};

// Telnet (RFC 854) with the options a terminal needs: echo and suppress
// go-ahead from the server, our terminal type (RFC 1091) and window size
// (RFC 1073). Everything else is refused.
class TelnetConnection final : public StreamConnection
{
    Q_OBJECT

public:
    TelnetConnection(const QString &host, quint16 port, const QString &terminalType,
                     QObject *parent = nullptr);

    void open() override;
    void write(const QByteArray &data) override;
    void resize(int columns, int rows) override;
    void close() override;
    QString description() const override;

private:
    void readSocket();
    void handleCommand(quint8 command, quint8 option);
    void handleSubnegotiation(const QByteArray &data);
    void sendCommand(quint8 command, quint8 option);
    void sendWindowSize();

    QString m_host;
    quint16 m_port;
    QByteArray m_terminalType;
    QTcpSocket *m_socket = nullptr;
    int m_columns = 80;
    int m_rows = 24;
    bool m_windowSizeAgreed = false;
    bool m_closing = false;
    // Parser state for IAC sequences that span reads.
    enum class State { Data, Iac, Command, Subnegotiation, SubnegotiationIac };
    State m_state = State::Data;
    quint8 m_command = 0;
    QByteArray m_subnegotiation;
};

// Rlogin (RFC 1282): the user names and terminal type, then a plain byte
// stream; the window size goes as the protocol's control message.
class RloginConnection final : public StreamConnection
{
    Q_OBJECT

public:
    RloginConnection(const QString &host, quint16 port, const QString &remoteUser, const RshOptions &options,
                     const QString &terminalType, QObject *parent = nullptr);

    void open() override;
    void write(const QByteArray &data) override;
    void resize(int columns, int rows) override;
    void close() override;
    QString description() const override;

private:
    void readSocket();
    void sendWindowSize();

    QString m_host;
    quint16 m_port;
    QString m_remoteUser;
    RshOptions m_options;
    QByteArray m_terminalType;
    QTcpSocket *m_socket = nullptr;
    QString m_portNote; // why no reserved port was used
    int m_columns = 80;
    int m_rows = 24;
    bool m_started = false; // the server's first byte came
    bool m_closing = false;
};

// RSH (rshd): runs one command and shows its output. The server has no
// terminal for it, so lines typed are edited and echoed here and sent
// when Enter is pressed; Ctrl+D ends the input, Ctrl+C the connection.
class RshConnection final : public StreamConnection
{
    Q_OBJECT

public:
    RshConnection(const QString &host, quint16 port, const QString &remoteUser, const RshOptions &options,
                  QObject *parent = nullptr);

    void open() override;
    void write(const QByteArray &data) override;
    void close() override;
    QString description() const override;

private:
    void readSocket();

    QString m_host;
    quint16 m_port;
    QString m_remoteUser;
    RshOptions m_options;
    QTcpSocket *m_socket = nullptr;
    QString m_portNote;
    QByteArray m_line; // typed, not yet sent
    bool m_started = false;
    bool m_closing = false;
    bool m_lastWasCr = false;
};

class SerialConnection final : public StreamConnection
{
    Q_OBJECT

public:
    SerialConnection(const QString &portName, const SerialOptions &options,
                     QObject *parent = nullptr);

    void open() override;
    void write(const QByteArray &data) override;
    void close() override;
    QString description() const override;

private:
    QString m_portName;
    SerialOptions m_options;
    QSerialPort *m_port = nullptr;
};
