// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "filetransfertest.h"

#include "ftpclient.h"
#include "localservers.h"
#include "iperf.h"
#include "sshsession.h"
#include "portforward.h"
#include "tunnels.h"
#include "jumphost.h"
#include "runcommand.h"
#include "streamconnection.h"
#include "xdmcp.h"
#include "httprequestpage.h"
#include "sftppanel.h"
#include "terminalpage.h"
#include "terminalappearance.h"

#include <QApplication>
#include <QLabel>
#include <QProcess>
#include <QRegularExpression>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QCoreApplication>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QTcpSocket>
#include <functional>

#include <cstdio>


namespace {

void say(const QString &text)
{
    std::fprintf(stdout, "%s\n", qUtf8Printable(text));
    std::fflush(stdout);
}

// Waits for a signal (or 60 s); false on timeout.
template<typename Sender, typename Signal>
bool waitFor(Sender *sender, Signal signal, int timeoutMs = 60000)
{
    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, [&loop] { loop.exit(1); });
    QObject::connect(sender, signal, &loop, [&loop] { loop.exit(0); });
    return loop.exec() == 0;
}

QMap<QString, QByteArray> checksums(const QString &root)
{
    QMap<QString, QByteArray> sums;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            sums.insert(QDir(root).relativeFilePath(path),
                        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
    }
    return sums;
}

}

