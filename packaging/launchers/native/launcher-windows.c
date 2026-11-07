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

// Every message says this: what is wrong never touches the user's data.
#define SAFE L"Your sessions and settings are unaffected by this."

// The package itself lacks what the program is unpacked from: only getting
// it again helps (the runtime copy is rebuilt by the launcher on its own).
// The package's folder must be writable: the program is unpacked next to it.
#define NOT_WRITABLE                                                                           \
    L"OpsRemote cannot write to its own folder:\n%ls\n\n"                                     \
    L"OpsRemote is portable and unpacks its program files next to itself, so its folder must "  \
    L"be writable. A protected folder such as Program Files, or a write-protected USB stick, "   \
    L"does not work.\n\n" SAFE L"\n\nMove the whole OpsRemote folder to a place you can write "  \
    L"to, for example Documents, and start it from there."

#define PACKAGE_DAMAGED                                                                        \
    L"Some of OpsRemote's own files are missing or damaged: %ls.\n\n"                        \
    L"OpsRemote unpacks its program from these files whenever it needs to, so without them "   \
    L"it cannot. This happens when a download was cut short or only part of the archive was "  \
    L"extracted.\n\n" SAFE L" They are in the \"settings\" folder, which is not touched.\n\n"  \
    L"To fix it, download OpsRemote again and extract the whole archive in the same place, "   \
    L"so that it replaces the files of this OpsRemote folder. The download contains no "       \
    L"settings, so yours stay as they are."

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
        fail(L"The path of the OpsRemote folder is too long for Windows:\n%ls\n\n" SAFE
             L"\n\nMove the whole OpsRemote folder to a shorter path, for example C:\\OpsRemote, "
             L"and start it from there.", here);
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

// The marker, unpacked.ok, is written last after unpacking and lists every
// file with its size ("size<TAB>relative path", UTF-8). A start checks the
// copy against it, so a copy that lost files (being removed, an antivirus)
// is found and rebuilt instead of failing.
static void list_files(HANDLE out, const wchar_t *root, const wchar_t *relative)
{
    wchar_t pattern[MAX_PATH];
    const int written = _snwprintf(pattern, MAX_PATH, relative[0] ? L"%ls\\%ls\\*" : L"%ls\\*",
                                   root, relative);
    if (written < 0 || written >= MAX_PATH)
        return;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0)
            continue;
        wchar_t name[MAX_PATH];
        const int length = _snwprintf(name, MAX_PATH, relative[0] ? L"%ls\\%ls" : L"%ls%ls",
                                      relative, found.cFileName);
        if (length < 0 || length >= MAX_PATH)
            continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            list_files(out, root, name);
            continue;
        }
        if (_wcsicmp(name, L"unpacked.ok") == 0 || _wcsicmp(name, L"remove-on-exit") == 0)
            continue;
        char utf8[3 * MAX_PATH], line[3 * MAX_PATH + 32];
        if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, NULL, NULL))
            continue;
        const unsigned long long size =
            ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        const int bytes = snprintf(line, sizeof line, "%llu\t%s\n", size, utf8);
        DWORD done = 0;
        if (bytes > 0 && bytes < (int)sizeof line)
            WriteFile(out, line, (DWORD)bytes, &done, NULL);
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

static void write_marker(const wchar_t *folder, const wchar_t *file)
{
    HANDLE handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    list_files(handle, folder, L"");
    CloseHandle(handle);
}

// TRUE when every file the marker lists is there with its size. A marker
// without a list (from earlier launchers) counts as not checked.
static BOOL copy_complete(const wchar_t *folder, const wchar_t *marker)
{
    HANDLE handle = CreateFileW(marker, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return FALSE;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 22)) {
        CloseHandle(handle);
        return FALSE;
    }
    char *text = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart + 1);
    DWORD read = 0;
    const BOOL ok = text && ReadFile(handle, text, (DWORD)size.QuadPart, &read, NULL);
    CloseHandle(handle);
    if (!ok) {
        if (text)
            HeapFree(GetProcessHeap(), 0, text);
        return FALSE;
    }
    text[read] = '\0';
    BOOL complete = TRUE;
    int files = 0;
    for (char *line = text, *next; complete && line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) {
            complete = FALSE;
            break;
        }
        *tab = '\0';
        const unsigned long long expected = _strtoui64(line, NULL, 10);
        wchar_t name[MAX_PATH], file[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA data;
        complete = MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1, name, MAX_PATH) > 0
                   && _snwprintf(file, MAX_PATH, L"%ls\\%ls", folder, name) > 0
                   && GetFileAttributesExW(file, GetFileExInfoStandard, &data)
                   && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                   && (((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow) == expected;
        ++files;
    }
    HeapFree(GetProcessHeap(), 0, text);
    return complete && files > 0;
}

