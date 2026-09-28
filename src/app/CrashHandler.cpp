// SPDX-License-Identifier: GPL-3.0-or-later
#include "CrashHandler.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <cstdarg>
#include <cstdlib>
#elif defined(Q_OS_UNIX)
#include <csignal>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

#ifndef VERTEXA_VERSION
#define VERTEXA_VERSION "dev"
#endif
#ifndef VERTEXA_BUILD_ID
#define VERTEXA_BUILD_ID "local"
#endif

namespace vx::app::crash {

namespace {

// Everything the crash handler reads lives in fixed buffers prepared in
// advance: after a crash nothing may be allocated.
constexpr size_t kRing = 48 * 1024;
char g_ring[kRing];
size_t g_ringPos = 0;
bool g_ringFull = false;

char g_header[8 * 1024];
size_t g_headerLen = 0;
char g_reason[1024]; ///< what led to abort() (uncaught exception, qFatal)

char g_reportPath[2048];
char g_dumpPath[2048];
char g_recoveryPath[2048];
#if defined(Q_OS_WIN)
wchar_t g_reportPathW[1024];
wchar_t g_dumpPathW[1024];
#endif

std::mutex g_mutex; ///< log, context
QFile* g_log = nullptr;
QtMessageHandler g_previous = nullptr;
std::map<QString, QString> g_context;
QString g_id;
QString g_logPath;
QString g_started;
std::unique_ptr<QLockFile> g_lock;
std::function<Saved(const QString&)> g_save;
time_t g_start = 0;
std::atomic<bool> g_installed{false};

void copyTo(char* dst, size_t size, const QByteArray& src)
{
    const size_t n = std::min(size - 1, size_t(src.size()));
    std::memcpy(dst, src.constData(), n);
    dst[n] = 0;
}

void appendRing(const QByteArray& line)
{
    for (char c : line) {
        g_ring[g_ringPos++] = c;
        if (g_ringPos == kRing) {
            g_ringPos = 0;
            g_ringFull = true;
        }
    }
}

// Rebuilt whenever the context changes (under g_mutex).
void rebuildHeader()
{
    QString h;
    h += QString("Version: %1 (build %2)\n").arg(VERTEXA_VERSION, VERTEXA_BUILD_ID);
    h += QString("Started: %1\n").arg(g_started);
    h += QString("System: %1 (%2 %3, %4), %5 threads\n")
             .arg(QSysInfo::prettyProductName(), QSysInfo::kernelType(), QSysInfo::kernelVersion(),
                  QSysInfo::currentCpuArchitecture())
             .arg(QThread::idealThreadCount());
    h += QString("Qt: %1 (built with %2)").arg(qVersion(), QT_VERSION_STR);
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance())) h += QString(", platform %1").arg(QGuiApplication::platformName());
    h += "\n";
    for (const auto& [k, v] : g_context) h += QString("%1: %2\n").arg(k, v);
    h += QString("Log: %1\n").arg(QDir::toNativeSeparators(g_logPath));
    copyTo(g_header, sizeof g_header, h.toUtf8());
    g_headerLen = std::strlen(g_header);
}

// Windows: qFatal ends the process with a fail-fast that no handler sees, so
// the report is written before Qt gets there.
void reportFatal();

void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    const char* level = "debug";
    switch (type) {
    case QtDebugMsg: level = "debug"; break;
    case QtInfoMsg: level = "info"; break;
    case QtWarningMsg: level = "warning"; break;
    case QtCriticalMsg: level = "critical"; break;
    case QtFatalMsg: level = "fatal"; break;
    }
    const bool ours = ctx.category && std::strncmp(ctx.category, "vx.", 3) == 0;
    QString text = QTime::currentTime().toString("HH:mm:ss.zzz") + ' ' + level + ' ';
    if (ctx.category && std::strcmp(ctx.category, "default") != 0) text += QString("[%1] ").arg(ctx.category);
    text += msg;
    const QByteArray line = text.toUtf8() + '\n';
    {
        std::lock_guard lock(g_mutex);
        appendRing(line);
        if (g_log) {
            g_log->write(line);
            g_log->flush();
        }
        if (type == QtFatalMsg) copyTo(g_reason, sizeof g_reason, "qFatal: " + msg.toUtf8());
    }
    // The console keeps Qt's own output; breadcrumbs only go to the log.
    if (!(ours && type == QtInfoMsg)) {
        if (g_previous) g_previous(type, ctx, msg);
        else std::fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, ctx, msg)));
    }
    if (type == QtFatalMsg) reportFatal();
}

