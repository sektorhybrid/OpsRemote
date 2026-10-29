// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "sessionimport.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>

namespace SessionImport {

namespace {

SessionDefinition newSession(const QString &name, const QString &protocol, const QString &host,
                             int port, const QString &username)
{
    SessionDefinition session;
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.name = name.trimmed().isEmpty() ? host : name.trimmed();
    session.protocol = protocol;
    session.host = host.trimmed();
    session.port = port > 0 && port <= 65535 ? quint16(port) : 0;
    session.username = username.trimmed();
    return session;
}

// Leaves out a protocol's usual port, so the session follows OpsRemote's default.
quint16 unlessDefault(const QString &protocol, int port)
{
    static const QHash<QString, int> defaults{{QStringLiteral("SSH"), 22},
                                              {QStringLiteral("Telnet"), 23},
                                              {QStringLiteral("Rlogin"), 513},
                                              {QStringLiteral("XDMCP"), 177},
                                              {QStringLiteral("RDP"), 3389},
                                              {QStringLiteral("VNC"), 5900}};
    return port == defaults.value(protocol, -1) || port <= 0 || port > 65535 ? 0 : quint16(port);
}

void add(Result &result, const QStringList &folders, SessionDefinition session)
{
    if (session.host.isEmpty()) {
        ++result.skipped;
        return;
    }
    session.port = unlessDefault(session.protocol, session.port);
    result.sessions.append({folders, session});
}

QString readText(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QStringLiteral("Cannot read %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return {};
    }
    const QByteArray data = file.readAll();
    // Windows programs often write their files in the ANSI code page.
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8.decode(data);
    if (utf8.hasError())
        text = QString::fromLatin1(data);
    return text;
}

}

// ---- OpenSSH ----

QString defaultOpenSshConfigPath()
{
    return QDir::home().filePath(QStringLiteral(".ssh/config"));
}

Result fromOpenSshConfig(const QString &path)
{
    Result result;
    const QString text = readText(path, &result.error);
    if (!result.error.isEmpty())
        return result;
    struct Entry
    {
        QStringList names;
        QHash<QString, QString> values; // lower-case keyword -> first value
    };
    QList<Entry> entries;
    bool inMatch = false;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        // "Keyword value" or "Keyword=value".
        static const QRegularExpression split(QStringLiteral("^(\\S+?)\\s*(?:=|\\s)\\s*(.*)$"));
        const auto match = split.match(line);
        if (!match.hasMatch())
            continue;
        const QString keyword = match.captured(1).toLower();
        QString value = match.captured(2).trimmed();
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2);
        if (keyword == QStringLiteral("host")) {
            inMatch = false;
            entries.append({value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts), {}});
        } else if (keyword == QStringLiteral("match")) {
            inMatch = true;
        } else if (!inMatch && !entries.isEmpty() && !entries.last().values.contains(keyword)) {
            entries.last().values.insert(keyword, value); // the first value wins, as in ssh
        }
    }
    for (const auto &entry : std::as_const(entries)) {
        for (const auto &name : entry.names) {
            if (name.contains(QLatin1Char('*')) || name.contains(QLatin1Char('?'))
                || name.startsWith(QLatin1Char('!'))) {
                ++result.skipped;
                continue;
            }
            const QString host = entry.values.value(QStringLiteral("hostname"), name)
                                     .replace(QStringLiteral("%h"), name);
            auto session = newSession(name, QStringLiteral("SSH"), host,
                                      entry.values.value(QStringLiteral("port")).toInt(),
                                      entry.values.value(QStringLiteral("user")));
            // ProxyJump user@host:port (the first hop; OpsRemote chains through
            // the jump host's own settings).
            const QString jump = entry.values.value(QStringLiteral("proxyjump"));
            if (!jump.isEmpty() && jump.compare(QStringLiteral("none"), Qt::CaseInsensitive) != 0)
                session.jumpHost = jump.section(QLatin1Char(','), 0, 0).trimmed();
            QString identity = entry.values.value(QStringLiteral("identityfile"));
            if (identity.startsWith(QStringLiteral("~/")))
                identity = QDir::home().filePath(identity.mid(2));
            session.ssh.identityFile = identity;
            if (const auto agent = entry.values.value(QStringLiteral("forwardagent")); !agent.isEmpty())
                session.ssh.forwardAgent = agent.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0;
            const auto x11 = entry.values.value(QStringLiteral("forwardx11"));
            if (!x11.isEmpty())
                session.ssh.forwardX11 = x11.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0;
            add(result, {}, session);
        }
    }
    // A ProxyJump naming another Host entry uses that imported session.
    for (auto &imported : result.sessions) {
        auto &jump = imported.session.jumpHost;
        for (const auto &other : std::as_const(result.sessions)) {
            if (!jump.isEmpty() && other.session.name == jump) {
                jump = QStringLiteral("session:") + other.session.id;
                break;
            }
        }
    }
    return result;
}

