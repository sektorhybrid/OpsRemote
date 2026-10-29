// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "conpty.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QPointer>
#include <QUuid>
#include <QRegularExpression>

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <mutex>

namespace {

// The pseudo console functions, looked up at run time so OpsRemote still
// starts (without local terminals) where kernel32 lacks them.
using PseudoConsole = void *;
using CreateFunction = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, PseudoConsole *);
using ResizeFunction = HRESULT(WINAPI *)(PseudoConsole, COORD);
using CloseFunction = void(WINAPI *)(PseudoConsole);

struct Functions
{
    CreateFunction create = nullptr;
    ResizeFunction resize = nullptr;
    CloseFunction close = nullptr;
};

const Functions &functions()
{
    static const Functions loaded = [] {
        Functions result;
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            result.create = reinterpret_cast<CreateFunction>(
                reinterpret_cast<void *>(GetProcAddress(kernel, "CreatePseudoConsole")));
            result.resize = reinterpret_cast<ResizeFunction>(
                reinterpret_cast<void *>(GetProcAddress(kernel, "ResizePseudoConsole")));
            result.close = reinterpret_cast<CloseFunction>(
                reinterpret_cast<void *>(GetProcAddress(kernel, "ClosePseudoConsole")));
        }
        return result;
    }();
    return loaded;
}

// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, for older headers.
constexpr DWORD_PTR PseudoConsoleAttribute = 0x00020016;

// One argument, quoted the way CommandLineToArgvW reads it back.
QString quoted(const QString &argument)
{
    if (!argument.isEmpty() && !argument.contains(QRegularExpression(QStringLiteral("[\\s\"]"))))
        return argument;
    QString result = QStringLiteral("\"");
    int backslashes = 0;
    for (const QChar c : argument) {
        if (c == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (c == QLatin1Char('"'))
            result += QString(backslashes * 2 + 1, QLatin1Char('\\'));
        else
            result += QString(backslashes, QLatin1Char('\\'));
        backslashes = 0;
        result += c;
    }
    result += QString(backslashes * 2, QLatin1Char('\\'));
    result += QLatin1Char('"');
    return result;
}


COORD size(int columns, int rows)
{
    return {SHORT(qBound(1, columns, 32767)), SHORT(qBound(1, rows, 32767))};
}

bool isConsoleAvailable()
{
    const auto &api = functions();
    return api.create && api.resize && api.close;
}

// Starts a command line attached to a pseudo console, in folder (or the
// current one when empty).
bool startInConsole(PseudoConsole console, const QString &commandLine, const QString &folder,
                    PROCESS_INFORMATION *process)
{
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    QByteArray attributeBuffer(qsizetype(attributeSize), '\0');
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
        return false;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = attributes;
    std::wstring command = commandLine.toStdWString();
    const std::wstring directory = QDir::toNativeSeparators(folder).toStdWString();
    const bool started =
        UpdateProcThreadAttribute(attributes, 0, PseudoConsoleAttribute, console, sizeof console,
                                  nullptr, nullptr)
        && CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                          EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                          directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo,
                          process);
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    SetLastError(error);
    return started;
}

}

bool ConPtyConnection::isAvailable()
{
    return isConsoleAvailable();
}

ConPtyConnection::ConPtyConnection(const QString &program, const QStringList &arguments,
                                   const QString &label, const QString &workingDirectory,
                                   QObject *parent)
    : StreamConnection(parent), m_program(program), m_arguments(arguments), m_label(label),
      m_workingDirectory(workingDirectory)
{
}

ConPtyConnection::~ConPtyConnection()
{
    close();
}

QString ConPtyConnection::description() const
{
    return m_label;
}

