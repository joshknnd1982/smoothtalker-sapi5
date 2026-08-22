#pragma once

#include <windows.h>

#include <string>

namespace st {

//: Every engine parameter SmoothTalker 3.5 actually honours.
//:
//: `gender` exists in the ROM's settings struct but is inert on this build --
//: all ten values render byte-identical audio, and the image's own copyright
//: string says "SmoothTalker (R), Version 3.5, male voice" -- so it is not
//: surfaced here.  The rest are the engine's native 0-9 scale (tone 0-1);
//: values outside that range clamp inside the ROM, and we clamp before
//: sending so what we log is what the engine heard.
struct settings {
    int rate = 5;    // engine "speed",  0 slowest .. 9 fastest
    int pitch = 5;   // 0 (~41 Hz) .. 9 (~154 Hz)
    int tone = 0;    // 0 full, 1 bright (cuts the low end)
    int volume = 5;  // 0 quietest .. 9 loudest
    int log_level = 4;

    static constexpr int VALUE_MIN = 0;
    static constexpr int VALUE_MAX = 9;
    static constexpr int TONE_MIN = 0;
    static constexpr int TONE_MAX = 1;

    static constexpr int RATE_DEFAULT = 5;
    static constexpr int PITCH_DEFAULT = 5;
    static constexpr int TONE_DEFAULT = 0;
    static constexpr int VOLUME_DEFAULT = 5;
    static constexpr int LOG_LEVEL_DEFAULT = 4;

    void clamp();
    void reset();
};

//: Read the settings file, filling anything missing or out of range with the
//: default.  Never fails: a missing or unreadable file yields the defaults.
[[nodiscard]] settings settings_load();

//: Write the settings file.  Returns false and logs on failure.
bool settings_save(const settings& s);

//: A settings view that re-reads the file when it changes on disk.
//:
//: This is what makes the configuration utility's changes take effect
//: immediately: the SAPI engine calls get() once per utterance, and a
//: timestamp/size comparison -- one cheap metadata query, no file open --
//: tells it whether anything moved since last time.  Polling beats a change
//: notification here because the engine is only allowed to act between
//: utterances anyway, so there is nothing to react to sooner.
class settings_watcher
{
public:
    settings_watcher();

    //: Current settings, re-read if the file changed since the last call.
    [[nodiscard]] settings get();

    //: Force a re-read on the next get().
    void invalidate();

private:
    [[nodiscard]] bool file_changed();

    std::wstring path_;
    settings cached_;
    FILETIME stamp_{};
    unsigned long long size_ = 0;
    bool have_stamp_ = false;
    bool loaded_ = false;
};

}