// ---- PuTTY ----

namespace {

void addPuttySession(Result &result, const QString &name, const QHash<QString, QString> &values)
{
    const QString protocol = values.value(QStringLiteral("Protocol"), QStringLiteral("ssh"));
    SessionDefinition session;
    if (protocol == QStringLiteral("ssh") || protocol == QStringLiteral("telnet")
        || protocol == QStringLiteral("rlogin")) {
        session = newSession(name, protocol == QStringLiteral("ssh")      ? QStringLiteral("SSH")
                                   : protocol == QStringLiteral("rlogin") ? QStringLiteral("Rlogin")
                                                                          : QStringLiteral("Telnet"),
                             values.value(QStringLiteral("HostName")),
                             values.value(QStringLiteral("PortNumber")).toInt(),
                             values.value(QStringLiteral("UserName")));
        // A user in the host field: "admin@server".
        if (const int at = session.host.lastIndexOf(QLatin1Char('@')); at > 0) {
            if (session.username.isEmpty())
                session.username = session.host.left(at);
            session.host = session.host.mid(at + 1);
        }
        if (protocol == QStringLiteral("ssh")) {
            if (const auto agent = values.value(QStringLiteral("AgentFwd")); !agent.isEmpty())
                session.ssh.forwardAgent = agent.toInt() != 0;
            if (const auto x11 = values.value(QStringLiteral("X11Forward")); !x11.isEmpty())
                session.ssh.forwardX11 = x11.toInt() != 0;
        }
    } else if (protocol == QStringLiteral("serial")) {
        session = newSession(name, QStringLiteral("Serial"), values.value(QStringLiteral("SerialLine")),
                             0, QString());
        session.serial.baudRate = values.value(QStringLiteral("SerialSpeed"), QStringLiteral("9600")).toInt();
        session.serial.dataBits = values.value(QStringLiteral("SerialDataBits"), QStringLiteral("8")).toInt();
        const int stop = values.value(QStringLiteral("SerialStopHalfbits"), QStringLiteral("2")).toInt();
        session.serial.stopBits = stop == 4 ? QStringLiteral("2") : stop == 3 ? QStringLiteral("1.5")
                                                                               : QStringLiteral("1");
        static const QStringList parities{QStringLiteral("none"), QStringLiteral("odd"),
                                          QStringLiteral("even"), QStringLiteral("mark"),
                                          QStringLiteral("space")};
        session.serial.parity = parities.value(values.value(QStringLiteral("SerialParity")).toInt(),
                                               QStringLiteral("none"));
        static const QStringList flows{QStringLiteral("none"), QStringLiteral("software"),
                                       QStringLiteral("hardware"), QStringLiteral("none")};
        session.serial.flowControl = flows.value(values.value(QStringLiteral("SerialFlowControl"),
                                                              QStringLiteral("1")).toInt(),
                                                 QStringLiteral("none"));
    } else {
        ++result.skipped; // raw, rlogin, supdup
        return;
    }
    // PuTTY has no folders; "Default Settings" is a template, not a host.
    if (name == QStringLiteral("Default Settings"))
        return;
    add(result, {}, session);
}

}

Result fromPutty()
{
    Result result;
#ifdef Q_OS_WIN
    QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions"),
                       QSettings::NativeFormat);
    for (const auto &group : registry.childGroups()) {
        registry.beginGroup(group);
        QHash<QString, QString> values;
        for (const auto &key : registry.childKeys())
            values.insert(key, registry.value(key).toString());
        registry.endGroup();
        addPuttySession(result, QUrl::fromPercentEncoding(group.toLatin1()), values);
    }
    if (registry.childGroups().isEmpty())
        result.error = QStringLiteral("No PuTTY sessions were found in the registry.");
