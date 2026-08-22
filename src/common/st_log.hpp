#pragma once

#include <string>

namespace st {

enum log_level {
    LOG_OFF = 0,
    LOG_ERROR = 1,
    LOG_WARN = 2,
    LOG_INFO = 3,
    LOG_DEBUG = 4,
    LOG_TRACE = 5,
};

//: Open the log for `component` (e.g. L"sapi5_x64"); the file lands in
//: st::log_dir().  Safe to call more than once -- later calls are ignored --
//: and safe to skip entirely, in which case logging is simply inert.
void log_open(const wchar_t* component);

void log_close();

//: Raise or lower verbosity at runtime.  The settings file carries a
//: LogLevel key so a user chasing a problem can turn on TRACE without a new
//: build; the default is LOG_DEBUG, which records every SAPI call and every
//: synthesis but not per-audio-block detail.
void log_set_level(int level);

[[nodiscard]] int log_get_level();

//: printf-style, UTF-8.  Cheap to call below the active level: the level
//: check happens before the arguments are formatted.
void log_write(int level, const char* fmt, ...);

//: Human-readable name for a Win32 error code, for logging.
[[nodiscard]] std::string win32_error_text(unsigned long code);

}

//: The level test is in the macro so that formatting a suppressed message
//: costs nothing -- this matters for ST_TRACE, which fires per audio block.
#define ST_LOG_AT(lvl, ...)                                                    \
    do {                                                                       \
        if (::st::log_get_level() >= (lvl)) {                                  \
            ::st::log_write((lvl), __VA_ARGS__);                               \
        }                                                                      \
    } while (0)

#define ST_ERROR(...) ST_LOG_AT(::st::LOG_ERROR, __VA_ARGS__)
#define ST_WARN(...) ST_LOG_AT(::st::LOG_WARN, __VA_ARGS__)
#define ST_INFO(...) ST_LOG_AT(::st::LOG_INFO, __VA_ARGS__)
#define ST_DEBUG(...) ST_LOG_AT(::st::LOG_DEBUG, __VA_ARGS__)
#define ST_TRACE(...) ST_LOG_AT(::st::LOG_TRACE, __VA_ARGS__)