int runFileTransferTest(const QString &spec)
{
    const QUrl url(spec);
    const QString scheme = url.scheme().toLower();
    const QUrlQuery query(url);
    RemoteFileSession *session = nullptr;
    if (scheme == QStringLiteral("sftp") || scheme == QStringLiteral("scp")) {
        auto *ssh = new SshSession;
        SshConnectionSettings settings;
        settings.host = url.host();
        settings.port = quint16(url.port(22));
        settings.username = url.userName();
        settings.password = url.password();
        settings.tryPublicKeys = false;
        settings.fileTransfer = query.hasQueryItem(QStringLiteral("auto")) ? QStringLiteral("auto")
                                                                          : scheme;
        // ?only: a file session (no shell), as the SFTP and SCP session types.
        if (query.hasQueryItem(QStringLiteral("only"))) {
            settings.openShell = false;
            settings.filesOnly = true;
        }
        settings.knownHostsPath = QDir::temp().filePath(QStringLiteral("opsremote-test-known_hosts"));
        QObject::connect(ssh, &SshSession::hostKeyVerificationRequired, ssh, [ssh] {
            ssh->answerHostKey(SshHostKeyDecision::AcceptOnce);
        });
        QObject::connect(ssh, &SshSession::inputRequired, ssh,
                         [ssh, password = settings.password](const QString &, const QStringList &prompts,
                                                             const QList<bool> &) {
                             ssh->answerPrompts(QStringList(prompts.size(), password));
                         });
        QObject::connect(ssh, &SshSession::statusMessage, [](const QString &m) { say(QStringLiteral("status: ") + m); });
        QObject::connect(ssh, &SshSession::disconnected, [](const QString &m, bool) { say(QStringLiteral("disconnected: ") + m); });
        ssh->connectToHost(settings);
        session = ssh;
    } else {
        auto *ftp = new FtpSession;
        FtpConnectionSettings settings;
        settings.host = url.host();
        settings.username = url.userName();
        settings.password = url.password();
        settings.security = query.queryItemValue(QStringLiteral("security"));
        if (settings.security.isEmpty())
            settings.security = scheme == QStringLiteral("ftps") ? QStringLiteral("implicit")
                                                                 : QStringLiteral("auto");
        settings.port = quint16(url.port(settings.security == QStringLiteral("implicit") ? 990 : 21));
        settings.passive = query.queryItemValue(QStringLiteral("mode")) != QStringLiteral("active");
        if (query.hasQueryItem(QStringLiteral("listing")))
            settings.listing = query.queryItemValue(QStringLiteral("listing"));
        QObject::connect(ftp, &FtpSession::certificateQuestion, ftp,
                         [ftp](const QString &problems, const QString &fingerprint) {
                             say(QStringLiteral("certificate: %1 (%2)").arg(problems.section('\n', 0, 0), fingerprint));
                             ftp->answerCertificate(FtpCertificateDecision::AcceptOnce);
                         });
        QObject::connect(ftp, &FtpSession::plainTextQuestion, ftp, [ftp](const QString &message) {
            say(QStringLiteral("plain text: ") + message);
            ftp->answerPlainText(true, false);
        });
        QObject::connect(ftp, &FtpSession::loginFailed, [](const QString &m) { say(QStringLiteral("login failed: ") + m); });
        QObject::connect(ftp, &FtpSession::statusMessage, [](const QString &m) { say(QStringLiteral("status: ") + m); });
        QObject::connect(ftp, &FtpSession::disconnected, [](const QString &m, bool) { say(QStringLiteral("disconnected: ") + m); });
        if (qEnvironmentVariableIsSet("OPSREMOTE_TEST_FILES_LOG"))
            QObject::connect(ftp, &FtpSession::logLine, [](const QString &m) { say(QStringLiteral("  ") + m); });
        ftp->connectToHost(settings);
        session = ftp;
    }

    int failures = 0;
    const auto check = [&failures](bool ok, const QString &what) {
        say(QStringLiteral("%1 %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what));
        if (!ok)
            ++failures;
    };

    QString home, lastError, listedPath;
    QList<SftpEntry> listed;
    QObject::connect(session, &RemoteFileSession::sftpReady, [&home](const QString &path) { home = path; });
    QObject::connect(session, &RemoteFileSession::sftpUnavailable, [&lastError](const QString &e) { lastError = e; });
    QObject::connect(session, &RemoteFileSession::fileOperationFinished, [&lastError](const QString &e) { lastError = e; });
    QObject::connect(session, &RemoteFileSession::directoryListed,
                     [&](const QString &path, const QList<SftpEntry> &entries, const QString &error) {
                         listedPath = path;
                         listed = entries;
                         lastError = error;
                     });
    QObject::connect(session, &RemoteFileSession::transferConflict, session,
                     [session] { session->resolveConflict(SftpConflictResolution::Overwrite, true); });
    int transferred = 0;
    QStringList transferErrors;
    QObject::connect(session, &RemoteFileSession::transferFinished,
                     [&](int, bool, const QString &, int files, int, const QStringList &errors, bool) {
                         transferred = files;
                         transferErrors = errors;
                     });

    if (!waitFor(session, &RemoteFileSession::sftpReady, 30000) || home.isEmpty()) {
        check(false, QStringLiteral("connect: %1").arg(lastError));
        return 1;
    }
    check(true, QStringLiteral("connected over %1, home %2").arg(session->fileProtocol(), home));

    // A local tree: a large random file, an empty one, names with spaces and
    // UTF-8, a subfolder.
    QTemporaryDir local;
    const QString source = QDir(local.path()).filePath(QStringLiteral("upload"));
    QDir().mkpath(source + QStringLiteral("/sub folder/deeper"));
    const auto write = [](const QString &path, const QByteArray &data) {
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write(data);
    };
    QByteArray big(3 * 1024 * 1024 + 123, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(big.data()), big.size() / 4);
    write(source + QStringLiteral("/big.bin"), big);
    write(source + QStringLiteral("/empty.txt"), QByteArray());
    write(source + QStringLiteral("/name with spaces ü.txt"), "hello\n");
    write(source + QStringLiteral("/sub folder/deeper/note.txt"), "deep\n");

    const QString base = home + QStringLiteral("/opsremote-test");
    session->removePaths({base});
    waitFor(session, &RemoteFileSession::fileOperationFinished);
    session->makeDirectory(base);
    waitFor(session, &RemoteFileSession::fileOperationFinished);
    check(lastError.isEmpty(), QStringLiteral("mkdir %1 %2").arg(base, lastError));

    session->upload({source}, base);
    check(waitFor(session, &RemoteFileSession::transferFinished, 120000) && transferErrors.isEmpty()
              && transferred == 4,
          QStringLiteral("upload: %1 files %2").arg(transferred).arg(transferErrors.join(QStringLiteral("; "))));

    // Again over the files now there: replaced through their .filepart.
    session->upload({source}, base, SftpConflictResolution::Overwrite);
    check(waitFor(session, &RemoteFileSession::transferFinished, 120000) && transferErrors.isEmpty()
              && transferred == 4,
          QStringLiteral("upload over existing files: %1").arg(transferErrors.join(QStringLiteral("; "))));

    session->listDirectory(base + QStringLiteral("/upload"));
    waitFor(session, &RemoteFileSession::directoryListed);
    QStringList names;
    for (const auto &entry : std::as_const(listed))
        names << QStringLiteral("%1%2(%3)").arg(entry.name, entry.isDirectory ? QStringLiteral("/") : QString())
                     .arg(entry.size);
    names.sort();
    say(QStringLiteral("listing %1: %2").arg(listedPath, names.join(QStringLiteral(", "))));
    check(listed.size() == 4 && names.contains(QStringLiteral("big.bin(%1)").arg(big.size())),
          QStringLiteral("list %1").arg(lastError));

    session->renamePath(base + QStringLiteral("/upload/empty.txt"), base + QStringLiteral("/upload/renamed.txt"));
    waitFor(session, &RemoteFileSession::fileOperationFinished);
    check(lastError.isEmpty(), QStringLiteral("rename %1").arg(lastError));
    session->renamePath(base + QStringLiteral("/upload/renamed.txt"), base + QStringLiteral("/upload/big.bin"));
    waitFor(session, &RemoteFileSession::fileOperationFinished);
    check(!lastError.isEmpty(), QStringLiteral("rename onto an existing file refused: %1").arg(lastError));
    session->renamePath(base + QStringLiteral("/upload/renamed.txt"), base + QStringLiteral("/upload/empty.txt"));
    waitFor(session, &RemoteFileSession::fileOperationFinished);

    const QString target = QDir(local.path()).filePath(QStringLiteral("download"));
    QDir().mkpath(target);
    session->download({base + QStringLiteral("/upload")}, target);
    check(waitFor(session, &RemoteFileSession::transferFinished, 120000) && transferErrors.isEmpty(),
          QStringLiteral("download: %1 files %2").arg(transferred).arg(transferErrors.join(QStringLiteral("; "))));
    check(checksums(source) == checksums(target + QStringLiteral("/upload")), QStringLiteral("checksums match"));
    check(QFileInfo(target + QStringLiteral("/upload/big.bin")).lastModified().toSecsSinceEpoch()
              == QFileInfo(source + QStringLiteral("/big.bin")).lastModified().toSecsSinceEpoch(),
          QStringLiteral("modification time kept"));

    QList<SftpTreeEntry> tree;
    QObject::connect(session, &RemoteFileSession::treeScanned,
                     [&](const QString &, const QList<SftpTreeEntry> &entries, const QString &error) {
                         tree = entries;
                         lastError = error;
                     });
    session->scanTree(base);
    waitFor(session, &RemoteFileSession::treeScanned);
    check(tree.size() == 7, QStringLiteral("scan tree: %1 entries %2").arg(tree.size()).arg(lastError));

    session->removePaths({base});
    waitFor(session, &RemoteFileSession::fileOperationFinished);
    check(lastError.isEmpty(), QStringLiteral("delete tree %1").arg(lastError));
    session->listDirectory(base);
    waitFor(session, &RemoteFileSession::directoryListed);
    check(!lastError.isEmpty(), QStringLiteral("deleted folder is gone: %1").arg(lastError));

    say(failures ? QStringLiteral("%1 FAILED").arg(failures) : QStringLiteral("ALL PASSED"));
    delete session;
    return failures ? 1 : 0;
}

int runLocalServersTest(const QString &folder)
{
    LocalServerSettings settings;
    settings.folder = QStringLiteral("custom");
    settings.customFolder = folder;
    settings.uploads = true;
    settings.listen = QStringLiteral("localhost");
    settings.maxUploadMb = 1;
    HttpFileServer http;
    TftpServer tftp;
    QObject::connect(&http, &HttpFileServer::logged, [](const QString &line) { say(line); });
    QObject::connect(&tftp, &TftpServer::logged, [](const QString &line) { say(line); });
    QString error;
    settings.port = 18080;
    if (!http.start(settings, &error))
        say(QStringLiteral("http: ") + error);
    settings.port = 16969;
    if (!tftp.start(settings, &error))
        say(QStringLiteral("tftp: ") + error);
    IperfTest::Settings iperf;
    iperf.server = true;
    iperf.port = 15201;
    iperf.seconds = 5; // the longest test
    iperf.listen = QStringLiteral("localhost");
    QPointer<IperfTest> test = IperfTest::start(iperf);
    QObject::connect(test, &IperfTest::line, [](const QString &line) { say(QStringLiteral("iperf: ") + line); });
    QEventLoop loop;
    QTimer::singleShot(qMax(5, qEnvironmentVariableIntValue("OPSREMOTE_TEST_SECONDS")) * 1000, &loop, &QEventLoop::quit);
    loop.exec();
    if (test)
        test->stop();
    return 0;
}

int runTunnelTest(const QString &spec)
{
    const QUrl url(spec);
    // A web server here for the forwards that come back to this computer.
    QTemporaryDir folder;
    {
        QFile page(QDir(folder.path()).filePath(QStringLiteral("hello.txt")));
        page.open(QIODevice::WriteOnly);
        page.write("tunnel-ok\n");
    }
    LocalServerSettings web;
    web.folder = QStringLiteral("custom");
    web.customFolder = folder.path();
    web.listen = QStringLiteral("localhost");
    web.port = 18080;
    HttpFileServer http;
    QObject::connect(&http, &HttpFileServer::logged, [](const QString &line) { say(line); });
    QString error;
    if (!http.start(web, &error))
        say(QStringLiteral("http: ") + error);

    SshConnectionSettings settings;
    settings.host = url.host();
    settings.port = quint16(url.port(22));
    settings.username = url.userName();
    settings.password = url.password();
    settings.tryPublicKeys = false;
    settings.openShell = false;
    settings.knownHostsPath = QDir::temp().filePath(QStringLiteral("opsremote-test-known_hosts"));
    const auto make = [](PortForward::Type type, quint16 listen, const QString &host, quint16 port) {
        PortForward forward;
        forward.type = type;
        forward.listenPort = listen;
        forward.targetHost = host;
        forward.targetPort = port;
        return forward;
    };
    settings.forwards = {make(PortForward::Type::Local, 17001, QStringLiteral("localhost"), 2121),
                         make(PortForward::Type::Socket, 17002, QStringLiteral("/tmp/test.sock"), 0),
                         make(PortForward::Type::Dynamic, 17003, {}, 0),
                         make(PortForward::Type::HttpProxy, 17004, {}, 0),
                         make(PortForward::Type::Remote, 17005, QStringLiteral("127.0.0.1"), 18080),
                         make(PortForward::Type::ReverseDynamic, 17006, {}, 0),
                         make(PortForward::Type::Local, 17007, QStringLiteral("localhost"), 8090),
                         make(PortForward::Type::Socket, 17008, QStringLiteral("/tmp/big.sock"), 0)};
    SshSession ssh;
    QObject::connect(&ssh, &SshSession::hostKeyVerificationRequired, &ssh,
                     [&ssh] { ssh.answerHostKey(SshHostKeyDecision::AcceptOnce); });
    QObject::connect(&ssh, &SshSession::inputRequired, &ssh,
                     [&ssh, password = settings.password](const QString &, const QStringList &prompts,
                                                          const QList<bool> &) {
                         ssh.answerPrompts(QStringList(prompts.size(), password));
                     });
    QObject::connect(&ssh, &SshSession::forwardStatus, [](int index, bool listening, const QString &m) {
        say(QStringLiteral("forward %1 %2: %3").arg(index).arg(listening ? "listening" : "FAILED", m));
    });
    QObject::connect(&ssh, &SshSession::forwardConnectionFailed, [](int index, const QString &m) {
        say(QStringLiteral("forward %1 connection failed: %2").arg(index).arg(m));
    });
    QObject::connect(&ssh, &SshSession::forwardStats, [](int index, int open, qint64 in, qint64 out) {
        say(QStringLiteral("stats %1: %2 open, %3 received, %4 sent").arg(index).arg(open).arg(in).arg(out));
    });
    QObject::connect(&ssh, &SshSession::probeFinished, [](int id, bool ok, const QString &m, int ms) {
        say(QStringLiteral("probe %1: %2 %3 (%4 ms)").arg(id).arg(ok ? "reachable" : "NOT reachable", m).arg(ms));
    });
    QObject::connect(&ssh, &SshSession::disconnected, [](const QString &m, bool) { say(QStringLiteral("disconnected: ") + m); });
    QObject::connect(&ssh, &SshSession::connected, &ssh, [&ssh] {
        say(QStringLiteral("connected"));
        ssh.probe(1, QStringLiteral("localhost"), 2121);
        ssh.probe(2, QStringLiteral("localhost"), 9);
        ssh.probe(3, QStringLiteral("/tmp/test.sock"), 0);
        // Added on the open connection, as a tunnel reusing a tab does.
        PortForward extra;
        extra.type = PortForward::Type::Local;
        extra.listenPort = 17010;
        extra.targetHost = QStringLiteral("localhost");
        extra.targetPort = 2122;
        ssh.addForwards({extra}, 1000);
        QTimer::singleShot(20000, &ssh, [&ssh] { ssh.removeForwards(1000, 1); say(QStringLiteral("removed 1000")); });
    });
    ssh.connectToHost(settings);
    QElapsedTimer clock;
    clock.start();
    const qint64 seconds = qMax(5, qEnvironmentVariableIntValue("OPSREMOTE_TEST_SECONDS"));
    while (clock.elapsed() < seconds * 1000)
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 200);
    say(QStringLiteral("test ends after %1 s").arg(clock.elapsed() / 1000));
    return 0;
}

int runTunnelRunnerTest(const QString &spec)
{
    const QUrl url(spec);
    SshConnectionSettings settings;
    settings.host = url.host();
    settings.port = quint16(url.port(22));
    settings.username = url.userName();
    settings.password = url.password();
    settings.tryPublicKeys = false;
    settings.knownHostsPath = QUrlQuery(url).queryItemValue(QStringLiteral("known_hosts"));
    SavedTunnel tunnel;
    tunnel.name = QStringLiteral("test");
    PortForward local;
    local.type = PortForward::Type::Local;
    local.listenPort = 17101;
    local.targetHost = QStringLiteral("localhost");
    local.targetPort = 2121;
    PortForward socket;
    socket.type = PortForward::Type::Socket;
    socket.listenPort = 17102;
    socket.targetHost = QStringLiteral("/tmp/test.sock");
    tunnel.forwards = {local, socket};
    QWidget parent;
    TunnelRunner runner(&parent);
    const auto stateName = [&runner] {
        static const char *names[] = {"Stopped", "Waking", "Connecting", "Running", "Retrying", "Failed"};
        return QString::fromLatin1(names[int(runner.state())]);
    };
    QObject::connect(&runner, &TunnelRunner::changed, [&] {
        static QString last;
        const QString now = QStringLiteral("%1 %2").arg(stateName(), runner.message());
        if (now != last)
            say(QStringLiteral("state: ") + now);
        last = now;
    });
    QObject::connect(&runner, &TunnelRunner::probeFinished, [](int forward, bool ok, const QString &m, int ms) {
        say(QStringLiteral("probe %1: %2 %3 (%4 ms)").arg(forward).arg(ok ? "reachable" : "NOT reachable", m).arg(ms));
    });
    const auto waitUntil = [](const std::function<bool()> &done, int ms) {
        QElapsedTimer clock;
        clock.start();
        while (!done() && clock.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
        return done();
    };
    const auto read = [](quint16 port) {
        QTcpSocket socket;
        socket.connectToHost(QStringLiteral("127.0.0.1"), port);
        QByteArray data;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 3000 && data.size() < 9) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            data += socket.readAll();
        }
        return QString::fromUtf8(data).trimmed().left(20);
    };
    int failures = 0;
    const auto check = [&failures](bool ok, const QString &what) {
        say(QStringLiteral("%1 %2").arg(ok ? "PASS" : "FAIL", what));
        failures += ok ? 0 : 1;
    };

    // 1. Its own connection.
    runner.start(settings, tunnel, nullptr);
    check(waitUntil([&] { return runner.state() == TunnelRunner::State::Running; }, 15000), QStringLiteral("own connection runs"));
    check(read(17101).startsWith(QStringLiteral("220")), QStringLiteral("local forward answers"));
    check(read(17102) == QStringLiteral("socket-ok"), QStringLiteral("socket forward answers"));
    waitUntil([&] { return runner.totalReceived() > 0; }, 3000);
    check(runner.totalReceived() >= 20, QStringLiteral("traffic counted: %1 bytes").arg(runner.totalReceived()));
    bool probed = false;
    QObject::connect(&runner, &TunnelRunner::probeFinished, [&probed] { probed = true; });
    runner.probe(0);
    check(waitUntil([&] { return probed; }, 5000), QStringLiteral("probe answered"));
    runner.stop();
    waitUntil([] { return false; }, 300);
    check(runner.state() == TunnelRunner::State::Stopped && read(17101).isEmpty(), QStringLiteral("stopped and closed"));

    // 2. An open tab's connection, then the tab closes.
    auto *tab = new SshSession;
    auto tabSettings = settings;
    tabSettings.openShell = false;
    bool tabUp = false;
    QObject::connect(tab, &SshSession::connected, [&tabUp] { tabUp = true; });
    tab->connectToHost(tabSettings);
    check(waitUntil([&] { return tabUp; }, 15000), QStringLiteral("tab connected"));
    tunnel.reconnect = true;
    runner.start(settings, tunnel, tab);
    check(waitUntil([&] { return runner.state() == TunnelRunner::State::Running; }, 10000) && runner.usesTab(),
          QStringLiteral("runs over the tab's connection"));
    check(read(17101).startsWith(QStringLiteral("220")), QStringLiteral("forward over the tab answers"));
    delete tab;
    say(QStringLiteral("tab closed"));
    check(waitUntil([&] { return runner.state() == TunnelRunner::State::Running && !runner.usesTab(); }, 30000),
          QStringLiteral("took over with its own connection"));
    for (const auto &state : runner.forwards())
        say(QStringLiteral("  forward: listening %1 %2").arg(state.listening).arg(state.message));
    check(read(17101).startsWith(QStringLiteral("220")), QStringLiteral("forward answers again"));
    runner.stop();
    say(failures ? QStringLiteral("%1 FAILED").arg(failures) : QStringLiteral("ALL PASSED"));
    return failures ? 1 : 0;
}