#else
    const QDir folder(QDir::home().filePath(QStringLiteral(".putty/sessions")));
    const auto files = folder.entryList(QDir::Files);
    if (files.isEmpty())
        result.error = QStringLiteral("No PuTTY sessions were found in %1.").arg(folder.path());
    for (const auto &file : files) {
        QString error;
        const QString text = readText(folder.filePath(file), &error);
        QHash<QString, QString> values;
        for (const auto &line : text.split(QLatin1Char('\n'))) {
            const int equals = line.indexOf(QLatin1Char('='));
            if (equals > 0)
                values.insert(line.left(equals), line.mid(equals + 1).trimmed());
        }
        addPuttySession(result, QUrl::fromPercentEncoding(file.toLatin1()), values);
    }
#endif
    return result;
}

// ---- MobaXterm ----

Result fromMobaXterm(const QString &path)
{
    // [Bookmarks], [Bookmarks_1], ...: SubRep=folder\sub, then
    // Name=#icon#type%host%port%user%... where type 0 is SSH, 1 Telnet,
    // 4 RDP, 5 VNC, 8 serial, 12 Mosh.
    Result result;
    const QString text = readText(path, &result.error);
    if (!result.error.isEmpty())
        return result;
    QStringList folders;
    bool inBookmarks = false;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inBookmarks = line.startsWith(QStringLiteral("[Bookmarks"), Qt::CaseInsensitive);
            folders.clear();
            continue;
        }
        const int equals = line.indexOf(QLatin1Char('='));
        if (!inBookmarks || equals <= 0)
            continue;
        const QString key = line.left(equals).trimmed();
        // MobaXterm escapes ";" and "=" in values.
        QString value = line.mid(equals + 1).trimmed();
        value.replace(QStringLiteral("__PTVIRG__"), QStringLiteral(";"))
            .replace(QStringLiteral("__DBLQUO__"), QStringLiteral("\""))
            .replace(QStringLiteral("__PIPE__"), QStringLiteral("|"));
        if (key.compare(QStringLiteral("SubRep"), Qt::CaseInsensitive) == 0) {
            folders = value.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
            continue;
        }
        if (key.compare(QStringLiteral("ImgNum"), Qt::CaseInsensitive) == 0 || !value.startsWith(QLatin1Char('#')))
            continue;
        const QStringList fields = value.section(QLatin1Char('#'), 2).split(QLatin1Char('%'));
        if (fields.size() < 2) {
            ++result.skipped;
            continue;
        }
        static const QHash<int, QString> types{{0, QStringLiteral("SSH")}, {1, QStringLiteral("Telnet")},
                                                {2, QStringLiteral("Rlogin")}, {3, QStringLiteral("XDMCP")},
                                                {4, QStringLiteral("RDP")}, {5, QStringLiteral("VNC")},
                                                {8, QStringLiteral("Serial")}, {12, QStringLiteral("Mosh")}};
        const QString protocol = types.value(fields[0].toInt());
        if (protocol.isEmpty()) {
            ++result.skipped;
            continue;
        }
        if (protocol == QStringLiteral("Serial")) {
            // "COM3  (USB Serial Port)%speed index%..."
            auto session = newSession(key, protocol, fields[1].section(QLatin1Char(' '), 0, 0), 0, QString());
            static const QList<int> speeds{110, 300, 600, 1200, 2400, 4800, 9600, 14400, 19200,
                                           38400, 57600, 115200, 128000, 256000};
            if (fields.size() > 2) {
                const int index = fields[2].toInt();
                session.serial.baudRate = index >= 0 && index < speeds.size() ? speeds[index]
                                                                              : fields[2].toInt();
                if (session.serial.baudRate < 50)
                    session.serial.baudRate = 9600;
            }
            add(result, folders, session);
            continue;
        }
        add(result, folders,
            newSession(key, protocol, fields.value(1), fields.value(2).toInt(), fields.value(3)));
    }
    if (result.sessions.isEmpty() && result.skipped == 0)
        result.error = QStringLiteral("No MobaXterm sessions were found in this file.");
    return result;
}

// ---- mRemoteNG ----

