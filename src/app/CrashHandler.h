// SPDX-License-Identifier: GPL-3.0-or-later
// Vertexa — crash reports, logs and recovery of unsaved work.
//
// Every run writes a log (Qt messages plus what the user did: edits, tools,
// files). A crash — a signal on Linux / macOS, an unhandled exception on
// Windows, an uncaught C++ exception, qFatal — writes a report with the
// version, the system, the renderer, the last lines of the log and the stack
// of the crashing thread (and a minidump on Windows), then saves unsaved
// work. Each run holds a marker; the next start finds the markers of runs
// that did not end normally and offers their reports and saved work.
#pragma once

#include <QString>

#include <functional>
#include <vector>

namespace vx::app::crash {

/// Folder with crash reports, logs and recovery files (the environment
/// variable VERTEXA_CRASH_DIR overrides it).
QString dataDir();
QString reportsDir();
QString logsDir();
/// The log of this run (empty before install()).
QString logPath();

/// Starts logging and catches crashes. Call once, right after the
/// application object is made.
void install();
/// The run ends normally: removes its marker and recovery file.
void shutdown();

/// A fact shown at the top of reports ("Renderer", "Document"…).
void setContext(const QString& key, const QString& value);

/// What the emergency save did.
enum class Saved { Nothing, Saved, Failed };
/// Called after a crash report is written, to save unsaved work to `path`.
/// Best effort: the process is in an unknown state and has seconds to do it.
void setEmergencySave(std::function<Saved(const QString& path)> save);
/// Where unsaved work of this run is saved (autosaves and the crash save).
QString recoveryPath();
/// Remembers which document the recovery file belongs to.
void setRecoveredDocument(const QString& originalPath);

/// A run that did not end normally.
struct Session {
    QString id;
    QString report;       ///< crash report; empty when the run was killed
    QString dump;         ///< minidump (Windows)
    QString recovery;     ///< unsaved work, if any was saved
    QString originalPath; ///< the document the unsaved work belongs to
};
/// Runs that ended without shutdown(), newest first. Each is reported once:
/// its marker is removed; reports stay in reportsDir().
std::vector<Session> uncleanSessions();
/// Deletes the unsaved work kept for a session.
void discardRecovery(const Session& s);

/// Crashes on purpose ("segv", "abort", "exception"), to check reports.
void crashForTesting(const QString& how);

/// Crash report as text, for copying or an issue.
QString readReport(const QString& path);
/// Short title of a report ("SIGSEGV at …").
QString reportSummary(const QString& text);

} // namespace vx::app::crash
