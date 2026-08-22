//: st_render -- render text with the SmoothTalker ROM to a .wav file.
//:
//: This exists to prove the C++ engine is the same engine as the Python
//: reference in bin/_smoothtalker_engine/core.py: run both over the same text
//: and parameters and the PCM must match sample for sample.  It is also the
//: quickest way to hear a change without going through SAPI at all.

#include "../common/st_log.hpp"
#include "../common/st_paths.hpp"
#include "../engine/st_engine.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct options {
    std::wstring text = L"Hello, this is Smooth Talker speaking.";
    std::wstring out = L"st_render.wav";
    std::wstring image;
    st::engine_params params;
    int out_rate = 22050;
    bool quiet = false;
};

void usage()
{
    std::fwprintf(
        stderr,
        L"st_render -- SmoothTalker 3.5 renderer\n"
        L"\n"
        L"  --text \"...\"      text to speak\n"
        L"  --text-file PATH   read the text from a UTF-8 file\n"
        L"  --out PATH         output .wav (default st_render.wav)\n"
        L"  --image PATH       engine.bin (default: next to this exe)\n"
        L"  --rate N           0-9, engine speed      (default 5)\n"
        L"  --pitch N          0-9                    (default 5)\n"
        L"  --tone N           0-1                    (default 0)\n"
        L"  --volume N         0-9                    (default 5)\n"
        L"  --gender N         0-9, inert on this ROM (default 0)\n"
        L"  --out-rate HZ      output rate, 0 = engine native (default 22050)\n"
        L"  --quiet            no progress on stdout\n");
}

[[nodiscard]] std::wstring read_text_file(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"cannot open %ls\n", path.c_str());
        return {};
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    std::string raw(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr);
    CloseHandle(h);
    raw.resize(got);
    // Skip a UTF-8 BOM if one is present.
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(),
                                      static_cast<int>(raw.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), static_cast<int>(raw.size()),
                        out.data(), n);
    return out;
}

bool write_wav(const std::wstring& path, const std::vector<int16_t>& pcm,
               int rate)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"cannot create %ls\n", path.c_str());
        return false;
    }
#pragma pack(push, 1)
    struct wav_header {
        char riff[4];
        uint32_t riff_size;
        char wave[4];
        char fmt[4];
        uint32_t fmt_size;
        uint16_t format;
        uint16_t channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits;
        char data[4];
        uint32_t data_size;
    };
#pragma pack(pop)
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    wav_header hdr = {};
    std::memcpy(hdr.riff, "RIFF", 4);
    hdr.riff_size = 36 + bytes;
    std::memcpy(hdr.wave, "WAVE", 4);
    std::memcpy(hdr.fmt, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.format = 1;
    hdr.channels = 1;
    hdr.sample_rate = static_cast<uint32_t>(rate);
    hdr.bits = 16;
    hdr.block_align = 2;
    hdr.byte_rate = hdr.sample_rate * hdr.block_align;
    std::memcpy(hdr.data, "data", 4);
    hdr.data_size = bytes;

    DWORD written = 0;
    bool ok = WriteFile(h, &hdr, sizeof(hdr), &written, nullptr) != 0;
    if (ok && bytes) {
        ok = WriteFile(h, pcm.data(), bytes, &written, nullptr) != 0;
    }
    CloseHandle(h);
    return ok;
}

[[nodiscard]] int to_int(const wchar_t* s, int fallback)
{
    wchar_t* end = nullptr;
    const long v = wcstol(s, &end, 10);
    return (end && *end == L'\0') ? static_cast<int>(v) : fallback;
}

}

int wmain(int argc, wchar_t** argv)
{
    options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool has_next = (i + 1 < argc);
        const wchar_t* next = has_next ? argv[i + 1] : L"";
        if (a == L"--help" || a == L"-h") {
            usage();
            return 0;
        } else if (a == L"--quiet") {
            o.quiet = true;
        } else if (!has_next) {
            std::fwprintf(stderr, L"%ls needs a value\n", a.c_str());
            return 2;
        } else if (a == L"--text") {
            o.text = next;
            ++i;
        } else if (a == L"--text-file") {
            o.text = read_text_file(next);
            ++i;
        } else if (a == L"--out") {
            o.out = next;
            ++i;
        } else if (a == L"--image") {
            o.image = next;
            ++i;
        } else if (a == L"--rate") {
            o.params.speed = to_int(next, 5);
            ++i;
        } else if (a == L"--pitch") {
            o.params.pitch = to_int(next, 5);
            ++i;
        } else if (a == L"--tone") {
            o.params.tone = to_int(next, 0);
            ++i;
        } else if (a == L"--volume") {
            o.params.volume = to_int(next, 5);
            ++i;
        } else if (a == L"--gender") {
            o.params.gender = to_int(next, 0);
            ++i;
        } else if (a == L"--out-rate") {
            o.out_rate = to_int(next, 22050);
            ++i;
        } else {
            std::fwprintf(stderr, L"unknown option %ls\n", a.c_str());
            usage();
            return 2;
        }
    }

    st::log_open(sizeof(void*) == 8 ? L"st_render_x64" : L"st_render_x86");
    st::log_set_level(st::LOG_DEBUG);

    std::wstring error;
    std::unique_ptr<st::engine> eng =
        o.image.empty() ? st::engine::create(&error)
                        : st::engine::create_from(o.image, &error);
    if (!eng) {
        std::fwprintf(stderr, L"engine did not start: %ls\n", error.c_str());
        return 1;
    }

    if (!eng->configure(o.params)) {
        std::fwprintf(stderr, L"could not apply engine parameters\n");
        return 1;
    }

    const std::string cp437 = st::to_cp437(o.text.c_str(), o.text.size());
    std::vector<int16_t> pcm;
    if (!eng->speak_all(cp437, o.out_rate, pcm, {})) {
        std::fwprintf(stderr, L"synthesis failed -- see the log in %ls\n",
                      st::log_dir().c_str());
        return 1;
    }

    const int rate = (o.out_rate > 0) ? o.out_rate : eng->native_rate();
    if (!write_wav(o.out, pcm, rate)) {
        return 1;
    }
    if (!o.quiet) {
        std::wprintf(L"%ls  %zu samples  %.3f s  %d Hz  "
                     L"(tone %d volume %d pitch %d speed %d)\n",
                     o.out.c_str(), pcm.size(),
                     rate ? static_cast<double>(pcm.size()) / rate : 0.0, rate,
                     o.params.tone, o.params.volume, o.params.pitch,
                     o.params.speed);
    }
    return 0;
}