Result fromMRemoteNg(const QString &path)
{
    Result result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot read %1: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }
    QXmlStreamReader xml(&file);
    QStringList folders;
    QList<bool> containerStack; // per open <Node>: whether it is a folder
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("Connections")
            && xml.attributes().value(QStringLiteral("FullFileEncryption")) == QStringLiteral("true")) {
            result.error = QStringLiteral("This mRemoteNG file is fully encrypted. In mRemoteNG, turn "
                                          "off \"Encrypt complete connection file\", save, and import "
                                          "again.");
            return result;
        }
        if (xml.isStartElement() && xml.name() == QStringLiteral("Node")) {
            const auto attributes = xml.attributes();
            const QString name = attributes.value(QStringLiteral("Name")).toString();
            const bool container = attributes.value(QStringLiteral("Type")) == QStringLiteral("Container");
            containerStack.append(container);
            if (container) {
                folders.append(name);
                continue;
            }
            const QString kind = attributes.value(QStringLiteral("Protocol")).toString().toUpper();
            static const QHash<QString, QString> protocols{
                {QStringLiteral("SSH2"), QStringLiteral("SSH")}, {QStringLiteral("SSH1"), QStringLiteral("SSH")},
                {QStringLiteral("TELNET"), QStringLiteral("Telnet")}, {QStringLiteral("RDP"), QStringLiteral("RDP")},
                {QStringLiteral("VNC"), QStringLiteral("VNC")}};
            const QString protocol = protocols.value(kind);
            if (protocol.isEmpty()) {
                ++result.skipped;
                continue;
            }
            QString username = attributes.value(QStringLiteral("Username")).toString();
            const QString domain = attributes.value(QStringLiteral("Domain")).toString();
            if (protocol == QStringLiteral("RDP") && !domain.isEmpty() && !username.isEmpty())
                username = domain + QLatin1Char('\\') + username;
            add(result, folders,
                newSession(name, protocol, attributes.value(QStringLiteral("Hostname")).toString(),
                           attributes.value(QStringLiteral("Port")).toInt(), username));
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("Node") && !containerStack.isEmpty()) {
            if (containerStack.takeLast() && !folders.isEmpty())
                folders.removeLast();
        }
    }
    if (xml.hasError() && result.sessions.isEmpty())
        result.error = QStringLiteral("Not an mRemoteNG connection file: %1").arg(xml.errorString());
    return result;
}

// ---- Remmina ----

QString defaultRemminaFolder()
{
    return QDir::home().filePath(QStringLiteral(".local/share/remmina"));
}

Result fromRemmina(const QString &folder)
{
    Result result;
    const QDir dir(folder);
    const auto files = dir.entryList({QStringLiteral("*.remmina")}, QDir::Files);
    if (files.isEmpty()) {
        result.error = QStringLiteral("No Remmina connections (.remmina files) were found in %1.")
                           .arg(QDir::toNativeSeparators(folder));
        return result;
    }
    for (const auto &name : files) {
        QSettings file(dir.filePath(name), QSettings::IniFormat);
        file.beginGroup(QStringLiteral("remmina"));
        const QString kind = file.value(QStringLiteral("protocol")).toString().toUpper();
        static const QHash<QString, QString> protocols{{QStringLiteral("SSH"), QStringLiteral("SSH")},
                                                       {QStringLiteral("RDP"), QStringLiteral("RDP")},
                                                       {QStringLiteral("VNC"), QStringLiteral("VNC")}};
        const QString protocol = protocols.value(kind);
        if (protocol.isEmpty()) {
            ++result.skipped;
            continue;
        }
        // server = host, host:port, or [v6]:port.
        QString server = file.value(QStringLiteral("server")).toString().trimmed();
        int port = 0;
        static const QRegularExpression bracketed(QStringLiteral("^\\[([^\\]]+)\\](?::(\\d+))?$"));
        if (const auto match = bracketed.match(server); match.hasMatch()) {
            server = match.captured(1);
            port = match.captured(2).toInt();
        } else if (server.count(QLatin1Char(':')) == 1) {
            port = server.section(QLatin1Char(':'), 1).toInt();
            server = server.section(QLatin1Char(':'), 0, 0);
        }
        QString username = file.value(protocol == QStringLiteral("SSH") ? QStringLiteral("ssh_username")
                                                                         : QStringLiteral("username"))
                               .toString();
        if (username.isEmpty())
            username = file.value(QStringLiteral("username")).toString();
        const QString domain = file.value(QStringLiteral("domain")).toString();
        if (protocol == QStringLiteral("RDP") && !domain.isEmpty() && !username.isEmpty())
            username = domain + QLatin1Char('\\') + username;
        const QStringList folders = file.value(QStringLiteral("group")).toString()
                                        .split(QLatin1Char('/'), Qt::SkipEmptyParts);
        add(result, folders,
            newSession(file.value(QStringLiteral("name")).toString(), protocol, server, port, username));
    }
    return result;
}

}
