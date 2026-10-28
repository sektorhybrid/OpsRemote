// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "mainwindow.h"
#include "preferences.h"
#include "sshsession.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
// OPSREMOTE_LOG=<file> appends Qt's messages to that file, which helps
// diagnose problems where no console is attached (the Windows GUI program).
// Deliberately never deleted: Qt may log until the very end of the process,
// after static objects are destroyed; the system closes the file.
QFile *logFile = nullptr;

void writeLog(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (!logFile)
        return;
    logFile->write(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz ")).toUtf8()
                   + message.toUtf8() + '\n');
    logFile->flush();
}

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
    // PORTABLE_TERMINAL_LOG is the name from before the rename to OpsRemote.
    QString path = qEnvironmentVariable("OPSREMOTE_LOG");
    if (path.isEmpty())
        path = qEnvironmentVariable("PORTABLE_TERMINAL_LOG");
    if (!path.isEmpty()) {
        logFile = new QFile(path);
        if (logFile->open(QIODevice::Append | QIODevice::Text))
            qInstallMessageHandler(writeLog);
    }
    QApplication application(argc, argv);
    QApplication::setApplicationName("OpsRemote");
    QApplication::setOrganizationName("OpsRemote");

    // A bundled OpenSSL looks for its providers at the path it was built
    // with; use the bundle's copy of the legacy provider (MD4 for NTLM).
    for (const auto &folder : {QStringLiteral("lib/ossl-modules"), QStringLiteral("ossl-modules")}) {
        const QDir modules(QDir(QCoreApplication::applicationDirPath()).filePath(folder));
        if (qEnvironmentVariableIsEmpty("OPENSSL_MODULES") && modules.exists())
            qputenv("OPENSSL_MODULES", QDir::toNativeSeparators(modules.absolutePath()).toLocal8Bit());
    }

    int result = 0;
    {
        MainWindow window;
        window.show();
        // For automated tests: close the window like the user would.
        if (const int closeAfter = qEnvironmentVariableIntValue("OPSREMOTE_TEST_CLOSE_MS");
            closeAfter > 0)
            QTimer::singleShot(closeAfter, &window, &QWidget::close);
        result = application.exec();
        // Whatever hangs from here on (window teardown, a worker thread stuck
        // in libssh or OpenSSL), the process must not outlive its window: a
        // watchdog ends it after a grace period.
        std::thread([result] {
            std::this_thread::sleep_for(std::chrono::seconds(8));
            endProcess(result);
        }).detach();
    }

    // Finish SSH connections so servers see a clean logout. Settings were
    // saved when the window closed, so if a worker is stuck (for example in a
    // connection attempt), end at once.
    const bool finished = SshSession::shutdownAll(5000);
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
