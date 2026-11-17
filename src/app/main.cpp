// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "stallwatch.h"
#include "workerthreads.h"
#include "mainwindow.h"
#include "sessionendedpanel.h"
#include "sftppanel.h"
#include "panedrag.h"
#include "rdpsession.h"
#include "rdppage.h"
#include <qtermwidget.h>
#include "settingsmigrations.h"
#include "localsessions.h"
#ifdef Q_OS_WIN
#include "conpty.h"
#endif
#include "filetransfertest.h"
#include "statusindicators.h"
#include "transfers.h"
#include "remotefiles.h"
#include "ftpclient.h"
#include "filespage.h"
#include "preferences.h"
#include "sshsession.h"
#include "systemcredentials.h"
#include "terminalpage.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QUrlQuery>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTabWidget>
#include <QPushButton>
#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QStandardPaths>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QCheckBox>
#include <QSpinBox>
#include <QFrame>
#include <QLabel>
#include <QFormLayout>
#include <QAbstractSpinBox>
#include <QMimeData>
#include <QDropEvent>
#include <QUrl>
#include <QTimer>
#include <QHeaderView>
#include <QTreeView>
#include <QSplitter>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>

#include <openssl/provider.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {
// OPSREMOTE_LOG=<file> appends Qt's messages to that file, which helps
// diagnose problems where no console is attached (the Windows GUI program).
// Deliberately never deleted: Qt may log until the very end of the process,
// after static objects are destroyed; the system closes the file.
QFile *logFile = nullptr;
// qInstallMessageHandler() may call this from any application thread. Both
// allocations intentionally survive static teardown, when Qt can still log.
QMutex *logFileMutex = new QMutex;

void writeLog(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (!logFile)
        return;
    QMutexLocker locker(logFileMutex);
    logFile->write(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz ")).toUtf8()
                   + message.toUtf8() + '\n');
    logFile->flush();
}

#ifdef Q_OS_WIN
// With OPSREMOTE_LOG (unless OPSREMOTE_LOG_BUSY_POINTER=0): notes when Windows shows its busy or "starting"
// pointer, and which programs started shortly before, to find what causes it.
void watchBusyPointer(QObject *owner)
{
    // All of it on one thread that lives as long as the program: on Windows
    // every thread that ends pauses the whole program for about a second,
    // and this used to start one every half second.
    auto *thread = new QThread(owner);
    thread->setObjectName(QStringLiteral("Busy-pointer watch"));
    auto *worker = new QObject;
    worker->moveToThread(thread);
    QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(thread, &QThread::started, worker, [worker] {
        struct State
        {
            bool busy = false;
            bool haveSnapshot = false;
            QHash<DWORD, QString> processes;
            QStringList started; // in the last few checks
        };
        auto state = std::make_shared<State>();
        auto *timer = new QTimer(worker);
        QObject::connect(timer, &QTimer::timeout, worker, [state] {
            QHash<DWORD, QString> now;
            HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (handle != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W entry{};
                entry.dwSize = sizeof entry;
                for (BOOL more = Process32FirstW(handle, &entry); more; more = Process32NextW(handle, &entry))
                    now.insert(entry.th32ProcessID, QStringLiteral("%1 (%2, parent %3)")
                                                         .arg(QString::fromWCharArray(entry.szExeFile))
                                                         .arg(entry.th32ProcessID)
                                                         .arg(entry.th32ParentProcessID));
                CloseHandle(handle);
            }
            if (state->haveSnapshot) {
                for (auto it = now.cbegin(); it != now.cend(); ++it)
                    if (!state->processes.contains(it.key()))
                        state->started << it.value();
            }
            state->haveSnapshot = true;
            while (state->started.size() > 20)
                state->started.removeFirst();
            state->processes = now;
            CURSORINFO cursor{};
            cursor.cbSize = sizeof cursor;
            if (!GetCursorInfo(&cursor))
                return;
            static const HCURSOR starting = LoadCursorW(nullptr, IDC_APPSTARTING);
            static const HCURSOR wait = LoadCursorW(nullptr, IDC_WAIT);
            const bool busy = cursor.hCursor == starting || cursor.hCursor == wait;
            if (busy == state->busy)
                return;
            state->busy = busy;
            if (busy) {
                // InternalGetWindowText sends no message, so a stuck window
                // (OpsRemote's own, say) cannot hold this thread up.
                wchar_t title[256] = {};
                InternalGetWindowText(GetAncestor(WindowFromPoint(cursor.ptScreenPos), GA_ROOT), title, 256);
                qInfo().noquote() << "busy pointer:" << (cursor.hCursor == wait ? "wait" : "starting") << "over"
                                  << QString::fromWCharArray(title) << "| programs started lately:"
                                  << (state->started.isEmpty() ? QStringLiteral("none")
                                                               : state->started.join(QStringLiteral("; ")));
                state->started.clear();
            } else {
                qInfo("busy pointer: ended");
            }
        });
        timer->start(500);
    });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread] {
        thread->quit();
        if (!thread->wait(1000))
            qWarning("busy-pointer watch did not finish before shutdown deadline");
    });
    thread->start();
}
#endif

// Ends the process at once. On Windows, exit() and _Exit() still run every
// DLL's shutdown code (DLL_PROCESS_DETACH), and a library waiting there for a
// thread that was already stopped (OpenSSL, libssh, FreeRDP) hangs the
// process forever without a window. TerminateProcess skips that code.
[[noreturn]] void endProcess(int result)
{
    std::fflush(nullptr);
#ifdef Q_OS_WIN
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
#endif
    std::_Exit(result);
}
}