int runSshConfigTest(const QString &path)
{
    QString problem;
    const auto tunnels = tunnelsFromSshConfig(path, &problem);
    for (const auto &tunnel : tunnels) {
        TunnelServerInfo info;
        info.valid = true;
        QString user, host;
        quint16 port = 0;
        JumpHost::parseAddress(tunnel.server, &user, &host, &port);
        info.user = user;
        info.host = host;
        info.port = port ? port : 22;
        info.jumps = tunnel.via;
        say(QStringLiteral("%1: %2").arg(tunnel.name, tunnelCommand(tunnel, info)));
    }
    say(QStringLiteral("problem: ") + problem);
    return 0;
}


int runRunCommandTest(const QString &spec)
{
    const QUrl url(spec);
    const QUrlQuery query(url);
    const auto settingsFor = [&](int port) {
        SshConnectionSettings settings;
        settings.host = url.host();
        settings.port = quint16(port);
        settings.username = url.userName();
        settings.password = url.password();
        settings.tryPublicKeys = false;
        settings.connectTimeout = 5;
        settings.knownHostsPath = query.queryItemValue(QStringLiteral("known_hosts"));
        return settings;
    };
    const int second = query.queryItemValue(QStringLiteral("second")).toInt();
    const int down = query.queryItemValue(QStringLiteral("down")).toInt();
    const QList<RunCommandWindow::Host> hosts{
        {QStringLiteral("a"), QStringLiteral("first"), QStringLiteral("%1:%2").arg(url.host()).arg(url.port(22))},
        {QStringLiteral("b"), QStringLiteral("second"), QStringLiteral("%1:%2").arg(url.host()).arg(second)},
        {QStringLiteral("c"), QStringLiteral("refuses"), QStringLiteral("%1:%2").arg(url.host()).arg(down)}};
    const QHash<QString, int> ports{{QStringLiteral("a"), url.port(22)}, {QStringLiteral("b"), second},
                                    {QStringLiteral("c"), down}};
    auto *window = new RunCommandWindow(
        QStringLiteral("test"), hosts, 1,
        [&](const QStringList &ids) {
            QList<RunCommandWindow::Resolved> resolved;
            for (const auto &id : ids)
                resolved.append({settingsFor(ports.value(id)), QString()});
            return resolved;
        },
        [] { return QList<Snippet>{{QStringLiteral("disk"), QStringLiteral("df -h /"), true}}; }, nullptr);
    window->setAttribute(Qt::WA_DeleteOnClose, false);
    window->show();
    auto *command = window->findChild<QPlainTextEdit *>();
    auto *table = window->findChild<QTreeWidget *>();
    QPushButton *run = nullptr, *stop = nullptr;
    for (auto *button : window->findChildren<QPushButton *>()) {
        if (button->text().contains(QStringLiteral("Run on Checked")))
            run = button;
        if (button->text().contains(QStringLiteral("Stop")))
            stop = button;
    }
    QSpinBox *timeout = nullptr;
    for (auto *box : window->findChildren<QSpinBox *>())
        if (box->suffix() == QStringLiteral(" s"))
            timeout = box;
    auto *terminal = window->findChild<QCheckBox *>();
    auto *output = window->findChildren<QPlainTextEdit *>().value(1);
    const auto waitUntil = [](const std::function<bool()> &done, int ms) {
        QElapsedTimer clock;
        clock.start();
        while (!done() && clock.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
    };
    const auto finished = [&] { return run->isEnabled(); };
    const auto print = [&](const QString &title) {
        say(QStringLiteral("== %1").arg(title));
        for (int row = 0; row < table->topLevelItemCount(); ++row) {
            auto *item = table->topLevelItem(row);
            table->setCurrentItem(item);
            QString full = output->toPlainText();
            full.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
            if (full.size() > 160)
                full = full.left(80) + QStringLiteral(" … ") + full.right(70) + QStringLiteral(" (%1 chars)").arg(full.size());
            say(QStringLiteral("  %1 | %2 | exit %3 | %4 | %5").arg(item->text(0), item->text(1), item->text(2),
                                                                   item->text(3).isEmpty() ? QStringLiteral("-") : QStringLiteral("time"), full));
        }
    };
    const auto go = [&](const QString &text, const QString &title, int limit) {
        command->setPlainText(text);
        run->click();
        waitUntil(finished, limit);
        print(title);
    };
    go(QStringLiteral("echo out-$((40+2)); echo err-line >&2; hostname >/dev/null"), QStringLiteral("both streams"), 30000);
    go(QStringLiteral("echo before; exit 3"), QStringLiteral("exit status"), 30000);
    timeout->setValue(2);
    go(QStringLiteral("echo started; sleep 10"), QStringLiteral("time limit 2 s"), 30000);
    timeout->setValue(60);
    command->setPlainText(QStringLiteral("sleep 20"));
    run->click();
    waitUntil([&] { return table->topLevelItem(0)->text(1).contains(QStringLiteral("Running")); }, 15000);
    QElapsedTimer stopClock;
    stopClock.start();
    stop->click();
    waitUntil(finished, 15000);
    say(QStringLiteral("stop took %1 ms").arg(stopClock.elapsed()));
    print(QStringLiteral("stopped"));
    go(QStringLiteral("yes aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa | head -c 3000000; echo; echo tail-end"), QStringLiteral("3 MB of output"), 60000);
    go(QStringLiteral("tty"), QStringLiteral("no terminal"), 30000);
    terminal->setChecked(true);
    go(QStringLiteral("tty"), QStringLiteral("with a terminal"), 30000);
    say(QStringLiteral("csv:"));
    for (auto *button : window->findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("Copy Results"))
            button->click();
    say(QApplication::clipboard()->text().trimmed());
    say(QStringLiteral("recent: %1").arg(loadRunCommandPreferences().recent.mid(0, 3).join(QStringLiteral(" || "))));
    if (const auto picture = qEnvironmentVariable("OPSREMOTE_TEST_SCREENSHOT"); !picture.isEmpty()) {
        table->setCurrentItem(table->topLevelItem(0));
        window->grab().save(picture);
    }
    delete window;
    return 0;
}

int runStreamTest(const QString &spec)
{
    const QUrl url(spec);
    const QUrlQuery query(url);
    RshOptions options = RshOptions::builtIn();
    if (query.hasQueryItem(QStringLiteral("source")))
        options.sourcePort = query.queryItemValue(QStringLiteral("source"));
    options.command = query.queryItemValue(QStringLiteral("command"), QUrl::FullyDecoded);
    const bool rlogin = url.scheme() == QStringLiteral("rlogin");
    StreamConnection *connection = nullptr;
    if (rlogin)
        connection = new RloginConnection(url.host(), quint16(url.port(0)), url.userName(), options,
                                          QStringLiteral("xterm"));
    else
        connection = new RshConnection(url.host(), quint16(url.port(0)), url.userName(), options);
    QByteArray received;
    bool ended = false;
    QObject::connect(connection, &StreamConnection::connected, [] { say(QStringLiteral("connected")); });
    QObject::connect(connection, &StreamConnection::dataReceived, [&](const QByteArray &data) { received += data; });
    QObject::connect(connection, &StreamConnection::disconnected, [&](const QString &reason, bool error) {
        say(QStringLiteral("ended (%1): %2").arg(error ? "error" : "normal", reason));
        ended = true;
    });
    const auto waitFor = [&](const QByteArray &text, int ms) {
        QElapsedTimer clock;
        clock.start();
        while (!ended && !received.contains(text) && clock.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
        return received.contains(text);
    };
    const auto show = [&] {
        QString text = QString::fromUtf8(received);
        text.replace(QLatin1Char('\r'), QStringLiteral("\\r")).replace(QLatin1Char('\n'), QStringLiteral("\\n"));
        say(QStringLiteral("received: %1").arg(text.right(600)));
    };
    connection->open();
    if (rlogin) {
        if (waitFor("assword:", 15000)) {
            say(QStringLiteral("password prompt: yes"));
            connection->write(url.password().toUtf8() + '\r');
        }
        if (waitFor("$ ", 15000)) {
            connection->resize(100, 30);
            received.clear();
            connection->write("stty size; echo marker-$((6*7))\r");
            waitFor("marker-42", 10000);
            show();
            connection->write("exit\r");
        }
        waitFor("never", 5000);
    } else {
        waitFor("never-yet", 3000);
        if (const auto input = query.queryItemValue(QStringLiteral("input")); !input.isEmpty() && !ended) {
            connection->write(input.toUtf8() + '\r');
            connection->write("\x04");
        }
        waitFor("never", 10000);
        show();
    }
    delete connection;
    return 0;
}

int runXdmcpTest(const QString &host)
{
    auto options = loadXdmcpDefaults();
    options.size = QStringLiteral("1024x768");
    auto *page = new XdmcpPage(QStringLiteral("test"), host, 0, options);
    const auto labels = page->findChildren<QLabel *>();
    auto *log = page->findChild<QPlainTextEdit *>();
    const auto state = [&] { return labels.value(1) ? labels.value(1)->text() : QString(); };
    say(QStringLiteral("state: %1").arg(state()));
    const int seconds = qEnvironmentVariableIntValue("OPSREMOTE_TEST_SECONDS") > 0
                            ? qEnvironmentVariableIntValue("OPSREMOTE_TEST_SECONDS") : 10;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < seconds * 1000)
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
    say(QStringLiteral("state after %1 s: %2").arg(seconds).arg(state()));
    if (const auto picture = qEnvironmentVariable("OPSREMOTE_TEST_SCREENSHOT"); !picture.isEmpty()) {
        static const QRegularExpression displayNumber(QStringLiteral("display :(\\d+)"));
        const auto match = displayNumber.match(state());
        if (match.hasMatch()) {
            QProcess capture;
            capture.start(QStringLiteral("sh"), {QStringLiteral("-c"),
                                                 QStringLiteral("xwd -display :%1 -root -silent | magick xwd:- '%2'")
                                                     .arg(match.captured(1), picture)});
            capture.waitForFinished(15000);
            say(QStringLiteral("screen saved: %1").arg(capture.exitCode() == 0 ? "yes" : "no"));
        }
    }
    say(QStringLiteral("messages: %1").arg(log ? log->toPlainText().right(800) : QString()));
    page->stop();
    say(QStringLiteral("after stop: %1").arg(state()));
    delete page;
    return 0;
}

int runHttpTest()
{
    HttpRequester requester;
    HttpResponse last;
    bool done = false;
    QObject::connect(&requester, &HttpRequester::finished, [&](const HttpResponse &response) {
        last = response;
        done = true;
    });
    const auto run = [&](const QString &title, HttpRequestSpec spec) {
        done = false;
        requester.send(spec);
        QElapsedTimer clock;
        clock.start();
        while (!done && clock.elapsed() < 20000)
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
        QString body = QString::fromUtf8(last.body.left(260));
        body.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
        say(QStringLiteral("== %1").arg(title));
        say(QStringLiteral("   %1 %2 | error: %3 | %4 bytes (wire %5, %6) | redirects %7 | dns %8 connect %9 tls %10 first %11 total %12")
                .arg(last.status).arg(last.reason, last.error.isEmpty() ? QStringLiteral("-") : last.error)
                .arg(last.body.size()).arg(last.bodyBytesOnWire)
                .arg(last.contentEncoding.isEmpty() ? QStringLiteral("identity") : last.contentEncoding)
                .arg(last.redirects.size())
                .arg(last.dnsMs).arg(last.connectMs).arg(last.tlsMs).arg(last.firstByteMs).arg(last.totalMs));
        if (last.tls)
            say(QStringLiteral("   tls: sni=%1 %2 %3 cert=%4 problems=%5").arg(last.sniSent, last.tlsProtocol, last.cipher,
                    last.certificates.isEmpty() ? QStringLiteral("-") : last.certificates.first().subjectDisplayName(),
                    last.certificateProblems.isEmpty() ? QStringLiteral("none") : last.certificateProblems.join(QStringLiteral("; "))));
        say(QStringLiteral("   body: %1").arg(body));
    };
    const QString plain = QStringLiteral("http://127.0.0.1:18480");
    HttpRequestSpec spec;
    spec.userAgent = QStringLiteral("OpsRemote-test");
    spec.url = plain + QStringLiteral("/echo?x=1");
    run(QStringLiteral("GET"), spec);
    spec.method = QStringLiteral("POST");
    spec.body = R"({"hello": "world"})";
    spec.contentType = QStringLiteral("application/json");
    spec.headers = {{true, QStringLiteral("X-Test"), QStringLiteral("yes")}, {false, QStringLiteral("X-Off"), QStringLiteral("no")}};
    run(QStringLiteral("POST JSON with headers"), spec);
    spec = HttpRequestSpec();
    spec.method = QStringLiteral("HEAD");
    spec.url = plain + QStringLiteral("/echo");
    run(QStringLiteral("HEAD"), spec);
    spec.method = QStringLiteral("GET");
    spec.url = plain + QStringLiteral("/redirect");
    run(QStringLiteral("redirect followed"), spec);
    spec.followRedirects = false;
    run(QStringLiteral("redirect not followed"), spec);
    spec.followRedirects = true;
    spec.method = QStringLiteral("POST");
    spec.body = "data";
    spec.url = plain + QStringLiteral("/see-other");
    run(QStringLiteral("303 after POST becomes GET"), spec);
    spec = HttpRequestSpec();
    spec.url = plain + QStringLiteral("/gzip");
    run(QStringLiteral("gzip"), spec);
    spec.url = plain + QStringLiteral("/chunked");
    run(QStringLiteral("chunked"), spec);
    spec.url = plain + QStringLiteral("/status/500");
    run(QStringLiteral("500"), spec);
    spec.url = plain + QStringLiteral("/slow");
    spec.timeoutSeconds = 1;
    run(QStringLiteral("time limit 1 s"), spec);
    spec = HttpRequestSpec();
    spec.url = QStringLiteral("https://127.0.0.1:18443/echo");
    run(QStringLiteral("https by address, checked"), spec);
    spec.verifyCertificates = false;
    run(QStringLiteral("https by address, not checked"), spec);
    spec.sni = QStringLiteral("b.test");
    run(QStringLiteral("SNI b.test"), spec);
    spec = HttpRequestSpec();
    spec.verifyCertificates = false;
    spec.url = QStringLiteral("https://a.test:18443/echo");
    spec.connectTo = QStringLiteral("127.0.0.1");
    spec.hostHeader = QStringLiteral("site.example");
    run(QStringLiteral("URL a.test, connect to 127.0.0.1, Host site.example"), spec);
    say(QStringLiteral("curl: %1").arg(curlCommand(spec)));
    spec.sni = QStringLiteral("b.test");
    say(QStringLiteral("curl with SNI b.test: %1").arg(curlCommand(spec)));
    // The POST body as the server got it.
    spec = HttpRequestSpec();
    spec.method = QStringLiteral("POST");
    spec.url = plain + QStringLiteral("/echo");
    spec.body = "name=ü&x=1";
    spec.contentType = QStringLiteral("application/x-www-form-urlencoded");
    run(QStringLiteral("POST body"), spec);
    say(QStringLiteral("   body tail: %1").arg(QString::fromUtf8(last.body.right(40))));

    // The tab: Send, then Repeat 30 times, 5 at once.
    auto *page = new HttpRequestPage;
    page->setUrl(plain + QStringLiteral("/echo"));
    page->resize(1100, 800);
    page->show();
    QPushButton *sendButton = nullptr, *repeatButton = nullptr;
    for (auto *button : page->findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Send"))
            sendButton = button;
        if (button->text() == QStringLiteral("Run"))
            repeatButton = button;
    }
    const auto waitFor = [](const std::function<bool()> &condition, int ms) {
        QElapsedTimer clock;
        clock.start();
        while (!condition() && clock.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    };
    sendButton->click();
    waitFor([&] { return sendButton->text() == QStringLiteral("Send"); }, 10000);
    for (auto *label : page->findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("ms")))
            say(QStringLiteral("tab status: %1").arg(label->text()));
    }
    for (auto *box : page->findChildren<QSpinBox *>()) {
        if (box->suffix() == QStringLiteral(" times"))
            box->setValue(30);
        if (box->suffix() == QStringLiteral(" at once"))
            box->setValue(5);
    }
    repeatButton->click();
    waitFor([&] { return repeatButton->text() == QStringLiteral("Run"); }, 30000);
    for (auto *label : page->findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("done in")))
            say(QStringLiteral("repeat: %1").arg(label->text()));
    }
    if (const auto picture = qEnvironmentVariable("OPSREMOTE_TEST_SCREENSHOT"); !picture.isEmpty())
        page->grab().save(picture);
    delete page;
    return 0;
}