[[noreturn]] void onTerminate()
{
    QByteArray what = "std::terminate called";
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            what = QByteArray("uncaught exception: ") + x.what();
        } catch (...) {
            what = "uncaught exception of an unknown type";
        }
    }
    copyTo(g_reason, sizeof g_reason, what);
    qCritical("%s", what.constData());
    std::abort();
}

void prune(const QString& dir, const QStringList& patterns, int keep)
{
    QDir d(dir);
    const QFileInfoList files = d.entryInfoList(patterns, QDir::Files, QDir::Time);
    for (int i = keep; i < files.size(); ++i) QFile::remove(files[i].absoluteFilePath());
}

QString recoveryDir() { return dataDir() + "/recovery"; }

// --- the crash itself -------------------------------------------------------

#if defined(Q_OS_UNIX)

pthread_t g_mainThread;

void put(int fd, const char* s, size_t n)
{
    while (n > 0) {
        const ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= size_t(w);
    }
}
void put(int fd, const char* s) { put(fd, s, std::strlen(s)); }
void putNumber(int fd, unsigned long long v, int base)
{
    char buf[32];
    int i = int(sizeof buf);
    do {
        buf[--i] = "0123456789abcdef"[v % unsigned(base)];
        v /= unsigned(base);
    } while (v && i > 0);
    put(fd, buf + i, sizeof buf - size_t(i));
}

const char* signalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT (abort)";
    }
    return "signal";
}

void writeRing(int fd)
{
    if (g_ringFull) put(fd, g_ring + g_ringPos, kRing - g_ringPos);
    put(fd, g_ring, g_ringPos);
}

// Function names of the frames, demangled. It allocates, so it runs last,
// when everything else is on disk.
void writeNamedStack(int fd, void* const* frames, int n)
{
    put(fd, "\nStack with names:\n");
    for (int i = 0; i < n; ++i) {
        Dl_info dl{};
        put(fd, "#");
        putNumber(fd, unsigned(i), 10);
        put(fd, " ");
        if (dladdr(frames[i], &dl) && dl.dli_fname) {
            const char* module = std::strrchr(dl.dli_fname, '/');
            put(fd, module ? module + 1 : dl.dli_fname);
            if (dl.dli_sname) {
                int status = -1;
                char* name = abi::__cxa_demangle(dl.dli_sname, nullptr, nullptr, &status);
                put(fd, "  ");
                put(fd, status == 0 && name ? name : dl.dli_sname);
                std::free(name);
                put(fd, " +0x");
                putNumber(fd, uintptr_t(frames[i]) - uintptr_t(dl.dli_saddr), 16);
            } else {
                put(fd, " +0x");
                putNumber(fd, uintptr_t(frames[i]) - uintptr_t(dl.dli_fbase), 16);
            }
        } else {
            put(fd, "0x");
            putNumber(fd, uintptr_t(frames[i]), 16);
        }
        put(fd, "\n");
    }
}

void onSignal(int sig, siginfo_t* info, void*)
{
    static volatile sig_atomic_t entered = 0;
    if (entered) _exit(128 + sig);
    entered = 1;
    alarm(20); // never hang: SIGALRM ends the process
    static void* frames[128];
    int n = 0;
    const int fd = open(g_reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        put(fd, "Vertexa crash report\n\n");
        put(fd, g_header, g_headerLen);
        put(fd, "\nCrash: ");
        put(fd, signalName(sig));
        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL) {
            put(fd, " at address 0x");
            putNumber(fd, reinterpret_cast<uintptr_t>(info->si_addr), 16);
        }
        put(fd, "\n");
        if (g_reason[0]) {
            put(fd, "Reason: ");
            put(fd, g_reason);
            put(fd, "\n");
        }
        put(fd, "Thread: ");
        put(fd, pthread_equal(pthread_self(), g_mainThread) ? "main (user interface)" : "worker");
        put(fd, "\nUptime: ");
        putNumber(fd, static_cast<unsigned long long>(time(nullptr) - g_start), 10);
        put(fd, " s\n\nStack:\n");
        n = backtrace(frames, 128);
        backtrace_symbols_fd(frames, n, fd);
        put(fd, "\nLog (latest last):\n");
        writeRing(fd);
    }
    // Unsaved work, last: this may not work out in a damaged process.
    if (g_save) {
        alarm(10);
        const Saved s = g_save(QString::fromUtf8(g_recoveryPath));
        if (fd >= 0 && s != Saved::Nothing) {
            put(fd, s == Saved::Saved ? "\nUnsaved work saved to: " : "\nUnsaved work could not be saved.\n");
            if (s == Saved::Saved) {
                put(fd, g_recoveryPath);
                put(fd, "\n");
            }
        }
    }
    if (fd >= 0) {
        alarm(5);
        writeNamedStack(fd, frames, n);
        close(fd);
    }
    // Let the system finish (core dump, the platform's crash reporter).
    signal(sig, SIG_DFL);
    raise(sig);
}