void ConPtyConnection::open()
{
    close();
    const auto &api = functions();
    if (!isAvailable()) {
        emit disconnected(QStringLiteral("Local terminals need Windows 10 version 1809 or newer."), true);
        return;
    }
    // Two pipes: the program's input (we write) and output (we read).
    HANDLE inputRead = nullptr, inputWrite = nullptr, outputRead = nullptr, outputWrite = nullptr;
    if (!CreatePipe(&inputRead, &inputWrite, nullptr, 0)
        || !CreatePipe(&outputRead, &outputWrite, nullptr, 0)) {
        emit disconnected(QStringLiteral("Cannot create the pipes for %1.").arg(m_label), true);
        return;
    }
    PseudoConsole console = nullptr;
    const HRESULT created = api.create(size(m_columns, m_rows), inputRead, outputWrite, 0, &console);
    // The pseudo console keeps its own copies of its ends.
    CloseHandle(inputRead);
    CloseHandle(outputWrite);
    if (FAILED(created)) {
        CloseHandle(inputWrite);
        CloseHandle(outputRead);
        emit disconnected(QStringLiteral("Cannot create a pseudo console (error 0x%1).")
                              .arg(quint32(created), 8, 16, QLatin1Char('0')),
                          true);
        return;
    }

    QString commandLine = quoted(QDir::toNativeSeparators(m_program));
    for (const auto &argument : std::as_const(m_arguments))
        commandLine += QLatin1Char(' ') + quoted(argument);
    PROCESS_INFORMATION process{};
    const bool started = startInConsole(console, commandLine, m_workingDirectory, &process);
    const DWORD error = GetLastError();
    if (!started) {
        api.close(console);
        CloseHandle(inputWrite);
        CloseHandle(outputRead);
        emit disconnected(QStringLiteral("Cannot start %1 (error %2).")
                              .arg(QDir::toNativeSeparators(m_program))
                              .arg(error),
                          true);
        return;
    }
    CloseHandle(process.hThread);
    m_console = console;
    m_input = inputWrite;
    m_output = outputRead;
    m_process = process.hProcess;
    m_closing = false;

    // Output is read on a thread of its own (ReadFile blocks) and handed to
    // this object's thread.
    QPointer<ConPtyConnection> self(this);
    m_reader = std::thread([self, output = outputRead] {
        char buffer[16384];
        DWORD read = 0;
        while (ReadFile(output, buffer, sizeof buffer, &read, nullptr) && read > 0) {
            QByteArray data(buffer, int(read));
            QMetaObject::invokeMethod(
                self, [self, data] { if (self) emit self->dataReceived(data); },
                Qt::QueuedConnection);
        }
    });
    m_waiter = std::thread([self, handle = process.hProcess] {
        WaitForSingleObject(handle, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(handle, &code);
        QMetaObject::invokeMethod(
            self, [self, code] { if (self) self->finished(code); }, Qt::QueuedConnection);
    });
    emit connected();
}

void ConPtyConnection::finished(unsigned long exitCode)
{
    if (m_closing)
        return;
    close();
    emit disconnected(exitCode == 0 ? QStringLiteral("%1 ended.").arg(m_label)
                                    : QStringLiteral("%1 ended (exit code %2).").arg(m_label).arg(exitCode),
                      exitCode != 0);
}

void ConPtyConnection::write(const QByteArray &data)
{
    if (!m_input || data.isEmpty())
        return;
    DWORD written = 0;
    WriteFile(m_input, data.constData(), DWORD(data.size()), &written, nullptr);
}

void ConPtyConnection::resize(int columns, int rows)
{
    m_columns = columns;
    m_rows = rows;
    if (m_console)
        functions().resize(m_console, size(columns, rows));
}

void ConPtyConnection::close()
{
    if (!m_console && !m_process)
        return;
    m_closing = true;
    // Closing the pseudo console ends its programs; the reader keeps
    // draining the output meanwhile, as older Windows versions need.
    if (m_process)
        TerminateProcess(m_process, 0);
    if (m_console)
        functions().close(m_console);
    m_console = nullptr;
    if (m_input)
        CloseHandle(m_input);
    m_input = nullptr;
    if (m_reader.joinable())
        m_reader.join();
    if (m_waiter.joinable())
        m_waiter.join();
    if (m_output)
        CloseHandle(m_output);
    m_output = nullptr;
    if (m_process)
        CloseHandle(m_process);
    m_process = nullptr;
}

// ---- Running as administrator ----

namespace {

// Frames on the helper's pipes: a type byte, a 4-byte little-endian
// length, then the payload. To the helper: 'D' keys, 'R' size (two 16-bit
// numbers). From it: 'O' screen output, 'X' exit code (32 bits).
bool writeAll(HANDLE pipe, const char *data, DWORD size)
{
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, data, size, &written, nullptr) || written == 0)
            return false;
        data += written;
        size -= written;
    }
    return true;
}

bool readAll(HANDLE pipe, char *data, DWORD size)
{
    while (size > 0) {
        DWORD read = 0;
        if (!ReadFile(pipe, data, size, &read, nullptr) || read == 0)
            return false;
        data += read;
        size -= read;
    }
    return true;
}

bool writeFrame(HANDLE pipe, char type, const char *data, quint32 size)
{
    char header[5] = {type, char(size & 0xff), char((size >> 8) & 0xff), char((size >> 16) & 0xff),
                      char((size >> 24) & 0xff)};
    return writeAll(pipe, header, sizeof header) && (size == 0 || writeAll(pipe, data, size));
}

