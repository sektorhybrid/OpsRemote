// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
//
// OpsRemote.exe: the Windows launcher of the combined package, a native
// program doing what legacy\OpsRemote.bat does (which stays as a fallback). It runs
// without a console or window of its own, carries the OpsRemote icon, and
// explains problems in message boxes.
//
// The program is stored compressed (compressed\windows-x86_64.tar.zst). The first start
// of a version extracts it into runtime\ next to this launcher with
// tools\zstd-windows-x86_64.exe and Windows' own tar.exe; later starts reuse
// that copy, and copies of older versions are removed once they are no
// longer running.

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#define PLATFORM L"windows-x86_64"

static wchar_t here[MAX_PATH]; // with a trailing backslash

static void fail(const wchar_t *format, ...)
{
    wchar_t message[2048];
    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf(message, sizeof message / sizeof *message - 1, format, arguments);
    message[sizeof message / sizeof *message - 1] = L'\0';
    va_end(arguments);
    wchar_t text[2300];
    swprintf(text, sizeof text / sizeof *text, L"OpsRemote cannot start.\n\n%ls", message);
    MessageBoxW(NULL, text, L"OpsRemote", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

// Formats a path into a MAX_PATH buffer, or explains instead of truncating.
static void format_path(wchar_t *out, const wchar_t *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int written = _vsnwprintf(out, MAX_PATH, format, arguments);
    va_end(arguments);
    if (written < 0 || written >= MAX_PATH) {
        out[MAX_PATH - 1] = L'\0';
        fail(L"The folder path is too long:\n%ls\n\nMove the OpsRemote folder to a shorter path.", here);
    }
}

static void path(wchar_t *out, const wchar_t *relative)
{
    format_path(out, L"%ls%ls", here, relative);
}

static BOOL exists(const wchar_t *file)
{
    return GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES;
}

// Deletes a folder with everything in it; a running program's files stay.
static void remove_tree(const wchar_t *folder)
{
    wchar_t from[MAX_PATH + 2] = {0}; // double-NUL-terminated for SHFileOperation
    wcsncpy(from, folder, MAX_PATH);
    SHFILEOPSTRUCTW operation = {0};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from;
    operation.fFlags = FOF_NO_UI;
    SHFileOperationW(&operation);
}

static void read_line(const wchar_t *file, char *out, size_t size)
{
    out[0] = '\0';
    HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    DWORD read = 0;
    if (ReadFile(handle, out, (DWORD)size - 1, &read, NULL))
        out[read] = '\0';
    CloseHandle(handle);
    out[strcspn(out, "\r\n")] = '\0';
}

// zstd -dcq <archive> | tar -xf - -C <folder>, without console windows.
static void write_marker(const wchar_t *file)
{
    HANDLE handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
}

// keep: leave files that are already there (filling in a copy that lost
// some of its files, while others are in use).
static BOOL unpack(const wchar_t *zstd, const wchar_t *archive, const wchar_t *folder, BOOL keep)
{
    SECURITY_ATTRIBUTES inherit;
    ZeroMemory(&inherit, sizeof inherit);
    inherit.nLength = sizeof inherit;
    inherit.bInheritHandle = TRUE;
    HANDLE read_end, write_end;
    if (!CreatePipe(&read_end, &write_end, &inherit, 1 << 20))
        return FALSE;
    wchar_t tar[MAX_PATH];
    const UINT system = GetSystemDirectoryW(tar, MAX_PATH - 16);
    if (system == 0 || system >= MAX_PATH - 16) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return FALSE;
    }
    wcscat(tar, L"\\tar.exe");

    wchar_t command[3 * MAX_PATH + 64];
    STARTUPINFOW start;
    ZeroMemory(&start, sizeof start);
    start.cb = sizeof start;
    start.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION decompress = {0}, extract = {0};

    start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    start.hStdOutput = write_end;
    start.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    swprintf(command, sizeof command / sizeof *command, L"\"%ls\" -dcq \"%ls\"", zstd, archive);
    BOOL ok = CreateProcessW(zstd, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &start,
                             &decompress);
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
    if (ok) {
        start.hStdInput = read_end;
        start.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        swprintf(command, sizeof command / sizeof *command, L"\"%ls\" -x%lsf - -C \"%ls\"", tar,
                 keep ? L"k" : L"", folder);
        ok = CreateProcessW(tar, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &start,
                            &extract);
    }
    CloseHandle(read_end);
    CloseHandle(write_end);
    DWORD code = 1;
    for (PROCESS_INFORMATION *process = &decompress; process; process = process == &decompress ? &extract : NULL) {
        if (!process->hProcess)
            continue;
        WaitForSingleObject(process->hProcess, INFINITE);
        if (!GetExitCodeProcess(process->hProcess, &code) || code != 0)
            ok = FALSE;
        CloseHandle(process->hProcess);
        CloseHandle(process->hThread);
    }
    return ok && decompress.hProcess && extract.hProcess;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show)
{
    (void)instance;
    (void)previous;
    (void)show;
    const DWORD length = GetModuleFileNameW(NULL, here, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        here[0] = L'\0';
        fail(L"Cannot find where this launcher is, or its folder path is too long.");
    }
    wchar_t *slash = wcsrchr(here, L'\\');
    if (!slash)
        fail(L"Cannot find where this launcher is.");
    slash[1] = L'\0';

    // Windows 10 version 1809 (build 17763) or newer: tar.exe and the
    // program's own requirements.
    RTL_OSVERSIONINFOW version;
    ZeroMemory(&version, sizeof version);
    version.dwOSVersionInfoSize = sizeof version;
    typedef LONG(WINAPI * GetVersion)(RTL_OSVERSIONINFOW *);
    const GetVersion getVersion = (GetVersion)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                     "RtlGetVersion");
    if (getVersion && getVersion(&version) == 0
        && (version.dwMajorVersion < 10 || version.dwBuildNumber < 17763))
        fail(L"OpsRemote needs Windows 10 version 1809 or newer, or Windows 11. This is Windows "
             L"%lu build %lu.", version.dwMajorVersion, version.dwBuildNumber);

    wchar_t data[MAX_PATH], archive[MAX_PATH], app[MAX_PATH], runtime[MAX_PATH];
    path(data, L"data");
    if (!exists(data))
        fail(L"This copy of OpsRemote is incomplete. Extract the whole archive again.");
    path(archive, L"compressed\\" PLATFORM L".tar.zst");
    if (exists(archive)) {
        wchar_t id_file[MAX_PATH];
        char id[128];
        path(id_file, L"compressed\\" PLATFORM L".id");
        read_line(id_file, id, sizeof id);
        BOOL safe = id[0] != '\0' && strlen(id) <= 64;
        for (const char *c = id; safe && *c; ++c)
            safe = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z');
        if (!safe)
            fail(L"This copy of OpsRemote is incomplete. Extract the whole archive again.");
        format_path(runtime, L"%lsruntime\\" PLATFORM L"-%hs", here, id);
        format_path(app, L"%ls\\OpsRemote.exe", runtime);
        // Written last after unpacking. Missing while the program is there:
        // an older launcher's copy, or one that lost files (it was being
        // removed when this start reused it); the missing files are put back.
        wchar_t marker[MAX_PATH];
        format_path(marker, L"%ls\\unpacked.ok", runtime);
        if (exists(app) && !exists(marker)) {
            wchar_t zstd[MAX_PATH];
            path(zstd, L"tools\\zstd-" PLATFORM L".exe");
            if (exists(zstd) && unpack(zstd, archive, runtime, TRUE))
                write_marker(marker);
        }
        if (!exists(app)) {
            wchar_t zstd[MAX_PATH], runtime_root[MAX_PATH], tar[MAX_PATH];
            path(zstd, L"tools\\zstd-" PLATFORM L".exe");
            if (!exists(zstd))
                fail(L"This copy of OpsRemote is incomplete. Extract the whole archive again.");
            const UINT system = GetSystemDirectoryW(tar, MAX_PATH - 16);
            if (system == 0 || system >= MAX_PATH - 16)
                fail(L"Cannot find the Windows system folder.");
            wcscat(tar, L"\\tar.exe");
            if (!exists(tar))
                fail(L"The Windows tar program is missing; OpsRemote needs Windows 10 version 1809 "
                     L"or newer.");
            path(runtime_root, L"runtime");
            if (!CreateDirectoryW(runtime_root, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                fail(L"Cannot write to %ls. OpsRemote unpacks itself next to this file, so the "
                     L"folder must be writable.", here);
            wchar_t partial[MAX_PATH];
            format_path(partial, L"%ls.partial%lu", runtime, GetCurrentProcessId());
            remove_tree(partial);
            if (!CreateDirectoryW(partial, NULL))
                fail(L"Cannot write to %ls. OpsRemote unpacks itself next to this file, so the "
                     L"folder must be writable.", here);
            // Silently: a window appears only if something goes wrong.
            const BOOL unpacked = unpack(zstd, archive, partial, FALSE);
            wchar_t check[MAX_PATH];
            format_path(check, L"%ls\\OpsRemote.exe", partial);
            if (!unpacked || !exists(check)) {
                remove_tree(partial);
                fail(L"Could not unpack compressed\\" PLATFORM L".tar.zst. Is the disk full?");
            }
            wchar_t partial_marker[MAX_PATH];
            format_path(partial_marker, L"%ls\\unpacked.ok", partial);
            write_marker(partial_marker);
            // Another start may have finished first; either copy is complete.
            if (!MoveFileW(partial, runtime))
                remove_tree(partial);
        }
        // Copies of other versions; one that is still running stays until next time.
        wchar_t pattern[MAX_PATH];
        path(pattern, L"runtime\\" PLATFORM L"-*");
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW(pattern, &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                // Only a complete path: a cut-off one could name another folder.
                wchar_t other[MAX_PATH];
                const int written = _snwprintf(other, MAX_PATH, L"%lsruntime\\%ls", here,
                                               found.cFileName);
                // Not another start's unpacking still under way (.partial<pid>).
                if (written > 0 && written < MAX_PATH
                    && (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    && _wcsicmp(other, runtime) != 0 && !wcsstr(found.cFileName, L".partial"))
                    remove_tree(other);
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
        // Earlier packages unpacked into a hidden .runtime folder.
        wchar_t old_root[MAX_PATH];
        path(old_root, L".runtime");
        if (exists(old_root))
            remove_tree(old_root);
    } else {
        // An uncompressed package (make-package.sh --uncompressed).
        path(app, PLATFORM L"\\OpsRemote.exe");
    }

    wchar_t qt[MAX_PATH], folder[MAX_PATH];
    format_path(folder, L"%ls", app);
    wchar_t *last = wcsrchr(folder, L'\\');
    if (last)
        *last = L'\0';
    format_path(qt, L"%ls\\Qt6Core.dll", folder);
    if (!exists(app) || !exists(qt))
        fail(L"This copy of OpsRemote is incomplete. Extract the whole archive again.");

    // The program finds its settings next to this launcher.
    wchar_t home[MAX_PATH];
    wcsncpy(home, here, MAX_PATH);
    home[wcslen(home) - 1] = L'\0'; // without the trailing backslash
    SetEnvironmentVariableW(L"OPSREMOTE_HOME", home);

    wchar_t command[32768];
    swprintf(command, sizeof command / sizeof *command, L"\"%ls\" %ls", app, arguments ? arguments : L"");
    STARTUPINFOW start;
    ZeroMemory(&start, sizeof start);
    start.cb = sizeof start;
    PROCESS_INFORMATION process;
    if (!CreateProcessW(app, command, NULL, NULL, FALSE, 0, NULL, folder, &start, &process))
        fail(L"Cannot start %ls (error %lu).", app, GetLastError());
    CloseHandle(process.hThread);

    // Stay (without a window) and watch it: a crash gets explained instead of
    // the program just vanishing. Waiting with a message queue tells Windows
    // this launcher has finished starting; without one, Windows keeps showing
    // its "starting" pointer for it until a timeout.
    for (;;) {
        DWORD woke = MsgWaitForMultipleObjects(1, &process.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (woke != WAIT_OBJECT_0 + 1)
            break;
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process.hProcess, &code))
        code = 0;
    CloseHandle(process.hProcess);
    if (code == 0)
        return 0;
    wchar_t reason[256];
    if (code >= 0xC0000000u)
        swprintf(reason, sizeof reason / sizeof *reason,
                 L"OpsRemote crashed (exception 0x%08lX%ls).", code,
                 code == 0xC0000005u ? L", access violation"
                 : code == 0xC0000135u ? L", a DLL is missing"
                 : code == 0xC0000409u ? L", stack buffer overrun"
                                       : L"");
    else
        swprintf(reason, sizeof reason / sizeof *reason,
                 L"OpsRemote ended with an error (exit code %lu).", code);
    wchar_t text[1024];
    swprintf(text, sizeof text / sizeof *text,
             L"%ls\n\nYour settings are kept. If it happens again, set the environment "
             L"variable OPSREMOTE_LOG to a file name, start OpsRemote again, and report what "
             L"that file shows.", reason);
    MessageBoxW(NULL, text, L"OpsRemote", MB_OK | MB_ICONWARNING);
    return (int)code;
}
