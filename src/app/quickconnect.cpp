// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "quickconnect.h"
#include "appdirs.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <yaml-cpp/yaml.h>

namespace {

constexpr int MaximumHistory = 20;

QString historyFilePath()
{
    return QDir(portableFilePath(QStringLiteral("settings")))
        .filePath(QStringLiteral("quick-connect.yaml"));
}

void fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

}

namespace QuickConnect {

std::optional<SessionDefinition> parse(const QString &target, QString *error)
{
    QString rest = target.trimmed();
    if (rest.isEmpty()) {
        fail(error, QStringLiteral("Type a host, for example user@server or rdp://desktop."));
        return std::nullopt;
    }
    SessionDefinition session;
    session.protocol = QStringLiteral("SSH");
    if (const int scheme = rest.indexOf(QStringLiteral("://")); scheme > 0) {
        const QString name = rest.left(scheme).toLower();
        if (name == QStringLiteral("ssh"))
            session.protocol = QStringLiteral("SSH");
        else if (name == QStringLiteral("rdp"))
            session.protocol = QStringLiteral("RDP");
        else if (name == QStringLiteral("vnc"))
            session.protocol = QStringLiteral("VNC");
        else if (name == QStringLiteral("telnet"))
            session.protocol = QStringLiteral("Telnet");
        else if (name == QStringLiteral("mosh"))
            session.protocol = QStringLiteral("Mosh");
        else if (name == QStringLiteral("serial"))
            session.protocol = QStringLiteral("Serial");
        else if (name == QStringLiteral("sftp"))
            session.protocol = QStringLiteral("SFTP");
        else if (name == QStringLiteral("scp"))
            session.protocol = QStringLiteral("SCP");
        else if (name == QStringLiteral("rlogin"))
            session.protocol = QStringLiteral("Rlogin");
        else if (name == QStringLiteral("rsh"))
            session.protocol = QStringLiteral("RSH");
        else if (name == QStringLiteral("xdmcp"))
            session.protocol = QStringLiteral("XDMCP");
        else if (name == QStringLiteral("ftp") || name == QStringLiteral("ftps")
                 || name == QStringLiteral("ftpes")) {
            session.protocol = QStringLiteral("FTP");
            // ftps: implicit TLS (as curl reads it); ftpes: explicit TLS.
            if (name == QStringLiteral("ftps"))
                session.ftp.security = QStringLiteral("implicit");
            else if (name == QStringLiteral("ftpes"))
                session.ftp.security = QStringLiteral("explicit");
        } else {
            fail(error, QStringLiteral("Unknown protocol \"%1\" (use ssh, telnet, mosh, serial, "
                                       "rdp, vnc, sftp, scp, ftp, ftps or ftpes).").arg(name));
            return std::nullopt;
        }
        rest = rest.mid(scheme + 3);
    }
    // serial://COM3, serial:///dev/ttyUSB0, with an optional :baud.
    if (session.protocol == QStringLiteral("Serial")) {
        static const QRegularExpression baud(QStringLiteral(":(\\d+)$"));
        if (const auto match = baud.match(rest); match.hasMatch()) {
            session.serial.baudRate = match.captured(1).toInt();
            rest.chop(match.capturedLength());
        }
        if (rest.isEmpty()) {
            fail(error, QStringLiteral("Name the serial port, for example serial://COM3:9600."));
            return std::nullopt;
        }
        session.host = rest;
        session.id = QStringLiteral("quick:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        session.name = QStringLiteral("%1 (%2 baud)").arg(rest).arg(session.serial.baudRate);
        return session;
    }
    // An FTP URL's folder is not used: the file browser starts at home.
    if (session.protocol == QStringLiteral("FTP") || session.protocol == QStringLiteral("SFTP")
        || session.protocol == QStringLiteral("SCP"))
        rest = rest.section(QLatin1Char('/'), 0, 0);
    // A trailing slash from a pasted URL.
    while (rest.endsWith(QLatin1Char('/')))
        rest.chop(1);
    // The last "@" separates the user, so "user@domain@host" keeps "user@domain".
    if (const int at = rest.lastIndexOf(QLatin1Char('@')); at >= 0) {
        session.username = rest.left(at);
        rest = rest.mid(at + 1);
    }
    QString port;
    if (rest.startsWith(QLatin1Char('['))) {
        const int close = rest.indexOf(QLatin1Char(']'));
        if (close < 0) {
            fail(error, QStringLiteral("An IPv6 address needs a closing \"]\"."));
            return std::nullopt;
        }
        session.host = rest.mid(1, close - 1);
        const QString after = rest.mid(close + 1);
        if (after.startsWith(QLatin1Char(':')))
            port = after.mid(1);
        else if (!after.isEmpty()) {
            fail(error, QStringLiteral("Unexpected \"%1\" after the address.").arg(after));
            return std::nullopt;
        }
    } else if (rest.count(QLatin1Char(':')) == 1) {
        session.host = rest.section(QLatin1Char(':'), 0, 0);
        port = rest.section(QLatin1Char(':'), 1);
    } else {
        // No port, or a bare IPv6 address.
        session.host = rest;
    }
    if (session.host.isEmpty() || session.host.contains(QLatin1Char(' '))) {
        fail(error, QStringLiteral("\"%1\" is not a host name or address.").arg(session.host));
        return std::nullopt;
    }
    if (!port.isEmpty()) {
        bool ok = false;
        const uint number = port.toUInt(&ok);
        if (session.protocol == QStringLiteral("VNC") && ok && number < 100) {
            // "host:1" is VNC display 1, which openSession turns into 5901.
            session.host += QLatin1Char(':') + port;
        } else if (!ok || number == 0 || number > 65535) {
            fail(error, QStringLiteral("\"%1\" is not a port number.").arg(port));
            return std::nullopt;
        } else {
            session.port = quint16(number);
        }
    }
    session.id = QStringLiteral("quick:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.name = session.username.isEmpty() ? session.host
                                              : session.username + QLatin1Char('@') + session.host;
    return session;
}

QStringList history()
{
    QStringList targets;
    try {
        const auto list = YAML::LoadFile(historyFilePath().toStdString())["history"];
        for (const auto &entry : list) {
            const auto target = QString::fromStdString(entry.as<std::string>(""));
            if (!target.isEmpty() && !targets.contains(target))
                targets.append(target);
        }
    } catch (const YAML::Exception &) {
        // No history yet, or an unreadable file: start empty.
    }
    return targets.mid(0, MaximumHistory);
}

void remember(const QString &target)
{
    QStringList targets = history();
    targets.removeAll(target.trimmed());
    targets.prepend(target.trimmed());
    YAML::Node root;
    for (const auto &entry : targets.mid(0, MaximumHistory))
        root["history"].push_back(entry.toStdString());
    QSaveFile file(historyFilePath());
    if (!QDir().mkpath(QFileInfo(file.fileName()).absolutePath()) || !file.open(QIODevice::WriteOnly))
        return;
    YAML::Emitter out;
    out << root;
    file.write(out.c_str());
    file.commit();
}

}