constexpr quint32 MaximumFrame = 1 << 20;

bool readFrame(HANDLE pipe, char *type, std::string *payload)
{
    char header[5];
    if (!readAll(pipe, header, sizeof header))
        return false;
    *type = header[0];
    const quint32 size = quint32(uchar(header[1])) | quint32(uchar(header[2])) << 8
                         | quint32(uchar(header[3])) << 16 | quint32(uchar(header[4])) << 24;
    if (size > MaximumFrame)
        return false;
    payload->resize(size);
    return size == 0 || readAll(pipe, payload->data(), size);
}

std::wstring pipePath(const std::wstring &name, const wchar_t *direction)
{
    return L"\\\\.\\pipe\\" + name + direction;
}

}

QString windowsSudoPath()
{
    return QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")))
           + QStringLiteral("/System32/sudo.exe");
}

bool windowsSudoInlineEnabled()
{
    DWORD value = 0;
    DWORD size = sizeof value;
    // 1: in a new window, 2: with input closed, 3: inline.
    const bool read = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Sudo",
                                   L"Enabled", RRF_RT_REG_DWORD, nullptr, &value, &size)
                      == ERROR_SUCCESS;
    return read && value == 3 && QFile::exists(windowsSudoPath());
}

ElevatedConsoleConnection::ElevatedConsoleConnection(const QString &program,
                                                     const QStringList &arguments,
                                                     const QString &label,
                                                     const QString &workingDirectory, QObject *parent)
    : StreamConnection(parent), m_program(program), m_arguments(arguments), m_label(label),
      m_workingDirectory(workingDirectory)
{
}

ElevatedConsoleConnection::~ElevatedConsoleConnection()
{
    close();
}

QString ElevatedConsoleConnection::description() const
{
    return m_label;
}

void ElevatedConsoleConnection::open()
{
    close();
    m_closing = false;
    m_connected = false;
    m_pipeName = QStringLiteral("opsremote-elevated-") + QUuid::createUuid().toString(QUuid::Id128);
    const std::wstring name = m_pipeName.toStdWString();
    // One instance each, never shared with a pipe someone else made first,
    // and only for this computer.
    const auto create = [](const std::wstring &path, DWORD direction) {
        return CreateNamedPipeW(path.c_str(), direction | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                                    | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 65536, 65536, 0, nullptr);
    };
    HANDLE toHelper = create(pipePath(name, L"-in"), PIPE_ACCESS_OUTBOUND);
    HANDLE fromHelper = create(pipePath(name, L"-out"), PIPE_ACCESS_INBOUND);
    if (toHelper == INVALID_HANDLE_VALUE || fromHelper == INVALID_HANDLE_VALUE) {
        if (toHelper != INVALID_HANDLE_VALUE)
            CloseHandle(toHelper);
        if (fromHelper != INVALID_HANDLE_VALUE)
            CloseHandle(fromHelper);
        emit disconnected(QStringLiteral("Cannot create the pipes for the administrator terminal."), true);
        return;
    }

    // The elevated helper: this same program, through UAC.
    QString parameters = QStringLiteral("--elevated-console %1 %2 %3 %4 ")
                             .arg(m_pipeName)
                             .arg(GetCurrentProcessId())
                             .arg(m_columns)
                             .arg(m_rows);
    parameters += quoted(QDir::toNativeSeparators(m_workingDirectory)) + QLatin1Char(' ')
                  + quoted(QDir::toNativeSeparators(m_program));
    for (const auto &argument : std::as_const(m_arguments))
        parameters += QLatin1Char(' ') + quoted(argument);
    const std::wstring file = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    const std::wstring arguments = parameters.toStdWString();
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof execute;
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    // OPSREMOTE_TEST_ELEVATION=open starts the helper without elevation, so
    // tests can check the relay where nobody can answer UAC.
    const bool testWithoutElevation = qEnvironmentVariable("OPSREMOTE_TEST_ELEVATION") == QStringLiteral("open");
    execute.lpVerb = testWithoutElevation ? L"open" : L"runas";
    execute.lpFile = file.c_str();
    execute.lpParameters = arguments.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute) || !execute.hProcess) {
        const DWORD error = GetLastError();
        CloseHandle(toHelper);
        CloseHandle(fromHelper);
        emit disconnected(error == ERROR_CANCELLED
                              ? QStringLiteral("Windows did not give administrator rights (cancelled).")
                              : QStringLiteral("Cannot start the administrator terminal (error %1).").arg(error),
                          error != ERROR_CANCELLED);
        return;
    }
    m_toHelper = toHelper;
    m_fromHelper = fromHelper;
    m_helper = execute.hProcess;
    const DWORD helperId = GetProcessId(execute.hProcess);

    // Waits for the helper on both pipes, checks it is the process started
    // above, then reads its frames.
    QPointer<ElevatedConsoleConnection> self(this);
    m_reader = std::thread([this, self, toHelper, fromHelper, helperId] {
        const auto accept = [helperId](HANDLE pipe) {
            if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
                return false;
            ULONG client = 0;
            return GetNamedPipeClientProcessId(pipe, &client) && client == helperId;
        };
        const auto fail = [self](const QString &reason) {
            QMetaObject::invokeMethod(
                self, [self, reason] { if (self) emit self->disconnected(reason, true); },
                Qt::QueuedConnection);
        };
        if (!accept(toHelper) || !accept(fromHelper)) {
            if (!m_closing)
                fail(QStringLiteral("The administrator terminal did not start, or another program "
                                    "answered in its place."));
            return;
        }
        m_connected = true;
        QMetaObject::invokeMethod(
            self, [self] { if (self) emit self->connected(); }, Qt::QueuedConnection);
        char type = 0;
        std::string payload;
        while (readFrame(fromHelper, &type, &payload)) {
            if (type == 'O') {
                QByteArray data(payload.data(), qsizetype(payload.size()));
                QMetaObject::invokeMethod(
                    self, [self, data] { if (self) emit self->dataReceived(data); },
                    Qt::QueuedConnection);
            } else if (type == 'X' && payload.size() == 4) {
                const unsigned long code = quint32(uchar(payload[0])) | quint32(uchar(payload[1])) << 8
                                           | quint32(uchar(payload[2])) << 16
                                           | quint32(uchar(payload[3])) << 24;
                QMetaObject::invokeMethod(
                    self, [self, code] { if (self) self->finished(code); }, Qt::QueuedConnection);
                return;
            }
        }
        if (!m_closing)
            QMetaObject::invokeMethod(
                self, [self] { if (self) self->finished(1); }, Qt::QueuedConnection);
    });
}