int runFollowTest(const QString &spec)
{
    const QUrl url(spec);
    SshConnectionSettings settings;
    settings.host = url.host();
    settings.port = quint16(url.port(22));
    settings.username = url.userName();
    settings.password = url.password();
    settings.tryPublicKeys = false;
    settings.knownHostsPath = QUrlQuery(url).queryItemValue(QStringLiteral("known_hosts"));
    settings.followTerminalFolder = false;
    auto *page = new TerminalPage(settings, TerminalAppearance());
    page->resize(900, 600);
    page->show();
    const auto wait = [](int ms) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    };
    wait(5000);
    say(QStringLiteral("connected, Files at: %1").arg(page->sftpPanel()->currentPath()));
    page->typeText(QStringLiteral("cd /tmp\r"));
    wait(2500);
    say(QStringLiteral("after cd /tmp, following off: %1 (follows: %2)")
            .arg(page->sftpPanel()->currentPath()).arg(page->followsFolder()));
    page->setFollowsFolder(true);
    wait(2500);
    say(QStringLiteral("after turning following on: %1 (follows: %2)")
            .arg(page->sftpPanel()->currentPath()).arg(page->followsFolder()));
    page->typeText(QStringLiteral("cd /etc\r"));
    wait(2500);
    say(QStringLiteral("after cd /etc: %1").arg(page->sftpPanel()->currentPath()));
    page->setFollowsFolder(false);
    page->typeText(QStringLiteral("cd /usr\r"));
    wait(2500);
    say(QStringLiteral("after turning it off and cd /usr: %1").arg(page->sftpPanel()->currentPath()));
    delete page;
    return 0;
}
