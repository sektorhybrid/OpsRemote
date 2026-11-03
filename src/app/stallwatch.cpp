// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos

#include "stallwatch.h"
#include "workerthreads.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#else
#include <csignal>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#endif

namespace {

constexpr int TickMs = 100;
int stallMs = 700; // shorter pauses are hardly noticeable; OPSREMOTE_LOG_STALL_MS changes it
constexpr int SampleMs = 250;  // while stalled
constexpr int MaximumFrames = 48;

struct Stack
{
    int count = 0;
    std::uintptr_t frames[MaximumFrames] = {};
};

QElapsedTimer sinceStart;
std::atomic<qint64> lastTick{0};

#ifdef Q_OS_WIN
HANDLE guiThread = nullptr;

// The thread is suspended meanwhile: nothing here may allocate or take a
// lock that thread could hold (the heap's, for one), or both would hang.
Stack sample(HANDLE thread)
{
    Stack stack;
    if (SuspendThread(thread) == DWORD(-1))
        return stack;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context)) {
        while (stack.count < MaximumFrames && context.Rip) {
            stack.frames[stack.count++] = context.Rip;
            DWORD64 base = 0;
            auto *function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
            if (!function) {
                // A leaf function: the return address is on top of the stack.
                context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                context.Rsp += 8;
            } else {
                void *handler = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handler,
                                 &frame, nullptr);
            }
        }
    }
    ResumeThread(thread);
    return stack;
}

Stack sample()
{
    return sample(guiThread);
}

// The other threads of this program (not the GUI thread, not this one):
// what they do while the GUI stalls, e.g. one that is ending and waits.
QList<QPair<DWORD, Stack>> sampleOthers()
{
    QList<DWORD> ids;
    const DWORD self = GetCurrentThreadId();
    const DWORD gui = GetThreadId(guiThread);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof entry;
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != self
                && entry.th32ThreadID != gui)
                ids << entry.th32ThreadID;
        }
        CloseHandle(snapshot);
    }
    QList<QPair<DWORD, Stack>> stacks;
    stacks.reserve(ids.size()); // no allocation while a thread is suspended
    for (const DWORD id : std::as_const(ids)) {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, id);
        if (!thread)
            continue;
        stacks.append({id, Stack{}});
        stacks.last().second = sample(thread);
        CloseHandle(thread);
    }
    return stacks;
}

// module+offset; the offsets of opsremote.exe resolve with nm/addr2line on
// the build's executable, and exported names are added where there is one.
QString describe(std::uintptr_t address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
        return QStringLiteral("0x%1").arg(address, 0, 16);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    QString name = QString::fromWCharArray(path).section(QLatin1Char('\\'), -1);
    return QStringLiteral("%1+0x%2").arg(name).arg(address - std::uintptr_t(module), 0, 16);
}
#else
pthread_t guiThread;
// Filled by the GUI thread in its signal handler. Static, so a handler that
// runs after sample() gave up writes nowhere harmful.
Stack pending;
std::atomic_bool sampled{false};

void handleSample(int)
{
    // backtrace() is not formally async-signal-safe, but it is after its
    // first call (made at start-up below), which loads libgcc.
    void *frames[MaximumFrames];
    const int count = backtrace(frames, MaximumFrames);
    // Frame 0 and 1 are this handler and the signal trampoline.
    pending.count = 0;
    for (int index = 2; index < count; ++index)
        pending.frames[pending.count++] = std::uintptr_t(frames[index]);
    sampled = true;
}

Stack sample()
{
    sampled = false;
    pthread_kill(guiThread, SIGURG);
    for (int waited = 0; !sampled && waited < 200; ++waited)
        QThread::msleep(1);
    return sampled ? pending : Stack{};
}

QString describe(std::uintptr_t address)
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(address), &info) || !info.dli_fname)
        return QStringLiteral("0x%1").arg(address, 0, 16);
    const QString module = QString::fromLocal8Bit(info.dli_fname).section(QLatin1Char('/'), -1);
    if (!info.dli_sname)
        return QStringLiteral("%1+0x%2").arg(module).arg(address - std::uintptr_t(info.dli_fbase), 0, 16);
    int status = 0;
    char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
    QString name = QString::fromLocal8Bit(status == 0 && demangled ? demangled : info.dli_sname);
    std::free(demangled);
    if (name.size() > 160)
        name = name.left(160) + QStringLiteral("…");
    return QStringLiteral("%1 (%2)").arg(name, module);
}
#endif

// CPU time used so far, in ms: by the GUI thread, and by the whole process.
// Over a stall they tell a busy GUI thread (its time grows as fast as the
// clock), one that waits (neither grows), and other threads using the CPU.
struct CpuTimes
{
    qint64 gui = 0;
    qint64 process = 0;
};

