#include "st_settings.hpp"
#include "st_log.hpp"
#include "st_paths.hpp"

#include <windows.h>

#include <algorithm>

namespace st {

namespace {

constexpr const wchar_t* SECTION = L"SmoothTalker";

[[nodiscard]] int clamp_to(int v, int lo, int hi)
{
    return (std::max)(lo, (std::min)(hi, v));
}

[[nodiscard]] int read_int(const wchar_t* key, int fallback,
                           const std::wstring& path, int lo, int hi)
{
    // -1 as the "absent" marker rather than `fallback` itself, so a file that
    // genuinely contains the default is indistinguishable from one that does
    // not -- which is fine, both end at the same value.
    const UINT raw = GetPrivateProfileIntW(SECTION, key,
                                           static_cast<INT>(0x7FFFFFFF),
                                           path.c_str());
    if (raw == 0x7FFFFFFFu) {
        return fallback;
    }
    return clamp_to(static_cast<int>(raw), lo, hi);
}

bool write_int(const wchar_t* key, int value, const std::wstring& path)
{
    wchar_t buf[24];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", value);
    return WritePrivateProfileStringW(SECTION, key, buf, path.c_str()) != FALSE;
}

}

void settings::clamp()
{
    rate = clamp_to(rate, VALUE_MIN, VALUE_MAX);
    pitch = clamp_to(pitch, VALUE_MIN, VALUE_MAX);
    tone = clamp_to(tone, TONE_MIN, TONE_MAX);
    volume = clamp_to(volume, VALUE_MIN, VALUE_MAX);
    log_level = clamp_to(log_level, 0, 5);
}

void settings::reset()
{
    rate = RATE_DEFAULT;
    pitch = PITCH_DEFAULT;
    tone = TONE_DEFAULT;
    volume = VOLUME_DEFAULT;
    log_level = LOG_LEVEL_DEFAULT;
}

settings settings_load()
{
    const std::wstring path = settings_path();
    settings s;
    s.rate = read_int(L"Rate", settings::RATE_DEFAULT, path,
                      settings::VALUE_MIN, settings::VALUE_MAX);
    s.pitch = read_int(L"Pitch", settings::PITCH_DEFAULT, path,
                       settings::VALUE_MIN, settings::VALUE_MAX);
    s.tone = read_int(L"Tone", settings::TONE_DEFAULT, path,
                      settings::TONE_MIN, settings::TONE_MAX);
    s.volume = read_int(L"Volume", settings::VOLUME_DEFAULT, path,
                        settings::VALUE_MIN, settings::VALUE_MAX);
    s.log_level = read_int(L"LogLevel", settings::LOG_LEVEL_DEFAULT, path, 0, 5);
    return s;
}

bool settings_save(const settings& in)
{
    settings s = in;
    s.clamp();
    const std::wstring path = settings_path();

    bool ok = true;
    ok &= write_int(L"Rate", s.rate, path);
    ok &= write_int(L"Pitch", s.pitch, path);
    ok &= write_int(L"Tone", s.tone, path);
    ok &= write_int(L"Volume", s.volume, path);
    ok &= write_int(L"LogLevel", s.log_level, path);

    // WritePrivateProfileString buffers writes to the mapped-file cache; the
    // flush makes the change visible to other processes (the running screen
    // reader, above all) before this call returns, which is what "takes
    // effect immediately" depends on.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());

    if (!ok) {
        ST_ERROR("settings: could not write %ls: %s", path.c_str(),
                 win32_error_text(GetLastError()).c_str());
    } else {
        ST_INFO("settings: saved rate=%d pitch=%d tone=%d volume=%d "
                "loglevel=%d -> %ls",
                s.rate, s.pitch, s.tone, s.volume, s.log_level, path.c_str());
    }
    return ok;
}

settings_watcher::settings_watcher() : path_(settings_path()) {}

void settings_watcher::invalidate()
{
    loaded_ = false;
    have_stamp_ = false;
}

bool settings_watcher::file_changed()
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fad)) {
        // No file yet.  Treat "absent" as a state of its own so that
        // creating the file later is seen as a change.
        const bool was = have_stamp_;
        have_stamp_ = false;
        return was;
    }
    const unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
        fad.nFileSizeLow;
    if (have_stamp_ &&
        CompareFileTime(&stamp_, &fad.ftLastWriteTime) == 0 &&
        size == size_) {
        return false;
    }
    stamp_ = fad.ftLastWriteTime;
    size_ = size;
    have_stamp_ = true;
    return true;
}

settings settings_watcher::get()
{
    if (!loaded_ || file_changed()) {
        cached_ = settings_load();
        loaded_ = true;
        log_set_level(cached_.log_level);
        ST_DEBUG("settings: (re)loaded rate=%d pitch=%d tone=%d volume=%d "
                 "loglevel=%d",
                 cached_.rate, cached_.pitch, cached_.tone, cached_.volume,
                 cached_.log_level);
    }
    return cached_;
}

}
