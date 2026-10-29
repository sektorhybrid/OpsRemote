// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
//
// OpsRemote: the Linux launcher of the combined package, a native program
// doing what legacy/OpsRemote.sh does (which stays as a fallback). It can be
// double-clicked in a file manager, where a .sh often opens in an editor.
//
// The program is stored compressed (compressed/linux-x86_64.tar.zst). The first start
// of a version extracts it into runtime/ next to this launcher with
// tools/zstd-linux-x86_64 (or the system's zstd) and the system's tar; later
// starts reuse that copy, and copies of older versions are removed. It then
// runs the bundle's own opsremote.sh, which checks the system's libraries,
// and stays to watch it: if OpsRemote crashes, it says so, with the last
// lines the program wrote.
//
// On a text console without a graphical session it only says that it needs
// one. It never crashes itself: every path is length-checked, every missing file
// is explained, and every failure ends in a message, not undefined behaviour.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PLATFORM "linux-x86_64"
// The program's last output kept for a crash report.
#define TAIL_SIZE 1500

static char here[PATH_MAX];
static volatile pid_t program = 0;

static int has_display(void)
{
    return getenv("DISPLAY") || getenv("WAYLAND_DISPLAY");
}

// A dialog with the usual desktop tools, when started without a terminal.
static void show_dialog(const char *text, int error)
{
    if (isatty(STDERR_FILENO) || !has_display())
        return;
    const char *dialogs[][7] = {
        {"zenity", error ? "--error" : "--warning", "--title=OpsRemote", "--no-wrap", "--text", text, NULL},
        {"kdialog", "--title", "OpsRemote", error ? "--error" : "--sorry", text, NULL, NULL},
        {"xmessage", "-center", text, NULL, NULL, NULL, NULL},
    };
    for (size_t i = 0; i < sizeof dialogs / sizeof dialogs[0]; ++i) {
        pid_t child = fork();
        if (child < 0)
            return;
        if (child == 0) {
            int null = open("/dev/null", O_WRONLY);
            if (null >= 0) {
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
            }
            execvp(dialogs[i][0], (char *const *)dialogs[i]);
            _exit(127);
        }
        int status = 0;
        if (waitpid(child, &status, 0) > 0 && WIFEXITED(status) && WEXITSTATUS(status) != 127)
            return; // that tool was there and showed it
    }
}

// Explains the problem (stderr and, without a terminal, a dialog) and ends.
static void fail(const char *format, ...)
{
    char message[2048];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    fprintf(stderr, "OpsRemote: %s\n", message);
    char text[2300];
    snprintf(text, sizeof text, "OpsRemote cannot start.\n\n%s", message);
    show_dialog(text, 1);
    exit(1);
}

// out = a/b, or a clear message when the result would not fit.
static void join(char *out, size_t size, const char *a, const char *b)
{
    int written = snprintf(out, size, "%s/%s", a, b);
    if (written < 0 || (size_t)written >= size)
        fail("the folder path is too long:\n%s", a);
}

static void copy(char *out, size_t size, const char *text)
{
    int written = snprintf(out, size, "%s", text);
    if (written < 0 || (size_t)written >= size)
        fail("the folder path is too long:\n%s", text);
}

static int is_executable(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode) && access(path, X_OK) == 0;
}

static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info;
    (void)type;
    (void)walk;
    remove(path);
    return 0; // keep going whatever fails
}

