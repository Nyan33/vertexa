// SPDX-License-Identifier: GPL-3.0-or-later
// Crash reports: the test runs itself again to crash in different ways, then
// reads what the crash handler left behind.
#include "TestMain.h"

#include "app/CrashHandler.h"
#include "app/Dialogs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

using namespace vx::app;

namespace {

// --- the crashing child ----------------------------------------------------------

void invalidWrite()
{
    // Not a constant the compiler could see through.
    volatile int* p = reinterpret_cast<int*>(std::strtoull("0", nullptr, 10));
    *p = 42;
}

[[noreturn]] void crashChild(const char* how)
{
    if (!std::strcmp(how, "segv")) invalidWrite();
    if (!std::strcmp(how, "abort")) std::abort();
    if (!std::strcmp(how, "exception")) throw std::runtime_error("boom from the test");
    if (!std::strcmp(how, "fatal")) qFatal("fatal from the test");
    if (!std::strcmp(how, "worker")) {
        std::thread t(invalidWrite);
        t.join();
    }
    if (!std::strcmp(how, "killed")) std::_Exit(0); // no shutdown(): like a killed process
    std::_Exit(0);
}

int runChild(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("VertexaTests");
    QCoreApplication::setApplicationName("VertexaCrashTest");
    crash::install();
    crash::setContext("Document", "test.vtx");
    crash::setEmergencySave([](const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return crash::Saved::Failed;
        f.write("{\"unsaved\": true}");
        return crash::Saved::Saved;
    });
    qInfo("before the crash");
    if (!std::strcmp(argv[2], "clean")) {
        crash::shutdown();
        return 0;
    }
    crashChild(argv[2]);
}

// --- the test side -----------------------------------------------------------------

struct Result {
    QString dir;
    int exitCode = 0;
    QProcess::ExitStatus status = QProcess::NormalExit;
    QString report;
    QString reportPath;
};

QTemporaryDir& base()
{
    static QTemporaryDir dir;
    return dir;
}

Result run(const QString& how)
{
    Result r;
    r.dir = base().path() + '/' + how;
    QDir().mkpath(r.dir);
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("VERTEXA_CRASH_DIR", r.dir);
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    p.start(QCoreApplication::applicationFilePath(), {"--crash", how});
    p.waitForFinished(120000);
    r.exitCode = p.exitCode();
    r.status = p.exitStatus();
    const QStringList reports = QDir(r.dir + "/crashes").entryList({"*.txt"}, QDir::Files);
    if (!reports.isEmpty()) {
        r.reportPath = r.dir + "/crashes/" + reports.front();
        r.report = crash::readReport(r.reportPath);
    }
    return r;
}

bool crashed(const Result& r) { return r.status == QProcess::CrashExit || r.exitCode != 0; }

int stackFrames(const QString& report)
{
    const int at = report.indexOf("Stack:\n");
    if (at < 0) return 0;
    const int end = report.indexOf("\nLog", at);
    return int(report.mid(at + 7, end - at - 7).split('\n', Qt::SkipEmptyParts).size());
}

std::vector<crash::Session> sessionsIn(const QString& dir)
{
    qputenv("VERTEXA_CRASH_DIR", dir.toUtf8());
    auto s = crash::uncleanSessions();
    qunsetenv("VERTEXA_CRASH_DIR");
    return s;
}

} // namespace

VX_TEST(invalid_memory_access_is_reported)
{
    const Result r = run("segv");
    CHECK(crashed(r));
    CHECK(r.report.startsWith("Vertexa crash report"));
    CHECK(r.report.contains("SIGSEGV") || r.report.contains("ACCESS_VIOLATION"));
    CHECK(r.report.contains("Version: "));
    CHECK(r.report.contains("Document: test.vtx"));
    CHECK(r.report.contains("Thread: main"));
    CHECK(r.report.contains("before the crash")); // the log
    CHECK(stackFrames(r.report) >= 3);
    CHECK(r.report.contains("Unsaved work saved to"));
    std::printf("  %s: %d stack frames\n", qPrintable(crash::reportSummary(r.report)), stackFrames(r.report));
    // The next start finds the run, its report and the saved work, once.
    const auto sessions = sessionsIn(r.dir);
    CHECK(sessions.size() == 1);
    if (sessions.size() == 1) {
        CHECK(sessions[0].report == r.reportPath);
        CHECK(!sessions[0].recovery.isEmpty() && QFile::exists(sessions[0].recovery));
        crash::discardRecovery(sessions[0]);
        CHECK(!QFile::exists(sessions[0].recovery));
    }
    CHECK(sessionsIn(r.dir).empty());
    // A crash report fits in a GitHub issue link.
    const QUrl url = CrashDialog::issueUrl(r.report, r.reportPath);
    const QString q = url.query(QUrl::FullyEncoded);
    CHECK(q.startsWith("title=Crash%3A%20") && q.contains("&body=") && !q.contains('+'));
    CHECK(url.toEncoded().size() < 12000);
}

VX_TEST(abort_is_reported)
{
    const Result r = run("abort");
    CHECK(crashed(r));
    CHECK(r.report.contains("SIGABRT") || r.report.contains("abort()"));
    CHECK(stackFrames(r.report) >= 3);
}

VX_TEST(uncaught_exception_is_reported)
{
    const Result r = run("exception");
    CHECK(crashed(r));
    CHECK(r.report.contains("Reason: uncaught exception: boom from the test"));
}

VX_TEST(qfatal_is_reported)
{
    const Result r = run("fatal");
    CHECK(crashed(r));
    CHECK(r.report.contains("fatal from the test"));
}

VX_TEST(worker_thread_crash_is_reported)
{
    const Result r = run("worker");
    CHECK(crashed(r));
    CHECK(r.report.contains("Thread: worker"));
}

VX_TEST(killed_and_clean_runs)
{
    // Killed: no report, but the run is found (its unsaved work would be too).
    const Result k = run("killed");
    CHECK(k.report.isEmpty());
    const auto sessions = sessionsIn(k.dir);
    CHECK(sessions.size() == 1 && sessions.front().report.isEmpty());
    // A normal quit leaves nothing to report.
    const Result c = run("clean");
    CHECK(!crashed(c) && c.report.isEmpty());
    CHECK(sessionsIn(c.dir).empty());
    // Every run leaves a log.
    CHECK(!QDir(c.dir + "/logs").entryList({"*.log"}, QDir::Files).isEmpty());
}

int main(int argc, char** argv)
{
    if (argc > 2 && !std::strcmp(argv[1], "--crash")) return runChild(argc, argv);
    QCoreApplication app(argc, argv);
    return vxtest::runAll(argc, argv);
}
