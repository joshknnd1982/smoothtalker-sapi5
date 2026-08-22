#pragma once

//: SmoothTalker 3.5 (First Byte / Dr. Sbaitso) speech engine.
//:
//: The original 1990 16-bit DOS engine is run under Unicorn.  There is no
//: DOS and no DOSBox: engine.bin is a snapshot of conventional memory taken
//: with SBTALKER already resident, mapped verbatim at linear 0 so every far
//: pointer inside it stays valid.  Nothing here touches the registry, and
//: nothing here goes through SAPI4.
//:
//: Engine interface, recovered by reverse engineering the original binaries:
//:     INT 2Fh AX=0FBFBh -> ES:BX = descriptor
//:     [ES:BX+4] = far entry point,  ES:BX+20h = text buffer
//:     buffer[0] = LENGTH byte, text follows at buffer+1
//:     AL = 7, then CALL FAR the entry point
//:
//: This is a direct port of the reference implementation in
//: bin/_smoothtalker_engine/core.py and is verified sample-for-sample
//: against it -- see tools/compare_engines.py.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st {

//: The five words the ROM reads out of its settings block.  Field order is
//: First Byte's own: READ.EXE and SET-ECHO.EXE both embed the struct's field
//: list as "outstring, gender, tone, volume, pitch, speed, startpos, action".
struct engine_params {
    int gender = 0;  // inert on this ROM -- see st_settings.hpp
    int tone = 0;    // 0/1
    int volume = 5;  // 0-9
    int pitch = 5;   // 0-9
    int speed = 5;   // 0-9
};

//: Longest utterance the ROM accepts in one call.  buffer[0] is a single
//: length byte and the text area is exactly 0x100 bytes before `outstring`
//: begins at buffer+0x100, so 255 characters is structural.  Overrunning it
//: is worse than truncation: 256 wraps the length byte to 0 (silence) and
//: 300 wraps to 44 (a fragment).
inline constexpr int MAX_TEXT = 255;

//: Break text into pieces the ROM can take in one call, cutting at the
//: strongest break available -- sentence end, then clause, then word -- so
//: that prosody restarts somewhere a listener expects a pause.  Splitting at
//: arbitrary points is what makes the speech sound like it has spurious
//: commas in it.
[[nodiscard]] std::vector<std::string> split_for_engine(const std::string& text,
                                                        int limit = MAX_TEXT);

//: Sound Blaster 8-bit unsigned -> 16-bit signed, which every Windows audio
//: path takes.
void to_pcm16(const uint8_t* pcm8, size_t count, std::vector<int16_t>& out);

//: Streaming linear resampler for 16-bit mono.
//:
//: Carries the phase and the last sample across calls, so feeding audio in
//: DMA-block-sized pieces gives the same continuous result as converting the
//: whole utterance at once.  Resampling each block independently would leave
//: a discontinuity, and therefore a click, at every block boundary.
class resampler
{
public:
    resampler(int src_rate, int dst_rate);

    void feed(const int16_t* src, size_t count, std::vector<int16_t>& out);

private:
    double ratio_ = 1.0;
    double pos_ = 0.0;
    int16_t prev_ = 0;
    bool passthrough_ = false;
};

class engine
{
public:
    //: Called with each DMA block as the ROM produces it, so playback can
    //: start long before a long utterance has finished rendering.  `rate` is
    //: the engine's native sample rate (8475 Hz on this ROM).  Return false
    //: to abandon synthesis.
    using block_fn =
        std::function<bool(const uint8_t* pcm8, size_t count, int rate)>;

    //: Polled periodically during emulation; return true to abort.
    using cancel_fn = std::function<bool()>;

    ~engine();

    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;

    //: Locate engine.bin next to this module and load it.  Returns nullptr
    //: on failure, with the reason in `error` (also logged).
    [[nodiscard]] static std::unique_ptr<engine> create(std::wstring* error);

    //: Load a specific image.  Used by the test harness.
    [[nodiscard]] static std::unique_ptr<engine> create_from(
        const std::wstring& image_path, std::wstring* error);

    //: Write the settings block and re-initialise the ROM.  Parameters
    //: persist on the resident engine until changed.
    bool configure(const engine_params& p);

    //: Synthesize one piece of at most MAX_TEXT characters.  `text` must
    //: already be CP437.  Returns false if the ROM faulted; a cancellation
    //: is not a failure.
    bool speak(const std::string& text, const block_fn& on_block,
               const cancel_fn& should_cancel);

    //: Native output rate, valid once something has been spoken.  The ROM
    //: programs the DSP time constant itself; on this image it works out to
    //: 8475 Hz.
    [[nodiscard]] int native_rate() const noexcept { return native_rate_; }

    [[nodiscard]] const std::wstring& image_path() const noexcept
    {
        return image_path_;
    }

    //: Convenience for callers that want the whole utterance at once:
    //: chunks `text`, renders every piece into one 16-bit buffer at
    //: `out_rate`, and returns it.  Used by the SAPI engine and the test
    //: harness alike.
    bool speak_all(const std::string& cp437_text, int out_rate,
                   std::vector<int16_t>& out, const cancel_fn& should_cancel);

private:
    engine() = default;

    class impl;
    std::unique_ptr<impl> p_;
    std::wstring image_path_;
    int native_rate_ = 8475;
};

//: Convert UTF-16 to the CP437 the ROM expects, mapping what it cannot
//: represent to a close ASCII equivalent (curly quotes, dashes, ellipsis)
//: rather than to '?', which the ROM would spell out loud.
[[nodiscard]] std::string to_cp437(const wchar_t* text, size_t len);

}