static void remove_tree(const char *path)
{
    nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

// The first line of a small file; empty if missing or unreadable.
static void read_line(const char *path, char *out, size_t size)
{
    out[0] = '\0';
    FILE *file = fopen(path, "r");
    if (!file)
        return;
    if (!fgets(out, (int)size, file))
        out[0] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    fclose(file);
}

// A version id is a short hex hash: nothing that could leave runtime/.
static int is_safe_id(const char *id)
{
    if (!id[0] || strlen(id) > 64)
        return 0;
    for (const char *c = id; *c; ++c) {
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')))
            return 0;
    }
    return 1;
}

static int waited_ok(pid_t child)
{
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// zstd -dcq <archive> | tar -xf - -C <folder>; true on success.
static int unpack(const char *zstd, const char *archive, const char *folder)
{
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
        return 0;
    pid_t decompress = fork();
    if (decompress == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execl(zstd, zstd, "-dcq", archive, (char *)NULL);
        _exit(127);
    }
    pid_t extract = decompress > 0 ? fork() : -1;
    if (extract == 0) {
        dup2(pipe_fds[0], STDIN_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execlp("tar", "tar", "-xf", "-", "-C", folder, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    int ok = decompress > 0 && extract > 0;
    if (decompress > 0 && !waited_ok(decompress))
        ok = 0;
    if (extract > 0 && !waited_ok(extract))
        ok = 0;
    return ok;
}

// Removes runtime/<platform>-* except keep (empty: all of them).
// Running programs keep their open files on Linux, so this is safe.
static void remove_other_versions(const char *runtime_root, const char *keep)
{
    DIR *dir = opendir(runtime_root);
    if (!dir)
        return;
    const size_t prefix = strlen(PLATFORM "-");
    const struct dirent *entry;
    while ((entry = readdir(dir))) {
        // Not another start's unpacking still under way (.partial.<pid>).
        if (strncmp(entry->d_name, PLATFORM "-", prefix) != 0 || strcmp(entry->d_name, keep) == 0
            || strstr(entry->d_name, ".partial."))
            continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof path, "%s/%s", runtime_root, entry->d_name) < (int)sizeof path)
            remove_tree(path);
    }
    closedir(dir);
}

// Ctrl+C or a termination request reaches OpsRemote, not only this launcher.
static void pass_on(int signal_number)
{
    if (program > 0)
        kill(program, signal_number);
}

// Runs OpsRemote (through the bundle's opsremote.sh) and waits for it. Its
// error output still reaches the terminal, and its last part is kept for a
// crash report. Returns its exit code.
static int run_and_watch(char *script, char **arguments)
{
    int output[2];
    const int capture = pipe(output) == 0;
    pid_t child = fork();
    if (child < 0)
        fail("cannot start %s: %s", script, strerror(errno));
    if (child == 0) {
        if (capture) {
            dup2(output[1], STDERR_FILENO);
            close(output[0]);
            close(output[1]);
        }
        // If this launcher went away, writing to the pipe must not kill OpsRemote.
        signal(SIGPIPE, SIG_IGN);
        execv(script, arguments);
        fprintf(stderr, "cannot run %s: %s\n", script, strerror(errno));
        _exit(126);
    }
    program = child;
    struct sigaction forward;
    memset(&forward, 0, sizeof forward);
    forward.sa_handler = pass_on;
    sigaction(SIGINT, &forward, NULL);
    sigaction(SIGTERM, &forward, NULL);
    sigaction(SIGHUP, &forward, NULL);

    char tail[TAIL_SIZE + 1] = {0};
    size_t kept = 0;
    if (capture) {
        close(output[1]);
        char buffer[4096];
        for (;;) {
            const ssize_t count = read(output[0], buffer, sizeof buffer);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            (void)!write(STDERR_FILENO, buffer, (size_t)count);
            // Keep the last TAIL_SIZE bytes.
            const size_t n = (size_t)count;
            if (n >= TAIL_SIZE) {
                memcpy(tail, buffer + n - TAIL_SIZE, TAIL_SIZE);
                kept = TAIL_SIZE;
            } else {
                if (kept + n > TAIL_SIZE) {
                    const size_t drop = kept + n - TAIL_SIZE;
                    memmove(tail, tail + drop, kept - drop);
                    kept -= drop;
                }
                memcpy(tail + kept, buffer, n);
                kept += n;
            }
        }
        close(output[0]);
    }
    tail[kept] = '\0';

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return 1;
    }
    program = 0;
    // Exit code 0 is a normal end; 1 is opsremote.sh's own explained error.
    if (WIFEXITED(status) && WEXITSTATUS(status) <= 1)
        return WEXITSTATUS(status);
    char reason[128];
    if (WIFSIGNALED(status)) {
        const int number = WTERMSIG(status);
        if (number == SIGINT || number == SIGTERM || number == SIGHUP || number == SIGKILL)
            return 128 + number; // ended on purpose, not a crash
        snprintf(reason, sizeof reason, "OpsRemote crashed (%s).", strsignal(number));
    } else {
        snprintf(reason, sizeof reason, "OpsRemote ended with an error (exit code %d).",
                 WEXITSTATUS(status));
    }
    fprintf(stderr, "OpsRemote: %s\n", reason);
    char text[TAIL_SIZE + 512];
    snprintf(text, sizeof text,
             "%s\n\nYour settings are kept. If it happens again, run OpsRemote from a terminal, "
             "or set OPSREMOTE_LOG=<file>, and report what it shows.%s%s",
             reason, kept ? "\n\nIts last messages:\n" : "", tail);
    show_dialog(text, 1);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
}

int main(int argc, char **argv)
{
    // A desktop program: on a text console (no X11 or Wayland session, e.g.
    // runlevel 3) say so instead of failing later. QT_QPA_PLATFORM set by
    // hand (such as "offscreen" for tests) is respected.
    if (!has_display() && !getenv("QT_QPA_PLATFORM")) {
        fprintf(stderr, "OpsRemote needs a graphical desktop (X11 or Wayland). Start it from a "
                        "desktop session.\n");
        return 1;
    }

    // Where this launcher is (symbolic links resolved), as legacy/OpsRemote.sh does.
    char self[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", self, sizeof self - 1);
    if (length <= 0)
        fail("cannot find where this launcher is (/proc is not available).");
    self[length] = '\0';
    char *slash = strrchr(self, '/');
    if (!slash)
        fail("cannot find where this launcher is.");
    *slash = '\0';
    copy(here, sizeof here, self[0] ? self : "/");

    char data[PATH_MAX], archive[PATH_MAX], app[PATH_MAX], script[PATH_MAX];
    join(data, sizeof data, here, "data");
    struct stat info;
    if (stat(data, &info) != 0 || !S_ISDIR(info.st_mode))
        fail("this copy of OpsRemote is incomplete (the data folder is missing). Extract the whole "
             "archive again.");
    join(archive, sizeof archive, here, "compressed/" PLATFORM ".tar.zst");
    if (access(archive, R_OK) == 0) {
        char id_file[PATH_MAX], id[128];
        join(id_file, sizeof id_file, here, "compressed/" PLATFORM ".id");
        read_line(id_file, id, sizeof id);
        if (!is_safe_id(id))
            fail("compressed/" PLATFORM ".id is missing or damaged. Extract the whole archive again.");

        char runtime_root[PATH_MAX], name[96], runtime[PATH_MAX];
        join(runtime_root, sizeof runtime_root, here, "runtime");
        snprintf(name, sizeof name, PLATFORM "-%s", id);
        join(runtime, sizeof runtime, runtime_root, name);
        join(script, sizeof script, runtime, "opsremote.sh");
        if (!is_executable(script)) {
            if (mkdir(runtime_root, 0755) != 0 && errno != EEXIST)
                fail("cannot write to %s. OpsRemote unpacks itself next to this launcher, so the "
                     "folder must be writable.", here);
            char zstd[PATH_MAX];
            join(zstd, sizeof zstd, here, "tools/zstd-" PLATFORM);
            if (!is_executable(zstd)) {
                // A copy without execute permission (some file systems): the system's.
                if (is_executable("/usr/bin/zstd"))
                    copy(zstd, sizeof zstd, "/usr/bin/zstd");
                else if (is_executable("/bin/zstd"))
                    copy(zstd, sizeof zstd, "/bin/zstd");
                else
                    fail("tools/zstd-" PLATFORM " is missing or not executable, and the system has "
                         "no zstd. Extract the whole archive again.");
            }
            fprintf(stderr, "OpsRemote: preparing this version (first start only)...\n");
            char partial[PATH_MAX];
            const int written = snprintf(partial, sizeof partial, "%s.partial.%d", runtime, (int)getpid());
            if (written < 0 || (size_t)written >= sizeof partial)
                fail("the folder path is too long:\n%s", here);
            remove_tree(partial);
            if (mkdir(partial, 0755) != 0)
                fail("cannot create %s: %s.", partial, strerror(errno));
            if (!unpack(zstd, archive, partial)) {
                remove_tree(partial);
                fail("could not unpack compressed/" PLATFORM ".tar.zst. Is the disk full, or is tar "
                     "missing?");
            }
            // Another start may have finished first; either copy is complete.
            if (rename(partial, runtime) != 0)
                remove_tree(partial);
            if (!is_executable(script))
                fail("unpacking compressed/" PLATFORM ".tar.zst gave no program. Extract the whole "
                     "archive again.");
        }
        remove_other_versions(runtime_root, name);
        // Earlier packages unpacked into a hidden .runtime folder.
        char old_root[PATH_MAX];
        join(old_root, sizeof old_root, here, ".runtime");
        remove_other_versions(old_root, "");
        rmdir(old_root);
        copy(app, sizeof app, runtime);
    } else {
        // An uncompressed package (make-package.sh --uncompressed).
        join(app, sizeof app, here, PLATFORM);
        join(script, sizeof script, app, "opsremote.sh");
        if (!is_executable(script))
            fail("the Linux program (compressed/" PLATFORM ".tar.zst) is missing. Extract the "
                 "whole archive again.");
    }

    if (setenv("OPSREMOTE_HOME", here, 1) != 0)
        fail("cannot set OPSREMOTE_HOME.");
    char **arguments = calloc((size_t)argc + 1, sizeof *arguments);
    if (!arguments)
        fail("out of memory.");
    arguments[0] = script;
    for (int i = 1; i < argc; ++i)
        arguments[i] = argv[i];
    return run_and_watch(script, arguments);
}
