// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "mainwindow.h"
#ifdef Q_OS_WIN
#include "conpty.h"
#endif
#include "filetransfertest.h"
#include "preferences.h"
#include "sshsession.h"
#include "systemcredentials.h"
#include "terminalpage.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTabWidget>
#include <QPushButton>
#include <QDateTime>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QMenu>
#include <QScrollBar>
#include <QTimer>
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

void writeLog(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (!logFile)
        return;
    logFile->write(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz ")).toUtf8()
                   + message.toUtf8() + '\n');
    logFile->flush();
}

#ifdef Q_OS_WIN
// With OPSREMOTE_LOG: notes when Windows shows its busy or "starting"
// pointer, and which programs started shortly before, to find what causes it.
void watchBusyPointer(QObject *owner)
{
    struct State
    {
        bool busy = false;
        QHash<DWORD, QString> processes;
        QStringList started; // in the last few checks
    };
    auto state = std::make_shared<State>();
    const auto snapshot = [] {
        QHash<DWORD, QString> processes;
        HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (handle == INVALID_HANDLE_VALUE)
            return processes;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof entry;
        for (BOOL more = Process32FirstW(handle, &entry); more; more = Process32NextW(handle, &entry))
            processes.insert(entry.th32ProcessID,
                             QStringLiteral("%1 (%2, parent %3)")
                                 .arg(QString::fromWCharArray(entry.szExeFile))
                                 .arg(entry.th32ProcessID)
                                 .arg(entry.th32ParentProcessID));
        CloseHandle(handle);
        return processes;
    };
    state->processes = snapshot();
    auto *timer = new QTimer(owner);
    QObject::connect(timer, &QTimer::timeout, owner, [state, snapshot] {
        const auto now = snapshot();
        for (auto it = now.cbegin(); it != now.cend(); ++it)
            if (!state->processes.contains(it.key()))
                state->started << it.value();
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
            POINT point = cursor.ptScreenPos;
            wchar_t title[256] = {};
            GetWindowTextW(GetAncestor(WindowFromPoint(point), GA_ROOT), title, 256);
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
#ifdef Q_OS_WIN
    if (logFile)
        watchBusyPointer(&application);
#endif
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
        if (const auto page = qEnvironmentVariable("OPSREMOTE_TEST_PREFERENCES"); !page.isEmpty()) {
            // "main" pictures the main window instead, "modal" the open dialog,
            // "tunnels" the SSH Tunnels window.
            QTimer::singleShot(300, &window, [&window, page] {
                if (page != QStringLiteral("main") && page != QStringLiteral("modal")
                    && page != QStringLiteral("tunnels"))
                    window.showPreferences(page);
                if (qEnvironmentVariableIsSet("OPSREMOTE_TEST_ADMIN"))
                    window.openLocalTerminalAsAdministrator();
                if (const auto text = qEnvironmentVariable("OPSREMOTE_TEST_TYPE"); !text.isEmpty()) {
                    QTimer::singleShot(3000, &window, [&window, text] {
                        if (auto *terminal = window.findChild<TerminalPage *>())
                            terminal->typeText(text + QLatin1Char('\r'));
                    });
                }
                if (qEnvironmentVariableIsSet("OPSREMOTE_TEST_SPLIT")) {
                    window.openLocalTerminal();
                    window.openInSplit(Qt::Horizontal, QStringLiteral("local"));
                    window.openInSplit(Qt::Vertical, QStringLiteral("local"));
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
                // OPSREMOTE_TEST_NEWSESSION_PICTURE, then closed.
                if (const auto protocol = qEnvironmentVariable("OPSREMOTE_TEST_NEWSESSION"); !protocol.isEmpty()) {
                    QTimer::singleShot(200, &window, [protocol] {
                        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                        if (!dialog)
                            return;
                        for (auto *box : dialog->findChildren<QComboBox *>())
                            if (box->findText(QStringLiteral("XDMCP")) >= 0 && box->findText(QStringLiteral("SSH")) >= 0)
                                box->setCurrentText(protocol);
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