CpuTimes cpuTimes()
{
    CpuTimes times;
#ifdef Q_OS_WIN
    const auto ms = [](const FILETIME &kernel, const FILETIME &user) {
        const auto value = [](const FILETIME &time) {
            return (qint64(time.dwHighDateTime) << 32 | time.dwLowDateTime) / 10000;
        };
        return value(kernel) + value(user);
    };
    FILETIME creation, exit, kernel, user;
    if (GetThreadTimes(guiThread, &creation, &exit, &kernel, &user))
        times.gui = ms(kernel, user);
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        times.process = ms(kernel, user);
#else
    const auto ms = [](clockid_t clock) {
        timespec time{};
        return clock_gettime(clock, &time) == 0 ? qint64(time.tv_sec) * 1000 + time.tv_nsec / 1000000 : 0;
    };
    clockid_t clock;
    if (pthread_getcpuclockid(guiThread, &clock) == 0)
        times.gui = ms(clock);
    times.process = ms(CLOCK_PROCESS_CPUTIME_ID);
#endif
    return times;
}

QString describe(const Stack &stack)
{
    QStringList lines;
    for (int index = 0; index < stack.count; ++index)
        lines << QStringLiteral("    #%1 %2").arg(index).arg(describe(stack.frames[index]));
    return lines.join(QLatin1Char('\n'));
}

// The worker that notices the stalls. It never touches widgets.
void watch()
{
    QHash<QString, int> stacks; // described stack -> times seen in this stall
    qint64 stallStart = 0;
    CpuTimes before, last = cpuTimes();
    qint64 lateMost = 0; // the longest this thread overslept during the stall
    QStringList others;  // the other threads, sampled once as the stall is noticed
    while (!QThread::currentThread()->isInterruptionRequested()) {
        const int sleepMs = stallStart ? SampleMs : TickMs;
        const qint64 asleep = sinceStart.elapsed();
        QThread::msleep(sleepMs);
        const qint64 now = sinceStart.elapsed();
        const qint64 quiet = now - lastTick;
        if (quiet > stallMs) {
            if (!stallStart) {
                stallStart = lastTick;
                before = last; // from before the stall began
                lateMost = 0;
#ifdef Q_OS_WIN
                others.clear();
                for (const auto &[id, stack] : sampleOthers()) {
                    if (stack.count)
                        others << QStringLiteral("  thread %1:\n%2").arg(id).arg(describe(stack));
                }
#endif
            }
            lateMost = std::max(lateMost, now - asleep - sleepMs);
            const Stack stack = sample();
            if (stack.count)
                ++stacks[describe(stack)];
            continue;
        }
        if (!stallStart) {
            last = cpuTimes();
            continue;
        }
        const CpuTimes after = cpuTimes();
        last = after;
        // Over: the stacks seen most often first, those are where it waited.
        QList<QPair<int, QString>> sorted;
        int samples = 0;
        for (auto it = stacks.cbegin(); it != stacks.cend(); ++it) {
            sorted.append({it.value(), it.key()});
            samples += it.value();
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        QString report = QStringLiteral("GUI stall: %1 ms, %2 samples; CPU used meanwhile: GUI thread %3 ms, "
                                        "other threads %4 ms; this watcher woke up to %5 ms late")
                             .arg(lastTick - stallStart)
                             .arg(samples)
                             .arg(after.gui - before.gui)
                             .arg((after.process - before.process) - (after.gui - before.gui))
                             .arg(lateMost);
        for (qsizetype index = 0; index < std::min<qsizetype>(sorted.size(), 3); ++index)
            report += QStringLiteral("\n  stack seen %1 times:\n%2").arg(sorted[index].first).arg(sorted[index].second);
        if (!others.isEmpty())
            report += QStringLiteral("\n  other threads when it was noticed:\n") + others.join(QLatin1Char('\n'));
        qWarning().noquote() << report;
        stacks.clear();
        others.clear();
        stallStart = 0;
    }
}

}

#ifdef Q_OS_WIN
namespace {
// Logs a serious exception (any thread) with its stack, then lets things go
// on as before. As it happens ("first chance"), so also those that Windows
// catches itself, for example in a library's thread-exit code, and reports
// silently through Windows Error Reporting.
LONG WINAPI logCrash(EXCEPTION_POINTERS *exception)
{
    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    // Only faults: not breakpoints, C++ (GCC 0x20474343) or other exceptions
    // that programs raise and handle on purpose.
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW
        && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO
        && code != EXCEPTION_PRIV_INSTRUCTION && code != STATUS_HEAP_CORRUPTION
        && code != EXCEPTION_IN_PAGE_ERROR && code != 0xC0000409 /* fast fail */)
        return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic_int logged{0};
    if (logged.fetch_add(1) >= 20)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT context = *exception->ContextRecord;
    Stack stack;
    while (stack.count < MaximumFrames && context.Rip) {
        stack.frames[stack.count++] = context.Rip;
        DWORD64 base = 0;
        auto *function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
        if (!function) {
            context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
            context.Rsp += 8;
        } else {
            void *handler = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handler, &frame,
                             nullptr);
        }
    }
    const auto *record = exception->ExceptionRecord;
    qCritical().noquote() << QStringLiteral("fault: exception 0x%1 at %2 in thread %3 (%4)%5\n%6")
                                 .arg(qulonglong(record->ExceptionCode), 8, 16, QLatin1Char('0'))
                                 .arg(describe(std::uintptr_t(record->ExceptionAddress)))
                                 .arg(GetCurrentThreadId())
                                 .arg(QThread::currentThread() ? QThread::currentThread()->objectName()
                                                               : QString())
                                 .arg(record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
                                          && record->NumberParameters >= 2
                                          ? QStringLiteral(", %1 address 0x%2")
                                                .arg(record->ExceptionInformation[0] ? QStringLiteral("writing")
                                                                                     : QStringLiteral("reading"))
                                                .arg(qulonglong(record->ExceptionInformation[1]), 0, 16)
                                          : QString())
                                 .arg(describe(stack));
    return EXCEPTION_CONTINUE_SEARCH;
}
}

