// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "localresources.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTcpSocket>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace LocalResources {

#ifdef Q_OS_WIN

namespace {

// A Windows service's state, or 0 when it cannot be read.
DWORD serviceState(const wchar_t *name)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        return 0;
    DWORD state = 0;
    if (SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS status{};
        if (QueryServiceStatus(service, &status))
            state = status.dwCurrentState;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return state;
}

}

QString soundProblem()
{
    // winmm is part of Windows; it only needs an output device.
    using Count = UINT(WINAPI *)();
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    if (!winmm)
        return QStringLiteral("Windows sound (winmm.dll) is not available.");
    const auto count = reinterpret_cast<Count>(reinterpret_cast<void *>(GetProcAddress(winmm, "waveOutGetNumDevs")));
    const UINT devices = count ? count() : 0;
    FreeLibrary(winmm);
    return devices > 0 ? QString() : QStringLiteral("This computer has no sound output device.");
}

QString printerProblem()
{
    const DWORD state = serviceState(L"Spooler");
    return state == 0 || state == SERVICE_RUNNING
               ? QString()
               : QStringLiteral("The Windows Print Spooler service is not running.");
}

QString smartCardProblem()
{
    // The Smart Card service starts when a reader is attached.
    const DWORD state = serviceState(L"SCardSvr");
    if (state == SERVICE_RUNNING)
        return {};
    return QStringLiteral("The Windows Smart Card service is not running (it starts when a "
                          "smart-card reader is connected).");
}

#else

QString soundProblem()
{
    // The PulseAudio interface, served by PulseAudio or by PipeWire's pulse
    // server: its socket in the user's runtime folder, or a server named in
    // PULSE_SERVER.
    if (!qEnvironmentVariableIsEmpty("PULSE_SERVER"))
        return {};
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtime.isEmpty() && QFileInfo::exists(QDir(runtime).filePath(QStringLiteral("pulse/native"))))
        return {};
    return QStringLiteral("No sound server is running for this user (PulseAudio, or PipeWire with "
                          "pipewire-pulse).");
}

QString printerProblem()
{
    if (!qEnvironmentVariableIsEmpty("CUPS_SERVER"))
        return {};
    for (const char *socket : {"/run/cups/cups.sock", "/var/run/cups/cups.sock"}) {
        if (QFileInfo::exists(QString::fromLatin1(socket)))
            return {};
    }
    return QStringLiteral("CUPS, the printing service, is not running on this computer.");
}

QString smartCardProblem()
{
    // FreeRDP opens the PC/SC library when it starts, so look for it the same
    // way; pcscd itself is started through its socket when needed.
    void *library = dlopen("libpcsclite.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!library)
        return QStringLiteral("The PC/SC library (pcsc-lite) is not installed.");
    dlclose(library);
    for (const char *socket : {"/run/pcscd/pcscd.comm", "/var/run/pcscd/pcscd.comm"}) {
        if (QFileInfo::exists(QString::fromLatin1(socket)))
            return {};
    }
    return QStringLiteral("The smart-card service pcscd is not running (install pcsc-lite and "
                          "enable pcscd).");
}

#endif

QString x11Problem()
{
    const QString display = qEnvironmentVariable("DISPLAY").trimmed();
#ifdef Q_OS_WIN
    // VcXsrv and Xming listen on TCP port 6000 + the display number.
    const int number = (display.isEmpty() ? QStringLiteral(":0") : display)
                           .section(QLatin1Char(':'), 1).section(QLatin1Char('.'), 0, 0).toInt();
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), quint16(6000 + number));
    if (socket.waitForConnected(300))
        return {};
    return QStringLiteral("No X server is running on this computer (start VcXsrv or Xming, "
                          "allowing connections).");
#else
    if (display.isEmpty())
        return QStringLiteral("No X server: DISPLAY is not set (on Wayland, XWayland provides one).");
    const QString host = display.section(QLatin1Char(':'), 0, 0);
    if (!host.isEmpty() && host != QStringLiteral("unix"))
        return {}; // a remote or TCP display; tried when used
    const QString number = display.section(QLatin1Char(':'), 1).section(QLatin1Char('.'), 0, 0);
    if (QFileInfo::exists(QStringLiteral("/tmp/.X11-unix/X") + number))
        return {};
    return QStringLiteral("No X server answers for DISPLAY %1.").arg(display);
#endif
}

QString moshProblem()
{
    if (!QStandardPaths::findExecutable(QStringLiteral("mosh")).isEmpty())
        return {};
#ifdef Q_OS_WIN
    return QStringLiteral("The mosh program is not installed (Windows has none of its own; one "
                          "from Cygwin or MSYS2 on the PATH works).");
#else
    return QStringLiteral("The mosh program is not installed (the \"mosh\" package; the server "
                          "needs it too).");
#endif
}

}
