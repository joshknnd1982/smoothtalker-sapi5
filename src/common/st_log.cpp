#include "st_log.hpp"
#include "st_paths.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace st {

namespace {

//: Rotate at 4 MB and keep one previous generation.  A screen reader speaks
//: thousands of utterances an hour, so an uncapped log would quietly eat the
//: disk; two bounded files keep the recent history that a bug report needs.
constexpr long long MAX_LOG_BYTES = 4LL * 1024 * 1024;

std::mutex g_mutex;
std::wstring g_path;
bool g_opened = false;
std::atomic<int> g_level{LOG_DEBUG};

[[nodiscard]] std::string timestamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[40];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds);
    return buf;
}

[[nodiscard]] const char* level_name(int level)
{
    switch (level) {
    case LOG_ERROR: return "ERROR";
    case LOG_WARN:  return "WARN ";
    case LOG_INFO:  return "INFO ";
    case LOG_DEBUG: return "DEBUG";
    case LOG_TRACE: return "TRACE";
    default:        return "?????";
    }
}

//: Caller holds g_mutex.
void rotate_if_needed()
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fad)) {
        return;
    }
    const long long size =
        (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (size < MAX_LOG_BYTES) {
        return;
    }
    const std::wstring old = g_path + L".1";
    DeleteFileW(old.c_str());
    MoveFileW(g_path.c_str(), old.c_str());
}

//: Caller holds g_mutex.  Opened and closed around every record rather than
//: held open: several processes (a screen reader, the config utility, a test
//: harness) log to the same file, and an exclusive handle held for the life
//: of the process would lock the others out.
void append(const std::string& line)
{
    if (g_path.empty()) {
        return;
    }
    rotate_if_needed();
    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written,
              nullptr);
    CloseHandle(h);
}

}

void log_open(const wchar_t* component)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_opened) {
        return;
    }
    g_opened = true;
    g_path = log_dir() + component + L".log";

    wchar_t exe[MAX_PATH + 1] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    char head[1024];
    const std::string ts = timestamp();
    _snprintf_s(head, sizeof(head), _TRUNCATE,
                "\r\n"
                "========================================"
                "========================================\r\n"
                "[%s] SmoothTalker SAPI5 -- log opened\r\n"
                "[%s]   component : %ls\r\n"
                "[%s]   host      : %ls (pid %lu, %d-bit)\r\n"
                "[%s]   module dir: %ls\r\n"
                "========================================"
                "========================================\r\n",
                ts.c_str(), ts.c_str(), component, ts.c_str(), exe,
                GetCurrentProcessId(),
                static_cast<int>(sizeof(void*) * 8), ts.c_str(),
                module_dir().c_str());
    append(head);
}

void log_close()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_opened = false;
    g_path.clear();
}

void log_set_level(int level)
{
    if (level < LOG_OFF) {
        level = LOG_OFF;
    }
    if (level > LOG_TRACE) {
        level = LOG_TRACE;
    }
    g_level.store(level, std::memory_order_relaxed);
}

int log_get_level()
{
    return g_level.load(std::memory_order_relaxed);
}

void log_write(int level, const char* fmt, ...)
{
    if (level > log_get_level()) {
        return;
    }

    std::vector<char> body(2048);
    for (;;) {
        va_list args;
        va_start(args, fmt);
        const int n = _vsnprintf_s(body.data(), body.size(), _TRUNCATE, fmt,
                                   args);
        va_end(args);
        if (n >= 0) {
            body.resize(static_cast<size_t>(n));
            break;
        }
        if (body.size() >= 64 * 1024) {
            body.resize(strlen(body.data()));
            break;
        }
        body.resize(body.size() * 2);
    }

    char prefix[80];
    _snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "[%s] %s [%04lu] ",
                timestamp().c_str(), level_name(level), GetCurrentThreadId());

    std::string line(prefix);
    line.append(body.begin(), body.end());
    line.append("\r\n");

    std::lock_guard<std::mutex> lock(g_mutex);
    append(line);
}

std::string win32_error_text(unsigned long code)
{
    char* buf = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string out;
    if (n && buf) {
        out.assign(buf, n);
        while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) {
            out.pop_back();
        }
    }
    if (buf) {
        LocalFree(buf);
    }
    char code_text[32];
    _snprintf_s(code_text, sizeof(code_text), _TRUNCATE, " (%lu)", code);
    out += code_text;
    return out;
}

}