int main(int argc, char *argv[])
{
    // OpenSSL 3 turns on its default provider, which has the random number
    // generator, by itself only if no provider was loaded before its first
    // use. LibVNCClient (DES for VNC passwords), FreeRDP, and Qt's hashing
    // load the "legacy" provider explicitly; whenever one of them came first,
    // every SSH connection afterwards failed with "PRNG error". Loading the
    // default provider here, before anything else, keeps it on for the run.
    OSSL_PROVIDER_load(nullptr, "default");

#ifdef Q_OS_WIN
    // The elevated helper of an administrator terminal (see conpty.h): no
    // window, just the relay.
    if (argc > 1 && std::strcmp(argv[1], "--elevated-console") == 0)
        return runElevatedConsoleHelper();
#endif

    // PORTABLE_TERMINAL_LOG is the name from before the rename to OpsRemote.
    QString path = qEnvironmentVariable("OPSREMOTE_LOG");
    if (path.isEmpty())
        path = qEnvironmentVariable("PORTABLE_TERMINAL_LOG");
    if (!path.isEmpty()) {
        logFile = new QFile(path);
        if (logFile->open(QIODevice::Append | QIODevice::Text))
            qInstallMessageHandler(writeLog);
    }
#ifdef Q_OS_LINUX
    // On Wayland Qt asks the desktop for an activation token whenever it
    // wants a window focused, which includes after most menus and dialogs
    // close. GNOME takes each token for a program starting and shows its
    // busy pointer until the token is used on a shown window or times out.
    // Without the protocol that stops; windows can no longer pull themselves
    // forward (Preferences > Startup turns it back on). A value the user set
    // is kept.
    if (!qEnvironmentVariableIsSet("QT_WAYLAND_DISABLED_INTERFACES") && !waylandActivationBeforeStart())
        qputenv("QT_WAYLAND_DISABLED_INTERFACES", "xdg_activation_v1");
#endif
    QApplication application(argc, argv);
    // For tests: every setting with its defaults (tests/settings-migration/
    // schema.yaml), without reading or writing the real settings.
    if (const auto snapshot = qEnvironmentVariable("OPSREMOTE_TEST_SETTINGS_SNAPSHOT"); !snapshot.isEmpty()) {
        QFile file(snapshot);
        return file.open(QIODevice::WriteOnly) && file.write(settingsSnapshot()) > 0 ? 0 : 1;
    }
    // Each diagnostic can be left out (=0), to tell whether one of them
    // changes what it is looking for.
#ifdef Q_OS_WIN
    if (logFile && qEnvironmentVariable("OPSREMOTE_LOG_BUSY_POINTER") != QStringLiteral("0"))
        watchBusyPointer(&application);
#endif
    if (logFile && qEnvironmentVariable("OPSREMOTE_LOG_STALLS") != QStringLiteral("0"))
        watchGuiStalls(&application);
    if (logFile)
        logCrashes();
    // For automated tests: a write, read, and remove in the system credential
    // store, reported to the file OPSREMOTE_TEST_CREDENTIALS names.
    if (const auto report = qEnvironmentVariable("OPSREMOTE_TEST_CREDENTIALS"); !report.isEmpty()) {
        QString error;
        QStringList lines;
        lines << QStringLiteral("available: %1").arg(SystemCredentials::isAvailable(&error)) + QLatin1Char(' ') + error;
        lines << QStringLiteral("write: %1 %2").arg(SystemCredentials::write(QStringLiteral("opsremote-selftest"), QStringLiteral("pässwörd 123"), &error)).arg(error);
        lines << QStringLiteral("read: '%1' %2").arg(SystemCredentials::read(QStringLiteral("opsremote-selftest"), &error), error);
        lines << QStringLiteral("remove: %1 %2").arg(SystemCredentials::remove(QStringLiteral("opsremote-selftest"), &error)).arg(error);
        lines << QStringLiteral("gone: %1").arg(SystemCredentials::read(QStringLiteral("opsremote-selftest")).isNull());
        QFile file(report);
        if (file.open(QIODevice::WriteOnly))
            file.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
        return 0;
    }
    // For automated tests: every file operation against a server.
    // OPSREMOTE_TEST_KUBECONFIG=token|certificate: prints the kubeconfig made
    // for https://192.0.2.9:6443 (token "secret-token", namespace "web").
    if (const auto kind = qEnvironmentVariable("OPSREMOTE_TEST_KUBECONFIG"); !kind.isEmpty()) {
        LocalSessionOptions options;
        options.kubeAuth = kind;
        options.server = QStringLiteral("https://192.0.2.9:6443");
        options.caFile = QStringLiteral("/tmp/ca.pem");
        options.clientCertificate = QStringLiteral("/tmp/client.pem");
        options.clientKey = QStringLiteral("/tmp/client-key.pem");
        options.kubeNamespace = QStringLiteral("web");
        std::fprintf(stdout, "%s\n", qUtf8Printable(generatedKubeconfig(options, QStringLiteral("secret-token"))));
        return 0;
    }
    if (const auto spec = qEnvironmentVariable("OPSREMOTE_TEST_FILES"); !spec.isEmpty())
        return runFileTransferTest(spec);
    if (const auto folder = qEnvironmentVariable("OPSREMOTE_TEST_SERVERS"); !folder.isEmpty())
        return runLocalServersTest(folder);
    if (const auto spec = qEnvironmentVariable("OPSREMOTE_TEST_TUNNEL"); !spec.isEmpty())
        return runTunnelTest(spec);
    if (const auto spec = qEnvironmentVariable("OPSREMOTE_TEST_TUNNEL_RUNNER"); !spec.isEmpty())
        return runTunnelRunnerTest(spec);
    if (const auto path = qEnvironmentVariable("OPSREMOTE_TEST_SSHCONFIG"); !path.isEmpty())
        return runSshConfigTest(path);
    if (const auto spec = qEnvironmentVariable("OPSREMOTE_TEST_RUNCOMMAND"); !spec.isEmpty())
        return runRunCommandTest(spec);
    if (const auto spec = qEnvironmentVariable("OPSREMOTE_TEST_STREAM"); !spec.isEmpty())
        return runStreamTest(spec);
    if (const auto host = qEnvironmentVariable("OPSREMOTE_TEST_XDMCP"); !host.isEmpty())
        return runXdmcpTest(host);
    if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_HTTP") > 0)
        return runHttpTest();
    if (const auto spec = qEnvironmentVariable("OPSREMOTE_TEST_FOLLOW"); !spec.isEmpty())
        return runFollowTest(spec);
    if (const auto spec = qEnvironmentVariable("OPSREMOTE_TEST_SSHDROP"); !spec.isEmpty())
        return runSshDropTest(spec);
    QApplication::setApplicationName("OpsRemote");
    QApplication::setOrganizationName("OpsRemote");
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/opsremote-%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(icon);

    // A bundled OpenSSL looks for its providers at the path it was built
    // with; use the bundle's copy of the legacy provider (MD4 for NTLM).
    for (const auto &folder : {QStringLiteral("lib/ossl-modules"), QStringLiteral("ossl-modules")}) {
        const QDir modules(QDir(QCoreApplication::applicationDirPath()).filePath(folder));
        if (qEnvironmentVariableIsEmpty("OPENSSL_MODULES") && modules.exists())
            qputenv("OPENSSL_MODULES", QDir::toNativeSeparators(modules.absolutePath()).toLocal8Bit());
    }

    // One copy per user: a second start asks the first to come forward.
    // Not a cryptographic hash: Qt may do those with the bundled OpenSSL,
    // and using it this early broke OpenSSL's random numbers for SSH and RDP.
    QString user = QString::fromLocal8Bit(qgetenv(
#ifdef Q_OS_WIN
        "USERNAME"
#else
        "USER"
#endif
        ));
    for (auto &character : user) {
        if (!character.isLetterOrNumber())
            character = QLatin1Char('_');
    }
    QString instanceName = QStringLiteral("OpsRemote-") + user;
#ifndef Q_OS_WIN
    // In the user's own runtime folder (owner-only), not the shared /tmp,
    // so another user cannot take the name and keep OpsRemote from starting.
    if (const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        !runtime.isEmpty())
        instanceName = QDir(runtime).filePath(QStringLiteral("opsremote-instance"));
#endif
    QLocalServer instanceServer;
    if (loadStartupPreferences().singleInstance) {
        QLocalSocket running;
        running.connectToServer(instanceName);
        if (running.waitForConnected(500)) {
            // "activate", then this start's command line, one argument per line.
            QByteArray message("activate\n");
            for (const auto &argument : application.arguments().mid(1))
                message += argument.toUtf8().replace('\n', ' ') + '\n';
            running.write(message);
            running.waitForBytesWritten(500);
            return 0;
        }
        // A socket left by a copy that crashed.
        QLocalServer::removeServer(instanceName);
        instanceServer.setSocketOptions(QLocalServer::UserAccessOption);
        // A runtime folder path too long for a socket: the plain name.
        if (!instanceServer.listen(instanceName) && instanceName.contains(QLatin1Char('/'))) {
            instanceName = QStringLiteral("OpsRemote-") + user;
            QLocalServer::removeServer(instanceName);
            instanceServer.listen(instanceName);
        }
    }

    // Older settings brought to the current shape, now that this is the
    // only copy running and before anything reads them (except the startup
    // keys read above: startup.single_instance and wayland_activation, which
    // a migration must therefore never move).
    for (const auto &line : migrateSettings())
        qInfo("settings migration: %s", qUtf8Printable(line));

    int result = 0;
    {
        MainWindow window;
        window.show();
        if (const auto arguments = application.arguments().mid(1); !arguments.isEmpty())
            QTimer::singleShot(0, &window, [&window, arguments] { window.handleCommandLine(arguments); });
        QObject::connect(&instanceServer, &QLocalServer::newConnection, &window, [&] {
            while (auto *connection = instanceServer.nextPendingConnection()) {
                // The other start's arguments arrive after "activate"; it
                // closes the connection when it has sent them.
                const auto handle = [&window, connection] {
                    QStringList arguments;
                    const auto lines = QString::fromUtf8(connection->readAll()).split(QLatin1Char('\n'));
                    for (const auto &line : lines.mid(1)) {
                        if (!line.isEmpty())
                            arguments.append(line);
                    }
                    if (!arguments.isEmpty())
                        window.handleCommandLine(arguments);
                    connection->deleteLater();
                };
                if (connection->state() == QLocalSocket::UnconnectedState)
                    handle();
                else
                    QObject::connect(connection, &QLocalSocket::disconnected, &window, handle);
            }
            window.setWindowState((window.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            window.show();
            window.raise();
            window.activateWindow();
        });
        // For automated tests: open Preferences on a page and save a picture
        // of it (OPSREMOTE_TEST_PREFERENCES=page, OPSREMOTE_TEST_SCREENSHOT=file,
        // OPSREMOTE_TEST_DELAY_MS=wait before the picture, 700 by default,
        // OPSREMOTE_TEST_SEARCH=text typed into the session search first,
        // OPSREMOTE_TEST_SPLIT=1 a local terminal split right, then down,
        // OPSREMOTE_TEST_ADMIN=1 an administrator terminal, on Windows,
        // OPSREMOTE_TEST_TYPE=command typed into the first terminal after 3 s,
        // OPSREMOTE_TEST_MULTIEXEC=1 with SPLIT: all of them chosen for multi-exec).
        // OPSREMOTE_TEST_DROP=path[;path]: opens a local terminal, cds it to
        // OPSREMOTE_TEST_DROP_FOLDER (if set) and drops these files on it after 2 s.
        // OPSREMOTE_TEST_MESSAGES=new|open: sample messages (one already read,
        // one warning); "open" also opens their list.
        if (const auto messages = qEnvironmentVariable("OPSREMOTE_TEST_MESSAGES"); !messages.isEmpty()) {
            QTimer::singleShot(200, &window, [&window, messages] {
                auto *log = window.findChild<MessageLog *>();
                if (!log)
                    return;
                log->add(QStringLiteral("Theme \"OpsRemote Orange\" could not load its icons; the built-in ones are used."));
                log->markAllRead();
                log->add(QStringLiteral("Could not reach the update server; OpsRemote checks again tomorrow."),
                         MessageLog::Level::Warning);
                log->add(QStringLiteral("Credential vault locked after 15 minutes without use"));
                log->add(QStringLiteral("3 sessions were imported from PuTTY; 1 was skipped because its host name is "
                                        "empty (\"Old web server\")."));
                if (messages == QStringLiteral("open")) {
                    for (auto *button : window.findChildren<StatusIndicator *>())
                        if (button->stateText().startsWith(QStringLiteral("Messages"))) {
                            QTimer::singleShot(300, button, &QAbstractButton::click);
                            break;
                        }
                }
            });
        }
        // OPSREMOTE_TEST_TAB_MESSAGES=new|open|ended: a local terminal with sample tab
        // messages; "open" also opens the tab's list, "ended" adds the session-ended prompt. "split" splits it first.
        if (const auto tab = qEnvironmentVariable("OPSREMOTE_TEST_TAB_MESSAGES"); !tab.isEmpty()) {
            QTimer::singleShot(300, &window, [&window, tab] {
                window.openLocalTerminal();
                QTimer::singleShot(900, &window, [&window, tab] {
                    auto *page = window.findChild<TerminalPage *>();
                    if (!page)
                        return;
                    window.tabMessage(page, QStringLiteral("Recording to ~/opsremote-logs/2026-11-17 web01.cast"));
                    window.tabMessage(page, QStringLiteral("Could not send the script deploy-web to the server: "
                                                           "permission denied for /var/tmp/opsremote-scripts"),
                                      true);
                    window.tabMessage(page, QStringLiteral("Scripts run in connected SSH tabs (and local terminals). "
                                                           "Connect this tab first, then run the script again."));
                    if (tab == QStringLiteral("ended")) {
                        // The session-ended prompt, as after a logout.
                        if (auto *ended = page->findChild<SessionEndedPanel *>()) {
                            ended->setMessage(QStringLiteral("Disconnected from 192.0.2.10"),
                                              QStringLiteral("Session closed (exit status 0)."), false);
                            ended->show();
                        }
                    }
                    if (tab == QStringLiteral("open"))
                        for (auto *strip : window.findChildren<TabMessageStrip *>())
                            QTimer::singleShot(300, strip, [strip] { emit strip->historyRequested(); });
                });
            });
        }
        // OPSREMOTE_TEST_TRANSFERS=strip|tab|all|none: a local terminal with sample
        // transfers (two running, one done); "tab" opens the tab's transfer
        // list, "all" the Transfers list.
        if (const auto mode = qEnvironmentVariable("OPSREMOTE_TEST_TRANSFERS"); !mode.isEmpty()) {
            QTimer::singleShot(300, &window, [&window, mode] {
                window.openLocalTerminal();
                QTimer::singleShot(900, &window, [&window, mode] {
                    auto *page = window.findChild<TerminalPage *>();
                    auto *list = TransferList::instance();
                    if (mode == QStringLiteral("none")) { // the list with no transfers at all
                        for (auto *button : window.findChildren<StatusIndicator *>())
                            if (button->stateText().startsWith(QStringLiteral("Transfers")))
                                QTimer::singleShot(300, button, &QAbstractButton::click);
                        return;
                    }
                    const int done = list->start(page, QStringLiteral("notes.txt"), false);
                    list->update(done, {}, 4096, 4096);
                    list->finish(done, TransferList::State::Done);
                    const auto cancelled = [] {};
                    const int down = list->start(page, QStringLiteral("report-2026-q3.pdf"), false, cancelled,
                                                 QStringLiteral("Cancel this transfer and any queued after it"));
                    list->update(down, {}, 2900000, 10400000, QStringLiteral("2.8 MB of 9.9 MB · 0.9 MB/s"));
                    const int up = list->start(page, QStringLiteral("backup-2026-11-17.tar.gz"), true, cancelled,
                                               QStringLiteral("Cancel this transfer and any queued after it"));
                    list->update(up, {}, 62000000, 100000000, QStringLiteral("59.1 MB of 95.4 MB · 1.4 MB/s"));
                    // The file transfers' buttons: pause / continue, and retry once cancelled.
                    list->setPause(up, [](bool) {});
                    list->setPause(down, [](bool) {});
                    list->pause(down, true);
                    const int stopped = list->start(page, QStringLiteral("archlinux-2026.10.01-x86_64.iso"), false, cancelled,
                                                    QStringLiteral("Cancel this transfer and any queued after it"));
                    list->setRetry(stopped, [] {});
                    list->update(stopped, {}, 247000000, 1640000000);
                    list->finish(stopped, TransferList::State::Cancelled);
                    window.tabMessage(page, QStringLiteral("Recording to ~/opsremote-logs/2026-11-17 web01.cast"));
                    // OPSREMOTE_TEST_TRANSFER_END=1: both running ones are cancelled once the list is open.
                    if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_TRANSFER_END") > 0) {
                        QTimer::singleShot(1000, &window, [list, up, down] {
                            list->finish(up, TransferList::State::Cancelled);
                            list->finish(down, TransferList::State::Cancelled);
                        });
                    }
                    if (mode == QStringLiteral("tab")) {
                        for (auto *strip : window.findChildren<TabMessageStrip *>())
                            QTimer::singleShot(300, strip, [strip] { emit strip->transfersRequested(); });
                    } else if (mode == QStringLiteral("all")) {
                        for (auto *button : window.findChildren<StatusIndicator *>())
                            if (button->stateText().startsWith(QStringLiteral("Transfers")))
                                QTimer::singleShot(300, button, &QAbstractButton::click);
                    }
                    // OPSREMOTE_TEST_OVERLAY_BLOCK=1 (with "all"): while the list is
                    // open, pointer moves and keys sent to the terminal must not
                    // reach it; a click on it closes the list, then they do again.
                    if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_OVERLAY_BLOCK") > 0) {
                        QTimer::singleShot(1000, &window, [&window] {
                            auto *terminal = window.findChild<TerminalPage *>()->findChild<QTermWidget *>();
                            auto *target = terminal ? terminal->findChild<QWidget *>() : nullptr;
                            if (!target) {
                                std::fprintf(stderr, "test overlay: no terminal\n");
                                return;
                            }
                            struct Counter : QObject
                            {
                                int moves = 0, keys = 0;
                                bool eventFilter(QObject *, QEvent *event) override
                                {
                                    moves += event->type() == QEvent::MouseMove;
                                    keys += event->type() == QEvent::KeyPress;
                                    return false;
                                }
                            };
                            auto *counter = new Counter;
                            target->installEventFilter(counter);
                            const auto send = [target](QEvent::Type type) {
                                const QPointF at(20, 20);
                                QMouseEvent event(type, at, target->mapToGlobal(at), Qt::LeftButton,
                                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
                                QApplication::sendEvent(target, &event);
                            };
                            const auto lists = [&window] {
                                int open = 0;
                                for (auto *child : window.findChildren<QFrame *>(QStringLiteral("messageList")))
                                    open += child->isVisible();
                                return open;
                            };
                            send(QEvent::MouseMove);
                            QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
                            QApplication::sendEvent(target, &key);
                            std::fprintf(stderr, "test overlay: list open %d, moves reached %d, keys reached %d\n",
                                         lists(), counter->moves, counter->keys);
                            send(QEvent::MouseButtonPress);
                            send(QEvent::MouseButtonRelease);
                            QTimer::singleShot(200, &window, [&window, send, counter, lists] {
                                send(QEvent::MouseMove);
                                std::fprintf(stderr, "test overlay: after a click: list open %d, moves reached %d\n",
                                             lists(), counter->moves);
                            });
                        });
                    }
                });
            });
        }
        // OPSREMOTE_TEST_FILES_TAB=ftp://user:password@host:port?upload=file&download=path[&cancel=ms][&same=…]
        // opens an FTP file tab, uploads the file to the home folder and
        // downloads path to a temporary folder at the same time (cancelling
        // the download through the transfer list after ms), then prints the
        // transfers' states after OPSREMOTE_TEST_SECONDS (20).
        if (const auto spec = qEnvironmentVariable("OPSREMOTE_TEST_FILES_TAB"); !spec.isEmpty()) {
            QTimer::singleShot(300, &window, [&window, spec] {
                const QUrl url(spec);
                const QUrlQuery query(url);
                FtpConnectionSettings settings;
                settings.host = url.host();
                settings.port = quint16(url.port(21));
                settings.username = url.userName();
                settings.password = url.password();
                settings.security = QStringLiteral("none");
                auto *page = new FilesPage(settings, false);
                window.openTestTab(page, QStringLiteral("FTP test"));
                const auto upload = query.queryItemValue(QStringLiteral("upload"));
                const auto download = query.queryItemValue(QStringLiteral("download"));
                const int cancelAfter = query.queryItemValue(QStringLiteral("cancel")).toInt();
                const int pauseAfter = query.queryItemValue(QStringLiteral("pause")).toInt();
                const bool retry = query.queryItemValue(QStringLiteral("retry")) == QStringLiteral("1");
                // The page makes its session when it starts connecting.
                auto *wait = new QTimer(page);
                wait->start(50);
                QObject::connect(wait, &QTimer::timeout, page, [=] {
                auto *session = page->findChild<RemoteFileSession *>();
                if (!session)
                    return;
                wait->stop();
                // same=wait|newname|stop|dont: the upload goes through the panel, and
                // a second FTP tab uploads the same file 1 s later through its panel,
                // answering "Same File in Transfer" so (its picture: OPSREMOTE_TEST_SAME_PICTURE).
                const auto same = query.queryItemValue(QStringLiteral("same"));
                if (!same.isEmpty()) {
                    QObject::connect(session, &RemoteFileSession::sftpReady, page, [=](const QString &home) {
                        page->findChild<SftpPanel *>()->uploadInto({upload}, home);
                        auto *mainWindow = qobject_cast<MainWindow *>(page->window());
                        QTimer::singleShot(1000, mainWindow, [=] {
                            auto *second = new FilesPage(settings, false);
                            mainWindow->openTestTab(second, QStringLiteral("FTP test 2"));
                            auto *poll = new QTimer(second);
                            poll->start(50);
                            QObject::connect(poll, &QTimer::timeout, second, [=] {
                                auto *other = second->findChild<RemoteFileSession *>();
                                if (!other)
                                    return;
                                poll->stop();
                                QObject::connect(other, &RemoteFileSession::sftpReady, second, [=](const QString &home) {
                                    // Answers each question as it comes (then "Overwrite" when
                                    // the file the other wrote is there).
                                    auto *answer = new QTimer(second);
                                    answer->start(200);
                                    QObject::connect(answer, &QTimer::timeout, second, [same] {
                                        for (auto *widget : QApplication::topLevelWidgets()) {
                                            auto *box = qobject_cast<QMessageBox *>(widget);
                                            if (!box || !box->isVisible())
                                                continue;
                                            const bool sameFile = box->windowTitle() == QStringLiteral("Same File in Transfer");
                                            if (const auto picture = qEnvironmentVariable("OPSREMOTE_TEST_SAME_PICTURE"); sameFile && !picture.isEmpty())
                                                box->grab().save(picture);
                                            const QString text = !sameFile ? QStringLiteral("Overwrite")
                                                : same == QStringLiteral("wait") ? QStringLiteral("Wait for It")
                                                : same == QStringLiteral("newname") ? QStringLiteral("Save Under a New Name")
                                                : same == QStringLiteral("stop") ? QStringLiteral("Stop It, Start This")
                                                                                 : QStringLiteral("Don't Start");
                                            for (auto *button : box->buttons()) {
                                                if (button->text() == text) {
                                                    std::fprintf(stderr, "test same: %s: %s (enabled %d)\n", qPrintable(box->windowTitle()),
                                                                 qPrintable(text), button->isEnabled());
                                                    button->click();
                                                }
                                            }
                                        }
                                    });
                                    second->findChild<SftpPanel *>()->uploadInto({upload}, home);
                                });
                            });
                        });
                    });
                    return;
                }
                QObject::connect(session, &RemoteFileSession::sftpReady, page, [=](const QString &home) {
                    if (!upload.isEmpty())
                        session->upload({upload}, home, SftpConflictResolution::Overwrite);
                    if (!download.isEmpty())
                        session->download({download}, QDir::tempPath(), SftpConflictResolution::Overwrite);
                    if (cancelAfter > 0) {
                        QTimer::singleShot(cancelAfter, page, [retry] {
                            const auto running = TransferList::instance()->running();
                            if (!running.isEmpty())
                                TransferList::instance()->cancel(running.first().id);
                            // retry=1: that cancelled transfer again, from the beginning.
                            if (retry && !running.isEmpty())
                                QTimer::singleShot(500, [id = running.first().id] { TransferList::instance()->retry(id); });
                        });
                    }
                    // pause=ms: paused for 2 s after ms; the % before and after must match.
                    if (pauseAfter > 0) {
                        QTimer::singleShot(pauseAfter, page, [page] {
                            const auto running = TransferList::instance()->running();
                            if (running.isEmpty())
                                return;
                            const int id = running.first().id;
                            TransferList::instance()->pause(id, true);
                            QTimer::singleShot(300, page, [id] { // what was already on its way
                                for (const auto &transfer : TransferList::instance()->transfers())
                                    if (transfer.id == id)
                                        std::fprintf(stderr, "test pause: paused at %llu\n", transfer.done);
                            });
                            QTimer::singleShot(2300, page, [id] {
                                for (const auto &transfer : TransferList::instance()->transfers())
                                    if (transfer.id == id)
                                        std::fprintf(stderr, "test pause: still at %llu, continuing\n", transfer.done);
                                TransferList::instance()->pause(id, false);
                            });
                        });
                    }
                });
                });
                const int seconds = qEnvironmentVariableIntValue("OPSREMOTE_TEST_SECONDS") > 0
                                        ? qEnvironmentVariableIntValue("OPSREMOTE_TEST_SECONDS")
                                        : 20;
                QTimer::singleShot(seconds * 1000, &window, [] {
                    static const char *names[] = {"running", "done", "failed", "cancelled"};
                    for (const auto &transfer : TransferList::instance()->transfers())
                        std::printf("transfer %s %s %d%% %s\n", transfer.upload ? "up" : "down",
                                    qUtf8Printable(transfer.name), transfer.percent(), names[int(transfer.state)]);
                    std::fflush(stdout);
                    QCoreApplication::exit(0);
                });
            });
        }
        // OPSREMOTE_TEST_LEFT_TAB=name: shows that side panel tab (Files, Commands…).
        if (const auto left = qEnvironmentVariable("OPSREMOTE_TEST_LEFT_TAB"); !left.isEmpty()) {
            QTimer::singleShot(1500, &window, [&window, left] {
                for (auto *tabs : window.findChildren<QTabWidget *>())
                    for (int index = 0; index < tabs->count(); ++index)
                        if (tabs->tabText(index).remove(QLatin1Char('&')) == left)
                            tabs->setCurrentIndex(index);
            });
        }
        if (const auto drop = qEnvironmentVariable("OPSREMOTE_TEST_DROP"); !drop.isEmpty()) {
            QTimer::singleShot(300, &window, [&window, drop] {
                window.openLocalTerminal();
                const auto folder = qEnvironmentVariable("OPSREMOTE_TEST_DROP_FOLDER");
                QTimer::singleShot(800, &window, [folder] {
                    for (auto *widget : QApplication::allWidgets())
                        if (widget->inherits("Konsole::TerminalDisplay") && widget->isVisible() && !folder.isEmpty()) {
                            QKeyEvent key(QEvent::KeyPress, 0, Qt::NoModifier,
                                          QStringLiteral("cd '%1'\r").arg(folder));
                            QCoreApplication::sendEvent(widget, &key);
                            break;
                        }
                });
                QTimer::singleShot(2000, &window, [&window, drop] {
                    // As a drop on the terminal does (real drag events are
                    // delivered only during a real drag).
                    if (auto *page = window.findChild<TerminalPage *>())
                        page->dropFiles(drop.split(QLatin1Char(';')), false);
                    else
                        std::fprintf(stderr, "test drop: no terminal\n");
                });
            });
        }
        // OPSREMOTE_TEST_EDGE_DBLCLICK=ms: narrows the side panel after 1 s, then
        // double-clicks its edge after ms (the panel fits what it shows).
        if (const int at = qEnvironmentVariableIntValue("OPSREMOTE_TEST_EDGE_DBLCLICK"); at > 0) {
            QTimer::singleShot(1000, &window, [&window] {
                if (auto *splitter = qobject_cast<QSplitter *>(window.centralWidget()))
                    splitter->setSizes({1, splitter->width()});
                if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_EDGE_NARROW_ONLY") > 0)
                    return;
            });
            QTimer::singleShot(at, &window, [&window] {
                if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_EDGE_NARROW_ONLY") > 0) {
                    // Where the side panel's rows start, in the window.
                    const auto left = [&window](QWidget *widget) { return widget ? widget->mapTo(&window, QPoint()).x() : -1; };
                    auto *search = window.findChild<QLineEdit *>(QStringLiteral("sessionSearch"));
                    auto *tree = search ? search->parentWidget()->findChild<QTreeView *>() : nullptr;
                    auto *tabs = search ? qobject_cast<QTabWidget *>(search->parentWidget()->parentWidget()->parentWidget()) : nullptr;
                    std::fprintf(stderr, "test rows: tab bar %d, first tab %d, search %d, list %d, list frame %d, header %d\n",
                                 tabs ? left(tabs->tabBar()) : -1,
                                 tabs ? left(tabs->tabBar()) + tabs->tabBar()->tabRect(0).x() : -1, left(search),
                                 left(tree), tree ? tree->frameWidth() : -1, tree ? left(tree->header()) : -1);
                    return; // the panel stays narrow, for a picture
                }
                auto *splitter = qobject_cast<QSplitter *>(window.centralWidget());
                if (!splitter)
                    return;
                auto *handle = splitter->handle(1);
                const QPointF at(handle->width() / 2.0, handle->height() / 2.0);
                QMouseEvent click(QEvent::MouseButtonDblClick, at, handle->mapToGlobal(at), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(handle, &click);
                std::fprintf(stderr, "test edge: panel %d\n", splitter->sizes().value(0));
                for (auto *files : window.findChildren<SftpPanel *>())
                    std::fprintf(stderr, "test edge: files %d wide, fits in %d\n", files->width(), files->contentsWidth());
            });
        }
        // OPSREMOTE_TEST_CLICK=text[;text…]: clicks the shown button whose text
        // starts with each, one after the other (OPSREMOTE_TEST_CLICK_MS
        // apart, 800 by default; the first after that long too).
        if (const auto texts = qEnvironmentVariable("OPSREMOTE_TEST_CLICK"); !texts.isEmpty()) {
            const int gap = qEnvironmentVariableIntValue("OPSREMOTE_TEST_CLICK_MS") > 0
                                ? qEnvironmentVariableIntValue("OPSREMOTE_TEST_CLICK_MS")
                                : 800;
            int at = 0;
            for (const auto &text : texts.split(QLatin1Char(';'))) {
                at += gap;
                QTimer::singleShot(at, &window, [text] {
                    for (auto *button : QApplication::allWidgets()) {
                        auto *push = qobject_cast<QAbstractButton *>(button);
                        if (push && push->isVisible() && push->text().remove(QLatin1Char('&')).startsWith(text)) {
                            push->click();
                            return;
                        }
                    }
                    std::fprintf(stderr, "test click: no button \"%s\"\n", qPrintable(text));
                });
            }
        }
        // OPSREMOTE_TEST_FONT_SIZE=pt: the interface font changes to it after 500 ms
        // (not saved), to picture how open windows follow.
        // OPSREMOTE_TEST_PANE_MOVE=hover|left|top|newtab|ontab|group, with
        // OPSREMOTE_TEST_SPLIT=1 (panes A left, B top right, C bottom right):
        // drags C as by its bar. hover: held over A's left part (for a
        // picture); left/top: let go there; newtab: on the tab strip past the
        // tabs; ontab: on the tab; group: a second split tab (D, E) dragged
        // whole onto A's right half. Prints the tabs after.
        if (const auto move = qEnvironmentVariable("OPSREMOTE_TEST_PANE_MOVE"); !move.isEmpty()) {
            if (move == QStringLiteral("group") || move == QStringLiteral("pull"))
                QTimer::singleShot(1200, &window, [&window] {
                    window.openLocalTerminal();
                    window.openInSplit(Qt::Horizontal, QStringLiteral("local"));
                });
            QTimer::singleShot(2500, &window, [&window, move] {
                auto *drag = window.findChild<PaneDrag *>();
                auto *tabs = drag ? qobject_cast<QTabWidget *>(drag->parent()) : nullptr;
                auto panes = window.findChildren<TerminalPage *>();
                if (!drag || !tabs || panes.size() < 3)
                    return;
                const QString names = QStringLiteral("ABCDE");
                for (int index = 0; index < panes.size() && index < names.size(); ++index)
                    panes[index]->setObjectName(names.at(index));
                auto *a = window.findChild<TerminalPage *>(QStringLiteral("A"));
                auto *c = window.findChild<TerminalPage *>(QStringLiteral("C"));
                const auto describe = [](QWidget *widget) {
                    std::function<QString(QWidget *)> of = [&of](QWidget *at) -> QString {
                        if (auto *terminal = qobject_cast<TerminalPage *>(at))
                            return terminal->objectName();
                        if (auto *splitter = qobject_cast<QSplitter *>(at)) {
                            QStringList parts;
                            for (int index = 0; index < splitter->count(); ++index)
                                parts << of(splitter->widget(index));
                            return QStringLiteral("%1(%2)").arg(splitter->orientation() == Qt::Horizontal ? QStringLiteral("H")
                                                                                                         : QStringLiteral("V"),
                                                                 parts.join(QLatin1Char(',')));
                        }
                        for (auto *child : at->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
                            if (qobject_cast<QSplitter *>(child) || qobject_cast<TerminalPage *>(child))
                                return of(child);
                        return QStringLiteral("?");
                    };
                    return of(widget);
                };
                const auto print = [tabs, describe] {
                    int grips = 0;
                    for (auto *widget : tabs->findChildren<QWidget *>())
                        grips += widget->isVisible() && widget->toolTip() == QStringLiteral("Drag to move this pane");
                    std::fprintf(stderr, "test move: %d grips shown\n", grips);
                    for (int index = 0; index < tabs->count(); ++index)
                        std::fprintf(stderr, "test move: tab %d%s %s\n", index, index == tabs->currentIndex() ? "*" : "",
                                     qPrintable(describe(tabs->widget(index))));
                };
                print();
                const auto inA = [a](double x, double y) {
                    return a->mapToGlobal(QPoint(int(a->width() * x), int(a->height() * y)));
                };
                auto *bar = tabs->tabBar();
                if (move == QStringLiteral("group") || move == QStringLiteral("pull")) {
                    QWidget *second = tabs->widget(tabs->count() - 1);
                    if (move == QStringLiteral("pull")) {
                        // pull: as by the mouse, the second tab pressed and pulled down.
                        const QPoint on = bar->tabRect(tabs->count() - 1).center();
                        QMouseEvent press(QEvent::MouseButtonPress, QPointF(on), bar->mapToGlobal(QPointF(on)), Qt::LeftButton,
                                          Qt::LeftButton, Qt::NoModifier);
                        QApplication::sendEvent(bar, &press);
                        const QPoint below = on + QPoint(0, bar->height() + 40);
                        QMouseEvent pull(QEvent::MouseMove, QPointF(below), bar->mapToGlobal(QPointF(below)), Qt::NoButton,
                                         Qt::LeftButton, Qt::NoModifier);
                        QApplication::sendEvent(bar, &pull);
                        std::fprintf(stderr, "test move: dragging after the pull: %d\n", drag->dragging());
                    } else {
                        drag->start(second);
                    }
                    drag->hover(bar->mapToGlobal(bar->tabRect(0).center())); // tab 1 opens after a moment
                    QTimer::singleShot(800, drag, [drag, inA, print] {
                        drag->finish(inA(0.9, 0.5), true);
                        print();
                    });
                    return;
                }
                drag->start(c);
                if (move == QStringLiteral("hover")) {
                    drag->hover(inA(0.15, 0.5));
                    return;
                }
                const QPoint at = move == QStringLiteral("left")     ? inA(0.1, 0.5)
                                  : move == QStringLiteral("top")    ? inA(0.5, 0.1)
                                  : move == QStringLiteral("ontab")  ? bar->mapToGlobal(bar->tabRect(0).center())
                                                                     : tabs->mapToGlobal(QPoint(tabs->width() - 20, bar->geometry().center().y()));
                drag->finish(at, true);
                QTimer::singleShot(300, &window, print);
            });
        }
        // OPSREMOTE_TEST_PANE_BAR=top|bottom|off[@ms]: the pane bar moves there
        // after ms (2000 by default), as from Preferences (saved), to picture how
        // open panes follow.
        if (const auto bar = qEnvironmentVariable("OPSREMOTE_TEST_PANE_BAR"); !bar.isEmpty()) {
            const auto place = bar.section(QLatin1Char('@'), 0, 0);
            const int delay = bar.contains(QLatin1Char('@')) ? bar.section(QLatin1Char('@'), 1).toInt() : 2000;
            QTimer::singleShot(delay, &window, [&window, place] {
                auto preferences = loadStartupPreferences();
                preferences.messageStrip = place;
                window.setStartupPreferences(preferences);
            });
        }
        if (const int size = qEnvironmentVariableIntValue("OPSREMOTE_TEST_FONT_SIZE"); size > 0) {
            QTimer::singleShot(500, &window, [size] {
                auto font = QApplication::font();
                font.setPointSize(size);
                QApplication::setFont(font);
                refreshWidgetFonts();
            });
        }
        if (const auto page = qEnvironmentVariable("OPSREMOTE_TEST_PREFERENCES"); !page.isEmpty()) {
            // "main" pictures the main window instead, "modal" the open dialog,
            // "tunnels" the SSH Tunnels window.
            QTimer::singleShot(300, &window, [&window, page] {
                if (page != QStringLiteral("main") && page != QStringLiteral("modal")
                    && page != QStringLiteral("tunnels"))
                    window.showPreferences(page);
                if (qEnvironmentVariableIsSet("OPSREMOTE_TEST_ADMIN"))
                    window.openLocalTerminalAsAdministrator();
                // OPSREMOTE_TEST_SESSION_WINDOW=protocol: the New Session window
                // for that protocol (picture it with OPSREMOTE_TEST_PREFERENCES=modal).
                if (const auto protocol = qEnvironmentVariable("OPSREMOTE_TEST_SESSION_WINDOW"); !protocol.isEmpty())
                    QTimer::singleShot(0, &window, [&window, protocol] {
                        QMetaObject::invokeMethod(&window, "createSessionOver", Q_ARG(QWidget *, &window),
                                                  Q_ARG(QString, protocol));
                    });
                if (const auto text = qEnvironmentVariable("OPSREMOTE_TEST_TYPE"); !text.isEmpty()) {
                    QTimer::singleShot(3000, &window, [&window, text] {
                        if (auto *terminal = window.findChild<TerminalPage *>())
                            terminal->typeText(text + QLatin1Char('\r'));
                    });
                }
                if (qEnvironmentVariableIsSet("OPSREMOTE_TEST_SPLIT")) {
                    // OPSREMOTE_TEST_SPLIT_TARGET: the first tab (a quick-connect
                    // target) instead of a local terminal.
                    if (const auto target = qEnvironmentVariable("OPSREMOTE_TEST_SPLIT_TARGET"); !target.isEmpty())
                        window.openTarget(target);
                    else if (const auto path = qEnvironmentVariable("OPSREMOTE_TEST_SPLIT_SESSION"); !path.isEmpty())
                        window.openSessionByPath(path);
                    else
                        window.openLocalTerminal();
                    // OPSREMOTE_TEST_SPLIT=down-same: down with a local terminal,
                    // then right with the same session; otherwise right, then down.
                    if (qEnvironmentVariable("OPSREMOTE_TEST_SPLIT") == QStringLiteral("down-same")) {
                        // As by hand: a while apart, the second beside the first
                        // pane's saved session (OPSREMOTE_TEST_SPLIT_SESSION, by path).
                        QTimer::singleShot(1500, &window, [&window] {
                            window.openInSplit(Qt::Vertical, QStringLiteral("local"));
                            std::fprintf(stderr, "test split: down done\n");
                        });
                        QTimer::singleShot(3000, &window, [&window] {
                            QString id;
                            for (auto *terminal : window.findChildren<TerminalPage *>())
                                if (!terminal->property("sessionId").toString().isEmpty())
                                    id = terminal->property("sessionId").toString().section(QLatin1Char('#'), 0, 0);
                            std::fprintf(stderr, "test split: right with %s\n", qPrintable(id));
                            window.openInSplit(Qt::Horizontal, id);
                            std::fprintf(stderr, "test split: right done\n");
                        });
                    } else {
                        window.openInSplit(Qt::Horizontal, QStringLiteral("local"));
                        window.openInSplit(Qt::Vertical, QStringLiteral("local"));
                    }
                    if (qEnvironmentVariableIsSet("OPSREMOTE_TEST_MULTIEXEC")) {
                        for (auto *terminal : window.findChildren<TerminalPage *>())
                            terminal->setMultiExecTarget(true);
                        window.updateMultiExecIndicator();
                    }
                }
                if (page == QStringLiteral("tunnels")) {
                    for (auto *widget : QApplication::topLevelWidgets()) {
                        if (widget->windowTitle() == QStringLiteral("SSH Tunnels"))
                            widget->show();
                    }
                    // OPSREMOTE_TEST_TUNNEL_PICK=1: choose the last SSH server
                    // ("Other server…") through the open dropdown, as a click would;
                    // =new: choose "New…", fill in the window that opens (name
                    // OPSREMOTE_TEST_NEW_NAME, host 192.0.2.7) and save it.
                    if (const auto pick = qEnvironmentVariable("OPSREMOTE_TEST_TUNNEL_PICK"); !pick.isEmpty()) {
                        const bool makeNew = pick == QStringLiteral("new");
                        if (makeNew) {
                            QTimer::singleShot(900, &window, [] {
                                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                                if (!dialog) {
                                    std::fprintf(stderr, "tunnel new: no window opened\n");
                                    return;
                                }
                                auto fields = dialog->findChildren<QLineEdit *>();
                                if (!fields.isEmpty())
                                    fields.first()->setText(qEnvironmentVariable("OPSREMOTE_TEST_NEW_NAME"));
                                for (auto *field : fields)
                                    if (field->placeholderText() == QStringLiteral("hostname or IP address"))
                                        field->setText(QStringLiteral("192.0.2.7"));
                                QString protocol;
                                for (auto *box : dialog->findChildren<QComboBox *>())
                                    if (!box->isEnabled() && box->findText(QStringLiteral("SSH")) >= 0)
                                        protocol = box->currentText();
                                std::fprintf(stderr, "tunnel new: window \"%s\", protocol fixed to \"%s\"\n",
                                             qPrintable(dialog->windowTitle()), qPrintable(protocol));
                                if (auto *buttons = dialog->findChild<QDialogButtonBox *>())
                                    if (auto *save = buttons->button(QDialogButtonBox::Save))
                                        save->click();
                                QTimer::singleShot(400, [] {
                                    for (auto *widget : QApplication::topLevelWidgets()) {
                                        if (widget->windowTitle() != QStringLiteral("SSH Tunnels"))
                                            continue;
                                        for (auto *combo : widget->findChildren<QComboBox *>())
                                            if (combo->accessibleName() == QStringLiteral("SSH server"))
                                                std::fprintf(stderr, "tunnel new: chosen \"%s\"\n",
                                                             qPrintable(combo->currentText()));
                                    }
                                });
                            });
                        }
                        QTimer::singleShot(300, &window, [makeNew] {
                            for (auto *widget : QApplication::topLevelWidgets()) {
                                if (widget->windowTitle() != QStringLiteral("SSH Tunnels"))
                                    continue;
                                for (auto *combo : widget->findChildren<QComboBox *>()) {
                                    if (combo->accessibleName() != QStringLiteral("SSH server"))
                                        continue;
                                    combo->showPopup();
                                    auto *view = combo->view();
                                    int row = combo->count() - 1;
                                    if (makeNew)
                                        row = combo->findText(QStringLiteral("New…"));
                                    view->setCurrentIndex(view->model()->index(row, 0));
                                    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                                    QApplication::sendEvent(view, &press);
                                    std::fprintf(stderr, "tunnel pick: done\n");
                                    return;
                                }
                            }
                        });
                    }
                    // OPSREMOTE_TEST_TUNNEL_MORE=1: click More Options (the window
                    // should make room for them).
                    if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_TUNNEL_MORE") > 0) {
                        QTimer::singleShot(400, &window, [] {
                            for (auto *widget : QApplication::topLevelWidgets()) {
                                if (widget->windowTitle() != QStringLiteral("SSH Tunnels"))
                                    continue;
                                std::fprintf(stderr, "tunnel more: window height %d\n", widget->height());
                                for (auto *button : widget->findChildren<QAbstractButton *>())
                                    if (button->text() == QStringLiteral("More Options"))
                                        button->click();
                                QTimer::singleShot(300, widget, [widget] {
                                    std::fprintf(stderr, "tunnel more: window height %d after\n", widget->height());
                                });
                            }
                        });
                    }
                    // OPSREMOTE_TEST_TUNNEL_GROW=n: + Add (a forward) n times, then
                    // + New Tunnel n times, printing the heights after each.
                    if (const int times = qEnvironmentVariableIntValue("OPSREMOTE_TEST_TUNNEL_GROW"); times > 0) {
                        for (int step = 0; step < 2 * times; ++step) {
                            QTimer::singleShot(400 + step * 250, &window, [step, times] {
                                for (auto *widget : QApplication::topLevelWidgets()) {
                                    if (widget->windowTitle() != QStringLiteral("SSH Tunnels"))
                                        continue;
                                    for (auto *button : widget->findChildren<QToolButton *>()) {
                                        if (step < times && button->text() == QStringLiteral("+ Add"))
                                            button->click();
                                        if (step >= times && button->text() == QStringLiteral("+ New Tunnel")
                                            && button->menu())
                                            button->menu()->actions().first()->trigger();
                                    }
                                    QTimer::singleShot(200, widget, [widget, step] {
                                        QListWidget *forwards = nullptr;
                                        for (auto *list : widget->findChildren<QListWidget *>())
                                            if (list->accessibleName() == QStringLiteral("Forwards"))
                                                forwards = list;
                                        auto *tunnels = widget->findChild<QTreeWidget *>();
                                        std::fprintf(stderr,
                                                     "tunnel grow %d: window %d, forwards %d rows %d high%s, "
                                                     "tunnels %d rows %d high%s\n",
                                                     step + 1, widget->height(), forwards ? forwards->count() : -1,
                                                     forwards ? forwards->height() : -1,
                                                     forwards && forwards->verticalScrollBar()->isVisible()
                                                         ? " (scrolls)" : "",
                                                     tunnels ? tunnels->topLevelItemCount() : -1,
                                                     tunnels ? tunnels->height() : -1,
                                                     tunnels && tunnels->verticalScrollBar()->isVisible()
                                                         ? " (scrolls)" : "");
                                    });
                                }
                            });
                        }
                    }
                    // OPSREMOTE_TEST_TUNNEL_FORWARDS=1: click the second forward,
                    // then + Add, then Remove, printing the list after each.
                    if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_TUNNEL_FORWARDS") > 0) {
                        QTimer::singleShot(300, &window, [] {
                            for (auto *widget : QApplication::topLevelWidgets()) {
                                if (widget->windowTitle() != QStringLiteral("SSH Tunnels"))
                                    continue;
                                QListWidget *list = nullptr;
                                for (auto *candidate : widget->findChildren<QListWidget *>())
                                    if (candidate->accessibleName() == QStringLiteral("Forwards"))
                                        list = candidate;
                                if (!list)
                                    return;
                                const auto print = [list](const char *step) {
                                    QStringList rows;
                                    for (int i = 0; i < list->count(); ++i)
                                        rows << list->item(i)->text();
                                    std::fprintf(stderr, "tunnel forwards %s: current %d [%s]\n", step,
                                                 list->currentRow(), qPrintable(rows.join(QStringLiteral(" | "))));
                                };
                                const auto click = [](QWidget *target, QPoint point) {
                                    QMouseEvent press(QEvent::MouseButtonPress, point, target->mapToGlobal(point),
                                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                                    QApplication::sendEvent(target, &press);
                                    QMouseEvent release(QEvent::MouseButtonRelease, point, target->mapToGlobal(point),
                                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                                    QApplication::sendEvent(target, &release);
                                };
                                const auto button = [widget](const QString &text) -> QAbstractButton * {
                                    for (auto *candidate : widget->findChildren<QAbstractButton *>())
                                        if (candidate->text() == text)
                                            return candidate;
                                    return nullptr;
                                };
                                print("start");
                                click(list->viewport(), list->visualItemRect(list->item(1)).center());
                                print("clicked row 1");
                                if (auto *add = button(QStringLiteral("+ Add")))
                                    add->click();
                                print("added");
                                if (auto *remove = button(QStringLiteral("Remove")))
                                    remove->click();
                                print("removed");
                                return;
                            }
                        });
                    }
                }
                // OPSREMOTE_TEST_NEWSESSION=protocol: the New Session window with
                // that protocol, its visible tabs printed and pictured to
                // OPSREMOTE_TEST_NEWSESSION_PICTURE, then closed (OPSREMOTE_TEST_NEWSESSION_TABS:
                // tab names to open first).
                if (const auto protocol = qEnvironmentVariable("OPSREMOTE_TEST_NEWSESSION"); !protocol.isEmpty()) {
                    QTimer::singleShot(200, &window, [protocol] {
                        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                        if (!dialog)
                            return;
                        for (auto *box : dialog->findChildren<QComboBox *>())
                            if (box->findText(QStringLiteral("XDMCP")) >= 0 && box->findText(QStringLiteral("SSH")) >= 0)
                                box->setCurrentText(protocol);
                        // OPSREMOTE_TEST_NEWSESSION_TABS=a,b: these tabs opened first.
                        for (const auto &name : qEnvironmentVariable("OPSREMOTE_TEST_NEWSESSION_TABS").split(QLatin1Char(','), Qt::SkipEmptyParts))
                            for (auto *tabWidget : dialog->findChildren<QTabWidget *>())
                                for (int i = 0; i < tabWidget->count(); ++i)
                                    if (tabWidget->tabText(i).contains(name.trimmed(), Qt::CaseInsensitive))
                                        tabWidget->setCurrentIndex(i);
                        // OPSREMOTE_TEST_NEWSESSION_FILL=1: every second drop-down, number and
                        // switch given its own value, so pictures show both kinds of row.
                        if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_NEWSESSION_FILL")) {
                            int n = 0;
                            for (auto *box : dialog->findChildren<QComboBox *>())
                                if (!box->isEditable() && box->count() > 1 && n++ % 2)
                                    box->setCurrentIndex(1);
                            for (auto *box : dialog->findChildren<QSpinBox *>())
                                if (n++ % 2)
                                    box->setValue(box->minimum() + 3);
                            for (auto *box : dialog->findChildren<QCheckBox *>())
                                if (n++ % 2)
                                    box->toggle();
                        }
                        QTimer::singleShot(600, dialog, [dialog] {
                            QStringList tabs;
                            for (auto *tabWidget : dialog->findChildren<QTabWidget *>()) {
                                if (!tabWidget->isVisible())
                                    continue;
                                QStringList names;
                                for (int i = 0; i < tabWidget->count(); ++i)
                                    if (tabWidget->isTabVisible(i))
                                        names << tabWidget->tabText(i);
                                tabs << QStringLiteral("%1: %2").arg(tabWidget->objectName(), names.join(QStringLiteral(" · ")));
                            }
                            std::fprintf(stderr, "session tabs: %s\n", qPrintable(tabs.join(QStringLiteral(" || "))));
                            // OPSREMOTE_TEST_NEWSESSION_ROWS=1: every shown tab's cards and rows.
                            if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_NEWSESSION_ROWS")) {
                                for (auto *tabWidget : dialog->findChildren<QTabWidget *>()) {
                                    if (!tabWidget->isVisible())
                                        continue;
                                    for (int i = 0; i < tabWidget->count(); ++i) {
                                        if (!tabWidget->isTabVisible(i))
                                            continue;
                                        std::fprintf(stderr, "TAB %s\n", qPrintable(tabWidget->tabText(i)));
                                        for (auto *form : tabWidget->widget(i)->findChildren<QFormLayout *>()) {
                                            if (auto *card = qobject_cast<QFrame *>(form->parentWidget());
                                                card && card->objectName() == QStringLiteral("formCard"))
                                                if (auto *title = card->findChild<QLabel *>())
                                                    std::fprintf(stderr, "  CARD %s\n", qPrintable(title->text()));
                                            for (int row = 0; row < form->rowCount(); ++row) {
                                                QString label;
                                                if (auto *item = form->itemAt(row, QFormLayout::LabelRole); item && item->widget())
                                                    if (auto *text = qobject_cast<QLabel *>(item->widget()))
                                                        label = text->text();
                                                QString field;
                                                for (auto role : {QFormLayout::FieldRole, QFormLayout::SpanningRole}) {
                                                    auto *item = form->itemAt(row, role);
                                                    QWidget *w = item ? item->widget() : nullptr;
                                                    if (!w)
                                                        continue;
                                                    if (auto *box = qobject_cast<QCheckBox *>(w); box || (box = w->findChild<QCheckBox *>()))
                                                        field = QStringLiteral("[x] ") + box->text();
                                                    else if (auto *combo = qobject_cast<QComboBox *>(w); combo || (combo = w->findChild<QComboBox *>()))
                                                        field = QStringLiteral("(list) ") + combo->currentText();
                                                    else if (auto *spin = qobject_cast<QAbstractSpinBox *>(w); spin || (spin = w->findChild<QAbstractSpinBox *>()))
                                                        field = QStringLiteral("(number) ") + spin->text();
                                                    else if (auto *edit = qobject_cast<QLineEdit *>(w); edit || (edit = w->findChild<QLineEdit *>()))
                                                        field = QStringLiteral("(text) ") + edit->placeholderText();
                                                    else if (auto *text = qobject_cast<QLabel *>(w); text || (text = w->findChild<QLabel *>()))
                                                        field = QStringLiteral("(note) ") + text->text().left(70);
                                                    else
                                                        field = QStringLiteral("(%1)").arg(QString::fromLatin1(w->metaObject()->className()));
                                                }
                                                if (!label.isEmpty() || !field.isEmpty())
                                                    std::fprintf(stderr, "    %s %s\n", qPrintable(label), qPrintable(field));
                                            }
                                        }
                                    }
                                }
                            }
                            if (const auto file = qEnvironmentVariable("OPSREMOTE_TEST_NEWSESSION_PICTURE"); !file.isEmpty())
                                dialog->grab().save(file);
                            dialog->reject();
                        });
                    });
                    QMetaObject::invokeMethod(&window, "createSession");
                }
                if (const auto search = qEnvironmentVariable("OPSREMOTE_TEST_SEARCH"); !search.isEmpty()) {
                    if (auto *box = window.findChild<QLineEdit *>(QStringLiteral("sessionSearch")))
                        box->setText(search);
                }
                QTimer::singleShot(qEnvironmentVariableIntValue("OPSREMOTE_TEST_DELAY_MS") > 0 ? qEnvironmentVariableIntValue("OPSREMOTE_TEST_DELAY_MS") : 700, &window, [&window, page] {
                    const auto file = qEnvironmentVariable("OPSREMOTE_TEST_SCREENSHOT");
                    if (file.isEmpty())
                        return;
                    if (page == QStringLiteral("main")) {
                        window.grab().save(file);
                        return;
                    }
                    // "tunnels" pictures the SSH Tunnels window.
                    if (page == QStringLiteral("tunnels")) {
                        for (auto *widget : QApplication::topLevelWidgets()) {
                            if (widget->isVisible() && widget->windowTitle() == QStringLiteral("SSH Tunnels"))
                                widget->grab().save(file);
                        }
                        return;
                    }
                    // "modal" pictures the dialog that is open.
                    if (page == QStringLiteral("modal")) {
                        if (auto *dialog = QApplication::activeModalWidget())
                            dialog->grab().save(file);
                        return;
                    }
                    for (auto *widget : QApplication::topLevelWidgets()) {
                        if (widget->isVisible() && widget->windowTitle() == QStringLiteral("Preferences"))
                            widget->grab().save(file);
                    }
                });
            });
        }
        // OPSREMOTE_TEST_CLOSE_SIDE=menu item: four local terminals, then that
        // item of the second tab's menu ("Close Tabs to the Right"…); prints
        // the tabs left.
        if (const auto item = qEnvironmentVariable("OPSREMOTE_TEST_CLOSE_SIDE"); !item.isEmpty()) {
            QTimer::singleShot(300, &window, [&window, item] {
                for (int count = 0; count < 4; ++count)
                    window.openLocalTerminal();
                QTimer::singleShot(1500, &window, [&window, item] {
                    QTabWidget *tabs = nullptr; // the one holding the terminals
                    for (auto *candidate : window.findChildren<QTabWidget *>())
                        if (candidate->count() > 0 && qobject_cast<TerminalPage *>(candidate->widget(0)))
                            tabs = candidate;
                    if (!tabs)
                        return;
                    for (int index = 0; index < tabs->count(); ++index)
                        tabs->setTabText(index, QStringLiteral("T%1").arg(index + 1));
                    QTimer::singleShot(300, &window, [item] {
                        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                            for (auto *action : menu->actions())
                                if (action->text() == item) {
                                    std::fprintf(stderr, "test close side: %s enabled %d\n", qPrintable(item),
                                                 action->isEnabled());
                                    action->trigger();
                                }
                            menu->close();
                        }
                    });
                    QTimer::singleShot(900, &window, [tabs] {
                        QStringList left;
                        for (int index = 0; index < tabs->count(); ++index)
                            left << tabs->tabText(index);
                        std::fprintf(stderr, "test close side: tabs left %s\n", qPrintable(left.join(QLatin1Char(' '))));
                    });
                    emit tabs->tabBar()->customContextMenuRequested(tabs->tabBar()->tabRect(1).center());
                });
            });
        }
        // OPSREMOTE_TEST_RDP_PASTE=1: a remote desktop tab (to an address that
        // never answers) gets the clipboard events of a paste there: one paste
        // is cancelled here, another stopped by a clipboard of the server's own;
        // prints each transfer's state.
        if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_RDP_PASTE") > 0) {
            QTimer::singleShot(300, &window, [&window] {
                RdpConnectionSettings settings;
                settings.host = QStringLiteral("192.0.2.1");
                auto *page = new RdpPage(settings);
                window.openTestTab(page, QStringLiteral("RDP test"));
                auto *session = page->findChild<RdpSession *>();
                auto *list = TransferList::instance();
                const auto report = [list](const char *step) {
                    static const char *names[] = {"running", "done", "failed", "cancelled"};
                    for (const auto &transfer : list->transfers())
                        std::fprintf(stderr, "test paste: %s: %s %s%s\n", step, qPrintable(transfer.name),
                                     names[int(transfer.state)], transfer.cancel ? " (can cancel)" : "");
                };
                QTimer::singleShot(500, page, [session] { emit session->localFilesProgress(100, 1000, QStringLiteral("first.iso")); });
                QTimer::singleShot(800, page, [report, list] {
                    report("pasting");
                    list->cancel(list->running().first().id);
                    report("after cancel");
                });
                QTimer::singleShot(1100, page, [session] { emit session->localFilesProgress(200, 1000, QStringLiteral("second.iso")); });
                QTimer::singleShot(1400, page, [session] { emit session->localFilesWithdrawn(); });
                QTimer::singleShot(1700, page, [report] { report("after the server's own copy"); });
                // =2: a third paste that Windows stops asking for: failed after 30 s.
                if (qEnvironmentVariableIntValue("OPSREMOTE_TEST_RDP_PASTE") == 2) {
                    QTimer::singleShot(2000, page, [session] { emit session->localFilesProgress(300, 1000, QStringLiteral("third.iso")); });
                    for (int at : {5000, 7000, 15000, 25000, 31000})
                        QTimer::singleShot(at, page, [report, at] { report(qPrintable(QStringLiteral("at %1 s").arg(at / 1000))); });
                    QTimer::singleShot(33000, page, [report] { report("32 s without requests"); });
                }
            });
        }
        // For automated tests: close the window like the user would.
        if (const int closeAfter = qEnvironmentVariableIntValue("OPSREMOTE_TEST_CLOSE_MS");
            closeAfter > 0)
            QTimer::singleShot(closeAfter, &window, &QWidget::close);
        result = application.exec();
        qInfo("shutdown: event loop ended");
        // Whatever hangs from here on (window teardown, a worker thread stuck
        // in libssh or OpenSSL), the process must not outlive its window: a
        // watchdog ends it after a grace period.
        std::thread([result] {
            std::this_thread::sleep_for(std::chrono::seconds(8));
            endProcess(result);
        }).detach();
    }

    // Finish SSH and FTP connections so servers see a clean logout, and end
    // the kept worker threads: exiting while a worker is still inside libssh
    // or OpenSSL can hang the process on Windows. Settings were saved when
    // the window closed, so if a worker is stuck (for example in a connection
    // attempt), end at once.
    QElapsedTimer shutdownClock;
    shutdownClock.start();
    const bool finished = WorkerThreads::shutdownAll(5000);
    qInfo("shutdown: worker threads %s after %lld ms", finished ? "ended" : "still running",
          qlonglong(shutdownClock.elapsed()));
    if (loadBackupPreferences().removeRuntimeOnExit)
        scheduleRuntimeRemoval();
#ifdef Q_OS_WIN
    // Library shutdown on Windows is where the process used to hang; nothing
    // after this point needs to run.
    endProcess(result);
#endif
    if (!finished)
        endProcess(result);
    return result;
}
