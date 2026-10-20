#pragma once
namespace bg3le {
void log_init();
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// "tid N name" for the calling thread, for logs that need to say which.
const char* thread_tag();

// Path of this process's log file, or "" before log_init().
const char* log_path();

// Records that this run crashed, for the next launch to report.
void log_mark_crash();

// Path of the previous run's log if that run crashed, else "".
const char* log_previous_crash();
}