// Less free space than unpacking needs (with some room to spare).
static BOOL disk_nearly_full(void)
{
    ULARGE_INTEGER available;
    return GetDiskFreeSpaceExW(here, &available, NULL, NULL)
           && available.QuadPart < 300ull * 1024 * 1024;
}

// zstd -dcq <archive> | tar -xf - -C <folder>, without console windows.

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
    // No busy pointer for these hidden helpers (Windows would show one for
    // each until a timeout, as they never open a window).
    start.dwFlags = STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK;
    // Below normal priority: unpacking takes all CPUs and the disk for a
    // while, which made the whole computer (the mouse too) lag meanwhile.
    const DWORD flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;
    PROCESS_INFORMATION decompress = {0}, extract = {0};

    start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    start.hStdOutput = write_end;
    start.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    swprintf(command, sizeof command / sizeof *command, L"\"%ls\" -dcq \"%ls\"", zstd, archive);
    BOOL ok = CreateProcessW(zstd, command, NULL, NULL, TRUE, flags, NULL, NULL, &start, &decompress);
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
    if (ok) {
        start.hStdInput = read_end;
        start.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        swprintf(command, sizeof command / sizeof *command, L"\"%ls\" -x%lsf - -C \"%ls\"", tar,
                 keep ? L"k" : L"", folder);
        ok = CreateProcessW(tar, command, NULL, NULL, TRUE, flags, NULL, NULL, &start, &extract);
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
        fail(L"Windows did not say where OpsRemote.exe is, or the path of its folder is too long."
             L"\n\n" SAFE L"\n\nMove the whole OpsRemote folder to a shorter path, for example "
             L"C:\\OpsRemote, and start it from there.");
    }
    wchar_t *slash = wcsrchr(here, L'\\');
    if (!slash)
        fail(L"Windows did not say where OpsRemote.exe is.\n\n" SAFE
             L"\n\nStart OpsRemote again from its folder.");
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
             L"%lu build %lu.\n\n" SAFE, version.dwMajorVersion, version.dwBuildNumber);

    wchar_t data[MAX_PATH], archive[MAX_PATH], app[MAX_PATH], runtime[MAX_PATH];
    path(data, L"data");
    if (!exists(data))
        fail(PACKAGE_DAMAGED, L"the \"data\" folder");
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
            fail(PACKAGE_DAMAGED, L"compressed\\" PLATFORM L".id");
        wchar_t zstd[MAX_PATH];
        path(zstd, L"tools\\zstd-" PLATFORM L".exe");
        format_path(runtime, L"%lsruntime\\" PLATFORM L"-%hs", here, id);
        format_path(app, L"%ls\\OpsRemote.exe", runtime);
        wchar_t marker[MAX_PATH];
        format_path(marker, L"%ls\\unpacked.ok", runtime);
        // A copy that is not complete (unpacking or removing it was cut
        // short, an antivirus took files) is rebuilt: it holds only program
        // files, the user's data is in settings\. Renamed away first, which
        // Windows refuses while an OpsRemote still runs from it; then the
        // missing files are filled in instead.
        if (exists(runtime) && !(exists(app) && copy_complete(runtime, marker))) {
            wchar_t broken[MAX_PATH];
            format_path(broken, L"%ls.removing%lu", runtime, GetCurrentProcessId());
            if (MoveFileW(runtime, broken)) {
                remove_tree(broken);
            } else if (exists(zstd) && unpack(zstd, archive, runtime, TRUE)) {
                write_marker(runtime, marker);
            }
        }
        if (!exists(app)) {
            wchar_t runtime_root[MAX_PATH], tar[MAX_PATH];
            if (!exists(zstd))
                fail(PACKAGE_DAMAGED, L"tools\\zstd-" PLATFORM L".exe");
            const UINT system = GetSystemDirectoryW(tar, MAX_PATH - 16);
            if (system == 0 || system >= MAX_PATH - 16)
                fail(L"Windows did not say where its system folder is, and OpsRemote needs its tar "
                     L"program from there to unpack itself.\n\n" SAFE L"\n\nRestart Windows and "
                     L"try again.");
            wcscat(tar, L"\\tar.exe");
            if (!exists(tar))
                fail(L"Windows' own tar program (%ls) is missing. OpsRemote uses it to unpack its "
                     L"program files; it comes with Windows 10 version 1809 and newer.\n\n" SAFE,
                     tar);
            path(runtime_root, L"runtime");
            if (!CreateDirectoryW(runtime_root, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                fail(NOT_WRITABLE, here);
            wchar_t partial[MAX_PATH];
            format_path(partial, L"%ls.partial%lu", runtime, GetCurrentProcessId());
            remove_tree(partial);
            if (!CreateDirectoryW(partial, NULL))
                fail(NOT_WRITABLE, here);
            // Silently: a window appears only if something goes wrong.
            const BOOL unpacked = unpack(zstd, archive, partial, FALSE);
            wchar_t check[MAX_PATH];
            format_path(check, L"%ls\\OpsRemote.exe", partial);
            if (!unpacked || !exists(check)) {
                remove_tree(partial);
                if (disk_nearly_full())
                    fail(L"OpsRemote could not unpack its program files (about 100 MB) into the "
                         L"\"runtime\" folder next to it: the disk is nearly full.\n\n" SAFE
                         L"\n\nFree some space on this drive and start OpsRemote again.");
                fail(PACKAGE_DAMAGED, L"compressed\\" PLATFORM L".tar.zst cannot be unpacked");
            }
            wchar_t partial_marker[MAX_PATH];
            format_path(partial_marker, L"%ls\\unpacked.ok", partial);
            write_marker(partial, partial_marker);
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
    // Still incomplete after rebuilding: something removes the files as they
    // are unpacked (an antivirus), or the uncompressed package lacks them.
    if (!exists(app) || !exists(qt))
        fail(exists(archive)
                 ? L"OpsRemote's program files were unpacked again, but some are still missing "
                   L"(OpsRemote.exe or Qt6Core.dll in %ls). An antivirus program may be removing "
                   L"them.\n\n" SAFE L"\n\nCheck your antivirus program's quarantine, allow "
                   L"OpsRemote, and start it again."
                 : PACKAGE_DAMAGED,
             exists(archive) ? folder : L"the \"" PLATFORM L"\" program folder");

    // Written by the program as it ends when it should be removed (Preferences:
    // remove the unpacked program on exit); one left by an earlier start that
    // did not get to remove it is no longer meant.
    wchar_t removal[MAX_PATH];
    format_path(removal, L"%ls\\remove-on-exit", folder);
    DeleteFileW(removal);

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
        fail(L"Windows could not start OpsRemote's program (%ls, error %lu). An antivirus "
             L"program may be blocking it.\n\n" SAFE L"\n\nCheck your antivirus program's "
             L"quarantine, allow OpsRemote, and start it again.", app, GetLastError());
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
    // Renamed first: Windows refuses that while another OpsRemote runs from
    // the folder, which then keeps its files; a start after the rename finds
    // no folder and unpacks a new one. The runtime folder goes too once empty.
    if (exists(removal)) {
        wchar_t removing[MAX_PATH];
        format_path(removing, L"%ls.removing%lu", folder, GetCurrentProcessId());
        if (MoveFileW(folder, removing))
            remove_tree(removing);
        wchar_t runtime_root[MAX_PATH];
        path(runtime_root, L"runtime");
        RemoveDirectoryW(runtime_root);
    }
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