void reportFatal() {} // qFatal aborts: the SIGABRT handler reports it

void installPlatform()
{
    g_mainThread = pthread_self();
    // Its own stack, so that a stack overflow is reported too.
    static std::unique_ptr<char[]> altStack(new char[256 * 1024]);
    stack_t ss{};
    ss.ss_sp = altStack.get();
    ss.ss_size = 256 * 1024;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = onSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(sig, &sa, nullptr);
    // backtrace() loads the unwinder on first use: not in the handler.
    void* warm[4];
    backtrace(warm, 4);
}

#elif defined(Q_OS_WIN)

DWORD g_mainThread = 0;

void put(HANDLE f, const char* s, size_t n)
{
    DWORD written = 0;
    WriteFile(f, s, DWORD(n), &written, nullptr);
}
void put(HANDLE f, const char* s) { put(f, s, std::strlen(s)); }
void putf(HANDLE f, const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    const int n = _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, args);
    va_end(args);
    if (n > 0) put(f, buf, size_t(n));
    else if (n < 0) put(f, buf, std::strlen(buf));
}

const char* exceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION (invalid memory access)";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case 0xE06D7363: return "C++ exception";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (fail fast)";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    }
    return "exception";
}

void writeStack(HANDLE f, CONTEXT ctx)
{
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    STACKFRAME64 frame{};
    DWORD machine;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrFrame.Offset = ctx.Rbp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = ctx.Pc;
    frame.AddrStack.Offset = ctx.Sp;
    frame.AddrFrame.Offset = ctx.Fp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrStack.Offset = ctx.Esp;
    frame.AddrFrame.Offset = ctx.Ebp;
#endif
    frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
    for (int i = 0; i < 96; ++i) {
        if (!StackWalk64(machine, process, GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr))
            break;
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0) break;
        char module[MAX_PATH] = "?";
        const DWORD64 base = SymGetModuleBase64(process, pc);
        if (base && GetModuleFileNameA(HMODULE(base), module, MAX_PATH)) {
            if (const char* slash = std::strrchr(module, '\\')) std::memmove(module, slash + 1, std::strlen(slash));
        }
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        std::memset(symBuf, 0, sizeof symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD lineDisp = 0;
        if (SymFromAddr(process, pc, &disp, sym)) {
            if (SymGetLineFromAddr64(process, pc, &lineDisp, &line))
                putf(f, "#%-2d %s!%s+0x%llx  (%s:%lu)\n", i, module, sym->Name, disp, line.FileName, line.LineNumber);
            else
                putf(f, "#%-2d %s!%s+0x%llx\n", i, module, sym->Name, disp);
        } else {
            putf(f, "#%-2d %s+0x%llx\n", i, module, base ? pc - base : pc);
        }
    }
}

void writeRing(HANDLE f)
{
    if (g_ringFull) put(f, g_ring + g_ringPos, kRing - g_ringPos);
    put(f, g_ring, g_ringPos);
}

DWORD WINAPI watchdog(LPVOID)
{
    Sleep(20000);
    TerminateProcess(GetCurrentProcess(), 0xDEAD);
    return 0;
}

