//: SmoothTalker Configuration -- rate, pitch, tone and volume for the
//: SmoothTalker 3.5 SAPI5 voice.
//:
//: Settings go to %APPDATA%\SmoothTalker\settings.ini, not the registry, and
//: the SAPI engines re-read the file whenever its timestamp moves.  That is
//: what "takes effect immediately" means here: move a slider and the very
//: next thing any SAPI5 application speaks uses the new value -- including
//: the screen reader announcing the slider you just moved, if it is set to
//: this voice.

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"
#include "../common/st_log.hpp"
#include "../common/st_paths.hpp"
#include "../common/st_settings.hpp"
#include "../engine/st_engine.hpp"

#pragma comment(linker,                                                        \
                "/manifestdependency:\"type='win32' "                          \
                "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "  \
                "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                "language='*'\"")

namespace {

//: Saves are coalesced through a short timer rather than written on every
//: notification: holding an arrow key down to run a value from 0 to 9 sends
//: ten CBN_SELCHANGEs and there is no reason to rewrite the file ten times.
//: 120 ms is below the threshold where a change stops feeling instant, and a
//: single keypress still results in exactly one write.
constexpr UINT SAVE_TIMER_ID = 1;
constexpr UINT SAVE_DELAY_MS = 120;

constexpr const wchar_t* TEST_PHRASE =
    L"This is Smooth Talker. The quick brown fox jumps over the lazy dog.";

st::settings g_settings;
std::atomic<bool> g_speaking{false};
HWND g_dialog = nullptr;

void set_status(HWND dlg, const std::wstring& text)
{
    SetDlgItemTextW(dlg, IDC_STATUS, text.c_str());
}

//: Fill a 0-9 drop-down list.  The item text is the bare number, because
//: that text is exactly what a screen reader announces as the control's
//: value -- which is the whole reason these are lists and not trackbars.
void setup_chooser(HWND dlg, int id, int value)
{
    HWND h = GetDlgItem(dlg, id);
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (int v = st::settings::VALUE_MIN; v <= st::settings::VALUE_MAX; ++v) {
        wchar_t buf[8];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", v);
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(value), 0);
}

[[nodiscard]] int chooser_value(HWND dlg, int id)
{
    const LRESULT sel = SendMessageW(GetDlgItem(dlg, id), CB_GETCURSEL, 0, 0);
    // The list is filled with VALUE_MIN..VALUE_MAX in order, so the index is
    // the value.
    return (sel == CB_ERR) ? st::settings::RATE_DEFAULT : static_cast<int>(sel);
}

void load_into_controls(HWND dlg, const st::settings& s)
{
    SendMessageW(GetDlgItem(dlg, IDC_RATE), CB_SETCURSEL,
                 static_cast<WPARAM>(s.rate), 0);
    SendMessageW(GetDlgItem(dlg, IDC_PITCH), CB_SETCURSEL,
                 static_cast<WPARAM>(s.pitch), 0);
    SendMessageW(GetDlgItem(dlg, IDC_VOLUME), CB_SETCURSEL,
                 static_cast<WPARAM>(s.volume), 0);
    CheckDlgButton(dlg, IDC_TONE, s.tone ? BST_CHECKED : BST_UNCHECKED);
}

void collect_from_controls(HWND dlg, st::settings& s)
{
    s.rate = chooser_value(dlg, IDC_RATE);
    s.pitch = chooser_value(dlg, IDC_PITCH);
    s.volume = chooser_value(dlg, IDC_VOLUME);
    s.tone = (IsDlgButtonChecked(dlg, IDC_TONE) == BST_CHECKED) ? 1 : 0;
    s.clamp();
}

void save_now(HWND dlg)
{
    KillTimer(dlg, SAVE_TIMER_ID);
    collect_from_controls(dlg, g_settings);
    if (st::settings_save(g_settings)) {
        set_status(dlg, L"Saved. Rate " + std::to_wstring(g_settings.rate) +
                            L", pitch " + std::to_wstring(g_settings.pitch) +
                            L", volume " + std::to_wstring(g_settings.volume) +
                            (g_settings.tone ? L", bright tone."
                                             : L", full tone."));
    } else {
        set_status(dlg, L"Could not write " + st::settings_path());
    }
}

void schedule_save(HWND dlg)
{
    SetTimer(dlg, SAVE_TIMER_ID, SAVE_DELAY_MS, nullptr);
}

//: Build a RIFF/WAVE image in memory so PlaySound can play it without a
//: temporary file -- one less thing to fail on a locked-down machine, and
//: nothing left behind.
[[nodiscard]] std::vector<BYTE> make_wav(const std::vector<int16_t>& pcm,
                                         int rate)
{
#pragma pack(push, 1)
    struct header {
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
    header h = {};
    memcpy(h.riff, "RIFF", 4);
    h.riff_size = 36 + bytes;
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);
    h.fmt_size = 16;
    h.format = 1;
    h.channels = 1;
    h.sample_rate = static_cast<uint32_t>(rate);
    h.bits = 16;
    h.block_align = 2;
    h.byte_rate = h.sample_rate * h.block_align;
    memcpy(h.data, "data", 4);
    h.data_size = bytes;

    std::vector<BYTE> out(sizeof(h) + bytes);
    memcpy(out.data(), &h, sizeof(h));
    if (bytes) {
        memcpy(out.data() + sizeof(h), pcm.data(), bytes);
    }
    return out;
}

//: Synthesis takes a couple of hundred milliseconds and playback takes
//: seconds; neither belongs on the UI thread, where it would freeze the
//: dialog and, with it, the screen reader's view of it.
void speak_test(st::settings s)
{
    std::wstring error;
    std::unique_ptr<st::engine> eng = st::engine::create(&error);
    if (!eng) {
        if (g_dialog) {
            PostMessageW(g_dialog, WM_APP + 1, 0,
                         reinterpret_cast<LPARAM>(new std::wstring(
                             L"The SmoothTalker engine did not start: " +
                             error)));
        }
        g_speaking = false;
        return;
    }

    st::engine_params p;
    p.tone = s.tone;
    p.volume = s.volume;
    p.pitch = s.pitch;
    p.speed = s.rate;
    eng->configure(p);

    std::vector<int16_t> pcm;
    const std::string cp437 = st::to_cp437(TEST_PHRASE, wcslen(TEST_PHRASE));
    const int rate = 22050;
    if (eng->speak_all(cp437, rate, pcm, {}) && !pcm.empty()) {
        const std::vector<BYTE> wav = make_wav(pcm, rate);
        // SND_SYNC keeps the buffer alive for the whole of playback simply by
        // not returning until it is over; this is a worker thread, so nothing
        // is blocked that matters.
        PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr,
                   SND_MEMORY | SND_SYNC | SND_NODEFAULT);
    } else if (g_dialog) {
        PostMessageW(g_dialog, WM_APP + 1, 0,
                     reinterpret_cast<LPARAM>(new std::wstring(
                         L"The test phrase could not be synthesized. See the "
                         L"log in " + st::log_dir())));
    }
    g_speaking = false;
    if (g_dialog) {
        PostMessageW(g_dialog, WM_APP + 2, 0, 0);
    }
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        g_dialog = dlg;
        g_settings = st::settings_load();
        setup_chooser(dlg, IDC_RATE, g_settings.rate);
        setup_chooser(dlg, IDC_PITCH, g_settings.pitch);
        setup_chooser(dlg, IDC_VOLUME, g_settings.volume);
        load_into_controls(dlg, g_settings);
        set_status(dlg, L"Settings file: " + st::settings_path());
        ST_INFO("config: opened, rate=%d pitch=%d volume=%d tone=%d",
                g_settings.rate, g_settings.pitch, g_settings.volume,
                g_settings.tone);
        return TRUE;
    }

    case WM_TIMER:
        if (wparam == SAVE_TIMER_ID) {
            save_now(dlg);
            return TRUE;
        }
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_RATE:
        case IDC_PITCH:
        case IDC_VOLUME:
            // Both keyboard stepping and picking from the open list arrive
            // here, so one case covers every way of changing a value.
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                schedule_save(dlg);
            }
            return TRUE;

        case IDC_TONE:
            save_now(dlg);
            return TRUE;

        case IDC_DEFAULTS: {
            st::settings d;
            d.reset();
            d.log_level = g_settings.log_level;  // not a user-facing setting
            g_settings = d;
            load_into_controls(dlg, g_settings);
            save_now(dlg);
            set_status(dlg, L"Restored the default settings: rate 5, pitch 5, "
                            L"volume 5, full tone.");
            SetFocus(GetDlgItem(dlg, IDC_RATE));
            return TRUE;
        }

        case IDC_TEST: {
            if (g_speaking.exchange(true)) {
                return TRUE;  // already speaking
            }
            save_now(dlg);
            set_status(dlg, L"Speaking the test phrase...");
            std::thread(speak_test, g_settings).detach();
            return TRUE;
        }

        case IDOK:
        case IDCANCEL:
            save_now(dlg);
            ST_INFO("config: closing");
            EndDialog(dlg, 0);
            return TRUE;

        default:
            return FALSE;
        }

    case WM_APP + 1: {
        std::wstring* message = reinterpret_cast<std::wstring*>(lparam);
        if (message) {
            set_status(dlg, *message);
            MessageBoxW(dlg, message->c_str(), L"SmoothTalker Configuration",
                        MB_OK | MB_ICONWARNING);
            delete message;
        }
        return TRUE;
    }

    case WM_APP + 2:
        // The Speak button is deliberately never disabled -- taking focus off
        // a control the user is standing on is far more disruptive than
        // ignoring a second press, which g_speaking already does.
        set_status(dlg, L"Settings file: " + st::settings_path());
        return TRUE;

    case WM_CLOSE:
        save_now(dlg);
        EndDialog(dlg, 0);
        return TRUE;

    default:
        return FALSE;
    }
}

}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    st::log_open(L"config");
    st::log_set_level(st::settings_load().log_level);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    if (!InitCommonControlsEx(&icc)) {
        ST_WARN("config: InitCommonControlsEx failed: %s",
                st::win32_error_text(GetLastError()).c_str());
    }

    const INT_PTR rc = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_CONFIG),
                                       nullptr, dialog_proc, 0);
    if (rc == -1) {
        ST_ERROR("config: could not create the dialog: %s",
                 st::win32_error_text(GetLastError()).c_str());
        MessageBoxW(nullptr, L"The configuration window could not be opened.",
                    L"SmoothTalker Configuration", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
