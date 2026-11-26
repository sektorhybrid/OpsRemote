// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "desktopentry.h"
#include "stallwatch.h"
#include "windowfit.h"
#include "workerthreads.h"
#include "localsessions.h"
#include "mainwindow.h"
#include "preferences.h"
#include "settingsmigrations.h"

#include "testhooks.h"
#ifdef Q_OS_WIN
#include "conpty.h" // runElevatedConsoleHelper
#endif

#include <QApplication>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QMutex>
#include <QMutexLocker>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QThread>

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
                qInfo().noquote() << "Busy pointer:" << (cursor.hCursor == wait ? "wait" : "starting") << "over"
                                  << QString::fromWCharArray(title) << "| programs started lately:"
                                  << (state->started.isEmpty() ? QStringLiteral("none")
                                                               : state->started.join(QStringLiteral("; ")));
                state->started.clear();
            } else {
                qInfo("Busy pointer: ended");
            }
        });
        timer->start(500);
    });
    const auto stop = [thread] {
        if (!thread->isRunning())
            return;
        thread->quit();
        if (!thread->wait(1000))
            qWarning("Busy pointer: the watch did not finish before the shutdown deadline");
    };
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, thread, stop);
    // Test modes return without running the event loop (no aboutToQuit):
    // stopped before its owner deletes it (destroyed comes before that).
    QObject::connect(owner, &QObject::destroyed, thread, stop);
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

#ifdef OPSREMOTE_COVERAGE
// Coverage builds only (test runs): a test that ends the program with TERM
// still gets the coverage data of what ran.
extern "C" void __gcov_dump(void);
static void dumpCoverageAndExit(int)
{
    __gcov_dump();
    std::_Exit(143);
}
#endif

int main(int argc, char *argv[])
{
#ifdef OPSREMOTE_COVERAGE
    std::signal(SIGTERM, dumpCoverageAndExit);
#endif
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
    // Test hooks (testhooks.h): a test that needs no window ends here.
    if (const int status = runHeadlessTest(); status >= 0)
        return status;
    QApplication::setApplicationName("OpsRemote");
    QApplication::setOrganizationName("OpsRemote");
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/icons/opsremote-%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(icon);
    // Windows open within the screen's free area (small screens, docks).
    WindowFit::install();
    // Wayland's app id, matching the desktop file of DesktopEntry.
    QGuiApplication::setDesktopFileName(QLatin1String(DesktopEntry::Id));

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
    const QStringList migrated = migrateSettings();
    forgetCachedSettings(); // read before (startup keys): take the migrated file
    for (const auto &line : migrated)
        qInfo("Settings migration: %s", qUtf8Printable(line));
    // The launcher removes the unpacked copy after this program ends, even
    // when it is killed or crashes, when it finds the mark.
    markRuntimeForRemoval(loadBackupPreferences().removeRuntimeOnExit);
    removeLeftoverKubeconfigs();
    // Before the first window, so the taskbar finds the entry for it.
    if (const auto entry = DesktopEntry::update(loadStartupPreferences().taskbarIcon); !entry.isEmpty())
        qInfo("Taskbar icon: %s", qUtf8Printable(entry));

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
        // Test hooks (testhooks.h) for the shown window.
        installWindowTestHooks(window);
        result = application.exec();
        qInfo("Shutdown: event loop ended");
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
    qInfo("Shutdown: worker threads %s after %lld ms", finished ? "ended" : "still running",
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