void logCrashes()
{
    AddVectoredExceptionHandler(1, logCrash);
}
#else
void logCrashes()
{
}
#endif

void watchGuiStalls(QObject *owner)
{
    if (const int ms = qEnvironmentVariableIntValue("OPSREMOTE_LOG_STALL_MS"); ms > 0)
        stallMs = std::max(ms, 2 * TickMs); // less would report the ticks themselves
    sinceStart.start();
#ifdef Q_OS_WIN
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &guiThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
#else
    guiThread = pthread_self();
    void *warmUp[1];
    backtrace(warmUp, 1);
    struct sigaction action{};
    action.sa_handler = handleSample;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGURG, &action, nullptr);
#endif
    auto *tick = new QTimer(owner);
    QObject::connect(tick, &QTimer::timeout, owner, [] { lastTick = sinceStart.elapsed(); });
    tick->start(TickMs);
    lastTick = sinceStart.elapsed();
    auto *thread = QThread::create(watch);
    thread->setObjectName(QStringLiteral("GUI stall watch"));
    // Ended with the application; it only sleeps and samples.
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread] {
        thread->requestInterruption();
        thread->wait(1000);
    });
    thread->start();
#ifdef Q_OS_WIN
    // The DLLs loaded so far, to see which ones run code as threads end.
    QTimer::singleShot(5000, owner, [] {
        HMODULE modules[512];
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) {
            QStringList names;
            for (DWORD index = 0; index < std::min<DWORD>(needed / sizeof(HMODULE), 512); ++index) {
                wchar_t path[MAX_PATH] = {};
                GetModuleFileNameW(modules[index], path, MAX_PATH);
                names << QString::fromWCharArray(path);
            }
            qInfo().noquote() << "loaded modules:" << names.join(QStringLiteral("; "));
        }
    });
    // For tests, a thread started and ended every 3 s, to tell which kind
    // makes the program stall as it ends: 1 a bare Windows thread that does
    // nothing, 2 a QThread (which makes a hidden window for its events), 3 a
    // bare thread that makes and destroys a hidden window, 4 a bare thread
    // that uses a C++ thread_local (libgcc's emutls), 5 an empty std::thread
    // (winpthreads), 6 a worker on a kept thread (workerthreads.h), released
    // again like a session that closes.
    if (const int kind = qEnvironmentVariableIntValue("OPSREMOTE_TEST_THREADS"); kind > 0) {
        auto *churn = new QTimer(owner);
        QObject::connect(churn, &QTimer::timeout, owner, [kind] {
            if (kind == 6) {
                auto *worker = new QObject;
                worker->moveToThread(WorkerThreads::acquire(QStringLiteral("test worker")));
                WorkerThreads::release(worker);
            } else if (kind == 5) {
                std::thread([] {}).detach();
            } else if (kind == 4) {
                if (HANDLE thread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                        static thread_local int used = 0;
                        return DWORD(++used);
                    }, nullptr, 0, nullptr))
                    CloseHandle(thread);
            } else if (kind == 2) {
                auto *thread = QThread::create([] {});
                QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
                thread->start();
            } else {
                const auto body = kind == 3 ? [](LPVOID) -> DWORD {
                    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                                  nullptr, nullptr);
                    if (window)
                        DestroyWindow(window);
                    return 0;
                } : [](LPVOID) -> DWORD { return 0; };
                if (HANDLE thread = CreateThread(nullptr, 0, body, nullptr, 0, nullptr))
                    CloseHandle(thread);
            }
            qInfo("test thread started (kind %d)", kind);
        });
        churn->start(3000);
    }
#endif
    // For tests: blocks the GUI thread that long, two seconds after start.
    if (const int stallMs = qEnvironmentVariableIntValue("OPSREMOTE_TEST_STALL_MS"); stallMs > 0)
        QTimer::singleShot(2000, owner, [stallMs] { QThread::msleep(stallMs); });
}