void ElevatedConsoleConnection::sendFrame(char type, const QByteArray &payload)
{
    if (m_connected && m_toHelper)
        writeFrame(m_toHelper, type, payload.constData(), quint32(payload.size()));
}

void ElevatedConsoleConnection::write(const QByteArray &data)
{
    if (!data.isEmpty())
        sendFrame('D', data);
}

void ElevatedConsoleConnection::resize(int columns, int rows)
{
    m_columns = columns;
    m_rows = rows;
    const quint16 c = quint16(qBound(1, columns, 32767)), r = quint16(qBound(1, rows, 32767));
    const char size[4] = {char(c & 0xff), char(c >> 8), char(r & 0xff), char(r >> 8)};
    sendFrame('R', QByteArray(size, 4));
}

void ElevatedConsoleConnection::finished(unsigned long exitCode)
{
    if (m_closing)
        return;
    close();
    emit disconnected(exitCode == 0 ? QStringLiteral("%1 ended.").arg(m_label)
                                    : QStringLiteral("%1 ended (exit code %2).").arg(m_label).arg(exitCode),
                      exitCode != 0);
}

void ElevatedConsoleConnection::close()
{
    if (!m_toHelper && !m_fromHelper)
        return;
    m_closing = true;
    // A reader still waiting for the helper is woken by connecting to our
    // own pipes (it then sees the wrong process id and stops). Closing the
    // pipes ends the helper, which ends the elevated program.
    if (!m_connected) {
        const std::wstring name = m_pipeName.toStdWString();
        for (const wchar_t *direction : {L"-in", L"-out"}) {
            HANDLE poke = CreateFileW(pipePath(name, direction).c_str(),
                                      std::wstring(direction) == L"-in" ? GENERIC_READ : GENERIC_WRITE,
                                      0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (poke != INVALID_HANDLE_VALUE)
                CloseHandle(poke);
        }
    }
    if (m_toHelper) {
        DisconnectNamedPipe(m_toHelper);
        CloseHandle(m_toHelper);
    }
    m_toHelper = nullptr;
    if (m_fromHelper)
        DisconnectNamedPipe(m_fromHelper);
    if (m_reader.joinable())
        m_reader.join();
    if (m_fromHelper)
        CloseHandle(m_fromHelper);
    m_fromHelper = nullptr;
    if (m_helper)
        CloseHandle(m_helper);
    m_helper = nullptr;
    m_connected = false;
}

int runElevatedConsoleHelper()
{
    int count = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &count);
    // exe --elevated-console <pipe> <owner pid> <columns> <rows> <folder> <program> [args...]
    if (!argv || count < 8)
        return 2;
    const std::wstring name = argv[2];
    const DWORD owner = DWORD(wcstoul(argv[3], nullptr, 10));
    const int columns = int(wcstol(argv[4], nullptr, 10));
    const int rows = int(wcstol(argv[5], nullptr, 10));
    const std::wstring folder = argv[6];
    QString commandLine = quoted(QString::fromWCharArray(argv[7]));
    for (int i = 8; i < count; ++i)
        commandLine += QLatin1Char(' ') + quoted(QString::fromWCharArray(argv[i]));
    LocalFree(argv);

    // The pipes of the OpsRemote that started us, and only of that one.
    const auto connect = [owner](const std::wstring &path, DWORD access) -> HANDLE {
        for (int attempt = 0; attempt < 100; ++attempt) {
            HANDLE pipe = CreateFileW(path.c_str(), access, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                ULONG server = 0;
                if (GetNamedPipeServerProcessId(pipe, &server) && server == owner)
                    return pipe;
                CloseHandle(pipe);
                return INVALID_HANDLE_VALUE;
            }
            if (GetLastError() == ERROR_PIPE_BUSY)
                WaitNamedPipeW(path.c_str(), 1000);
            else
                Sleep(100);
        }
        return INVALID_HANDLE_VALUE;
    };
    HANDLE keys = connect(pipePath(name, L"-in"), GENERIC_READ);
    HANDLE screen = keys == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE
                                                 : connect(pipePath(name, L"-out"), GENERIC_WRITE);
    if (screen == INVALID_HANDLE_VALUE) {
        if (keys != INVALID_HANDLE_VALUE)
            CloseHandle(keys);
        return 3;
    }
    std::mutex screenLock;
    const auto send = [&](char type, const char *data, quint32 size) {
        std::lock_guard<std::mutex> guard(screenLock);
        return writeFrame(screen, type, data, size);
    };
    const auto report = [&](DWORD code) {
        const char bytes[4] = {char(code & 0xff), char((code >> 8) & 0xff), char((code >> 16) & 0xff),
                               char((code >> 24) & 0xff)};
        send('X', bytes, 4);
    };

    const auto &api = functions();
    HANDLE inputRead = nullptr, inputWrite = nullptr, outputRead = nullptr, outputWrite = nullptr;
    PseudoConsole console = nullptr;
    if (!isConsoleAvailable() || !CreatePipe(&inputRead, &inputWrite, nullptr, 0)
        || !CreatePipe(&outputRead, &outputWrite, nullptr, 0)
        || FAILED(api.create(size(columns, rows), inputRead, outputWrite, 0, &console))) {
        report(1);
        return 4;
    }
    CloseHandle(inputRead);
    CloseHandle(outputWrite);
    PROCESS_INFORMATION process{};
    if (!startInConsole(console, commandLine, QString::fromStdWString(folder), &process)
        && !startInConsole(console, commandLine, QString(), &process)) {
        const char message[] = "\r\nThe program could not be started as administrator.\r\n";
        send('O', message, sizeof message - 1);
        report(GetLastError());
        api.close(console);
        return 5;
    }
    CloseHandle(process.hThread);

    std::thread output([&] {
        char buffer[16384];
        DWORD read = 0;
        while (ReadFile(outputRead, buffer, sizeof buffer, &read, nullptr) && read > 0) {
            if (!send('O', buffer, read))
                break;
        }
    });
    std::atomic<bool> ended{false};
    std::thread waiter([&] {
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        report(code);
        // Unblocks the key reader below.
        ended = true;
        CancelIoEx(keys, nullptr);
    });

    // Keys and sizes from OpsRemote until it closes its end or the program ends.
    char type = 0;
    std::string payload;
    while (!ended && readFrame(keys, &type, &payload)) {
        if (type == 'D')
            writeAll(inputWrite, payload.data(), DWORD(payload.size()));
        else if (type == 'R' && payload.size() == 4)
            api.resize(console, size(uchar(payload[0]) | uchar(payload[1]) << 8,
                                     uchar(payload[2]) | uchar(payload[3]) << 8));
    }
    // OpsRemote went away (or the program ended): end the program too.
    TerminateProcess(process.hProcess, 0);
    waiter.join();
    api.close(console);
    CloseHandle(inputWrite);
    output.join();
    CloseHandle(outputRead);
    CloseHandle(process.hProcess);
    CloseHandle(keys);
    CloseHandle(screen);
    return 0;
}