void report(const char* what, DWORD code, void* address, CONTEXT* ctx, EXCEPTION_POINTERS* ep)
{
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1)) return;
    CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr); // never hang
    HANDLE f = CreateFileW(g_reportPathW, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        put(f, "Vertexa crash report\r\n\r\n");
        put(f, g_header, g_headerLen);
        putf(f, "\nCrash: %s (0x%08lx) at 0x%p\n", what, code, address);
        if (ep && code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            putf(f, "Access: %s address 0x%llx\n", ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" : "reading",
                 (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
        if (g_reason[0]) putf(f, "Reason: %s\n", g_reason);
        putf(f, "Thread: %s\n", GetCurrentThreadId() == g_mainThread ? "main (user interface)" : "worker");
        putf(f, "Uptime: %lld s\n\nStack:\n", (long long)(time(nullptr) - g_start));
        if (ctx) writeStack(f, *ctx);
        put(f, "\nLog (latest last):\n");
        writeRing(f);
    }
    // A minidump for the debugger (open it with the matching vertexa.pdb).
    HANDLE dump = CreateFileW(g_dumpPathW, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                          MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), ep ? &mei : nullptr, nullptr,
                          nullptr);
        CloseHandle(dump);
        if (f != INVALID_HANDLE_VALUE) putf(f, "\nMinidump: %s\n", g_dumpPath);
    }
    if (g_save) {
        const Saved s = g_save(QString::fromUtf8(g_recoveryPath));
        if (f != INVALID_HANDLE_VALUE && s != Saved::Nothing) {
            if (s == Saved::Saved) putf(f, "\nUnsaved work saved to: %s\n", g_recoveryPath);
            else put(f, "\nUnsaved work could not be saved.\n");
        }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

LONG WINAPI onException(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // An uncaught C++ exception: the runtime's filter makes it the current
    // exception and calls std::terminate (onTerminate), then abort (onAbort).
    if (code == 0xE06D7363 && g_previousFilter) return g_previousFilter(ep);
    report(exceptionName(code), code, ep->ExceptionRecord->ExceptionAddress, ep->ContextRecord, ep);
    return EXCEPTION_EXECUTE_HANDLER; // ends the process
}

void reportHere(const char* what)
{
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    report(what, 0, nullptr, &ctx, nullptr);
}

void onAbort(int)
{
    reportHere("abort()");
    _exit(3);
}

void reportFatal() { reportHere("qFatal"); }

void onPureCall()
{
    reportHere("pure virtual function call");
    _exit(3);
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
{
    reportHere("invalid parameter passed to the C runtime");
    _exit(3);
}

void installPlatform()
{
    g_mainThread = GetCurrentThreadId();
    g_previousFilter = SetUnhandledExceptionFilter(onException);
    signal(SIGABRT, onAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_purecall_handler(onPureCall);
    _set_invalid_parameter_handler(onInvalidParameter);
    // Room for the handler when the stack overflows.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

#else
void reportFatal() {}
void installPlatform() {}
#endif

} // namespace

QString dataDir()
{
    const QString env = qEnvironmentVariable("VERTEXA_CRASH_DIR");
    if (!env.isEmpty()) return env;
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/.vertexa";
    return base;
}

QString reportsDir() { return dataDir() + "/crashes"; }
QString logsDir() { return dataDir() + "/logs"; }
QString logPath() { return g_logPath; }
QString recoveryPath() { return QString::fromUtf8(g_recoveryPath); }

void install()
{
    if (g_installed.exchange(true)) return;
    g_start = time(nullptr);
    const QDateTime now = QDateTime::currentDateTime();
    g_started = now.toString("yyyy-MM-dd HH:mm:ss");
    g_id = now.toString("yyyyMMdd-HHmmss") + '-' + QString::number(QCoreApplication::applicationPid());
    for (const QString& d : {reportsDir(), logsDir(), recoveryDir()}) QDir().mkpath(d);
    prune(logsDir(), {"*.log"}, 10);
    prune(reportsDir(), {"*.txt"}, 30);
    prune(reportsDir(), {"*.dmp"}, 5);

    const QString report = reportsDir() + '/' + g_id + ".txt";
    const QString dump = reportsDir() + '/' + g_id + ".dmp";
    copyTo(g_reportPath, sizeof g_reportPath, QDir::toNativeSeparators(report).toUtf8());
    copyTo(g_dumpPath, sizeof g_dumpPath, QDir::toNativeSeparators(dump).toUtf8());
    copyTo(g_recoveryPath, sizeof g_recoveryPath, (recoveryDir() + '/' + g_id + ".vtx").toUtf8());
#if defined(Q_OS_WIN)
    const std::wstring rw = QDir::toNativeSeparators(report).toStdWString(), dw = QDir::toNativeSeparators(dump).toStdWString();
    wcsncpy_s(g_reportPathW, rw.c_str(), _TRUNCATE);
    wcsncpy_s(g_dumpPathW, dw.c_str(), _TRUNCATE);
#endif

    // The marker of this run: left behind if it does not end normally.
    g_lock = std::make_unique<QLockFile>(recoveryDir() + '/' + g_id + ".lock");
    g_lock->setStaleLockTime(0);
    g_lock->tryLock(0);

    g_logPath = logsDir() + '/' + g_id + ".log";
    {
        std::lock_guard lock(g_mutex);
        g_log = new QFile(g_logPath);
        if (!g_log->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            delete g_log;
            g_log = nullptr;
        }
        rebuildHeader();
    }
    g_previous = qInstallMessageHandler(messageHandler);
    std::set_terminate(onTerminate);
    installPlatform();
    qInfo("Vertexa %s (build %s) started; logs and crash reports in %s", VERTEXA_VERSION, VERTEXA_BUILD_ID,
          qPrintable(QDir::toNativeSeparators(dataDir())));
}

void shutdown()
{
    if (!g_installed) return;
    qInfo("Vertexa quits normally");
    QFile::remove(recoveryPath());
    QFile::remove(recoveryPath() + ".path");
    if (g_lock) g_lock->unlock();
    std::lock_guard lock(g_mutex);
    if (g_log) g_log->flush();
}

void setContext(const QString& key, const QString& value)
{
    std::lock_guard lock(g_mutex);
    if (value.isEmpty()) g_context.erase(key);
    else g_context[key] = value;
    if (g_installed) rebuildHeader();
}

void setEmergencySave(std::function<Saved(const QString& path)> save) { g_save = std::move(save); }

void setRecoveredDocument(const QString& originalPath)
{
    if (!g_installed) return;
    QSaveFile f(recoveryPath() + ".path");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(originalPath.toUtf8());
        f.commit();
    }
}

std::vector<Session> uncleanSessions()
{
    std::vector<Session> out;
    QDir d(recoveryDir());
    const QFileInfoList locks = d.entryInfoList({"*.lock"}, QDir::Files, QDir::Time);
    for (const QFileInfo& fi : locks) {
        const QString id = fi.completeBaseName();
        if (id == g_id) continue;
        QLockFile lock(fi.absoluteFilePath());
        lock.setStaleLockTime(0);
        // Still held: another instance is running.
        if (!lock.tryLock(0)) continue;
        lock.unlock();
        Session s;
        s.id = id;
        if (QFileInfo::exists(reportsDir() + '/' + id + ".txt")) s.report = reportsDir() + '/' + id + ".txt";
        if (QFileInfo::exists(reportsDir() + '/' + id + ".dmp")) s.dump = reportsDir() + '/' + id + ".dmp";
        const QString rec = recoveryDir() + '/' + id + ".vtx";
        if (QFileInfo::exists(rec)) {
            s.recovery = rec;
            QFile p(rec + ".path");
            if (p.open(QIODevice::ReadOnly)) s.originalPath = QString::fromUtf8(p.readAll());
        }
        out.push_back(s);
    }
    // Recovery files nobody claimed for a month.
    const QDateTime old = QDateTime::currentDateTime().addDays(-30);
    for (const QFileInfo& fi : d.entryInfoList({"*.vtx", "*.path"}, QDir::Files))
        if (fi.lastModified() < old) QFile::remove(fi.absoluteFilePath());
    return out;
}

void discardRecovery(const Session& s)
{
    if (s.recovery.isEmpty()) return;
    QFile::remove(s.recovery);
    QFile::remove(s.recovery + ".path");
}

void crashForTesting(const QString& how)
{
    qWarning("Crashing on purpose (%s)", qPrintable(how));
    if (how == "abort") std::abort();
    if (how == "exception") throw std::runtime_error("crash test");
    volatile int* p = reinterpret_cast<int*>(std::strtoull("0", nullptr, 10));
    *p = 42;
}

QString readReport(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QString reportSummary(const QString& text)
{
    const QRegularExpression crash("^Crash: (.*)$", QRegularExpression::MultilineOption);
    const QRegularExpression reason("^Reason: (.*)$", QRegularExpression::MultilineOption);
    QString s = crash.match(text).captured(1).trimmed();
    const QString r = reason.match(text).captured(1).trimmed();
    if (!r.isEmpty()) s += " — " + r;
    return s.isEmpty() ? QString("unknown") : s;
}

} // namespace vx::app::crash
