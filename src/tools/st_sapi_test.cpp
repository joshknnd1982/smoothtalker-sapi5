//: st_sapi_test -- exercise the SmoothTalker SAPI5 engine end to end.
//:
//: Two modes, neither of which needs an elevated regsvr32:
//:
//:   --direct   Load the SAPI DLL by hand, ask it for a class object, drive
//:              ISpTTSEngine::Speak through a stub site and write the audio
//:              to a .wav.  Touches no registry at all, so it can run on a
//:              machine where nothing is installed.
//:
//:   --spvoice  The real thing: register the two coclasses under
//:              HKCU\Software\Classes\CLSID (per-user, no elevation), build
//:              a voice token exactly the way the enumerator does, hand it to
//:              a genuine SAPI SpVoice and let SAPI call us.  The HKCU keys
//:              are removed again before the tool exits.
//:
//: The HKLM half of a real installation -- the TokenEnums key that makes the
//: voice appear in every application's voice list -- is deliberately not
//: touched here; that belongs to the installer.

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <comdef.h>
#include <comip.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Must match src/sapi/IEnumSpObjectTokensImpl.hpp and ISpTTSEngineImpl.hpp.
// clang-format off
const CLSID CLSID_STEnum   = {0x196ac3ad, 0xaa2f, 0x4dc7, {0xa6, 0x64, 0x43, 0x43, 0x4b, 0x5c, 0xa3, 0xb3}};
const CLSID CLSID_STEngine = {0xf8ad08b4, 0xfe63, 0x4162, {0xac, 0x77, 0x86, 0x5f, 0x29, 0x03, 0x88, 0xc1}};
// clang-format on

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

[[nodiscard]] std::wstring guid_string(const GUID& g)
{
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return buf;
}

// -- a stub ISpTTSEngineSite -------------------------------------------------

class stub_site : public ISpTTSEngineSite
{
public:
    stub_site(long rate, USHORT volume, ULONGLONG interest)
        : rate_(rate), volume_(volume), interest_(interest)
    {
    }

    std::vector<BYTE> audio;
    std::vector<SPEVENT> events;

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ISpEventSink)) ||
            IsEqualIID(riid, __uuidof(ISpTTSEngineSite))) {
            *ppv = static_cast<ISpTTSEngineSite*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHOD_(ULONG, AddRef)() override { return ++refs_; }

    STDMETHOD_(ULONG, Release)() override
    {
        // Stack allocated on purpose: never deletes itself.
        return --refs_;
    }

    STDMETHOD(AddEvents)(const SPEVENT* ev, ULONG count) override
    {
        for (ULONG i = 0; i < count; ++i) {
            events.push_back(ev[i]);
        }
        return S_OK;
    }

    STDMETHOD(GetEventInterest)(ULONGLONG* out) override
    {
        *out = interest_;
        return S_OK;
    }

    STDMETHOD_(DWORD, GetActions)() override { return 0; }

    STDMETHOD(Write)(const void* buf, ULONG cb, ULONG* written) override
    {
        const BYTE* p = static_cast<const BYTE*>(buf);
        audio.insert(audio.end(), p, p + cb);
        if (written) {
            *written = cb;
        }
        return S_OK;
    }

    STDMETHOD(GetRate)(long* out) override
    {
        *out = rate_;
        return S_OK;
    }

    STDMETHOD(GetVolume)(USHORT* out) override
    {
        *out = volume_;
        return S_OK;
    }

    STDMETHOD(GetSkipInfo)(SPVSKIPTYPE* type, long* items) override
    {
        if (type) {
            *type = SPVST_SENTENCE;
        }
        if (items) {
            *items = 0;
        }
        return S_OK;
    }

    STDMETHOD(CompleteSkip)(long) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
    long rate_;
    USHORT volume_;
    ULONGLONG interest_;
};

bool write_wav(const std::wstring& path, const void* pcm, size_t bytes,
               int rate)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
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
    header hd = {};
    std::memcpy(hd.riff, "RIFF", 4);
    hd.riff_size = static_cast<uint32_t>(36 + bytes);
    std::memcpy(hd.wave, "WAVE", 4);
    std::memcpy(hd.fmt, "fmt ", 4);
    hd.fmt_size = 16;
    hd.format = 1;
    hd.channels = 1;
    hd.sample_rate = static_cast<uint32_t>(rate);
    hd.bits = 16;
    hd.block_align = 2;
    hd.byte_rate = hd.sample_rate * hd.block_align;
    std::memcpy(hd.data, "data", 4);
    hd.data_size = static_cast<uint32_t>(bytes);
    DWORD n = 0;
    WriteFile(h, &hd, sizeof(hd), &n, nullptr);
    if (bytes) {
        WriteFile(h, pcm, static_cast<DWORD>(bytes), &n, nullptr);
    }
    CloseHandle(h);
    return true;
}

// -- mode 1: drive the engine directly ---------------------------------------

void run_direct(HMODULE dll, const std::wstring& out_path,
                const std::wstring& text)
{
    std::printf("\n-- direct: ISpTTSEngine without any registration --\n");

    auto get_class = reinterpret_cast<DllGetClassObjectFn>(
        reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
    check(get_class != nullptr, "DllGetClassObject is exported");
    if (!get_class) {
        return;
    }

    IClassFactory* factory = nullptr;
    HRESULT hr = get_class(CLSID_STEngine, IID_IClassFactory,
                           reinterpret_cast<void**>(&factory));
    check(SUCCEEDED(hr) && factory, "class object for the TTS engine");
    if (FAILED(hr) || !factory) {
        return;
    }

    ISpTTSEngine* engine = nullptr;
    hr = factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine),
                                 reinterpret_cast<void**>(&engine));
    factory->Release();
    check(SUCCEEDED(hr) && engine, "CreateInstance(ISpTTSEngine)");
    if (FAILED(hr) || !engine) {
        return;
    }

    // The engine must also answer for ISpObjectWithToken.
    ISpObjectWithToken* with_token = nullptr;
    hr = engine->QueryInterface(__uuidof(ISpObjectWithToken),
                                reinterpret_cast<void**>(&with_token));
    check(SUCCEEDED(hr) && with_token, "QueryInterface(ISpObjectWithToken)");
    if (with_token) {
        with_token->Release();
    }

    GUID fmt_id = {};
    WAVEFORMATEX* wfex = nullptr;
    hr = engine->GetOutputFormat(nullptr, nullptr, &fmt_id, &wfex);
    check(SUCCEEDED(hr) && wfex, "GetOutputFormat");
    int rate = 22050;
    if (wfex) {
        std::printf("        %lu Hz, %u-bit, %u channel(s)\n",
                    wfex->nSamplesPerSec, wfex->wBitsPerSample,
                    wfex->nChannels);
        check(wfex->wFormatTag == WAVE_FORMAT_PCM && wfex->nChannels == 1 &&
                  wfex->wBitsPerSample == 16,
              "output format is 16-bit mono PCM");
        rate = static_cast<int>(wfex->nSamplesPerSec);
        CoTaskMemFree(wfex);
    }

    // A three-fragment utterance: a bookmark, some speech, and a spell-out,
    // so the fragment walk gets exercised rather than just the easy path.
    std::wstring bookmark = L"42";
    std::wstring spell = L"SAPI";

    SPVTEXTFRAG frag_spell = {};
    frag_spell.pNext = nullptr;
    frag_spell.State.eAction = SPVA_SpellOut;
    frag_spell.State.Volume = 100;
    frag_spell.pTextStart = spell.c_str();
    frag_spell.ulTextLen = static_cast<ULONG>(spell.size());
    frag_spell.ulTextSrcOffset = 100;

    SPVTEXTFRAG frag_text = {};
    frag_text.pNext = &frag_spell;
    frag_text.State.eAction = SPVA_Speak;
    frag_text.State.Volume = 100;
    frag_text.pTextStart = text.c_str();
    frag_text.ulTextLen = static_cast<ULONG>(text.size());
    frag_text.ulTextSrcOffset = 0;

    SPVTEXTFRAG frag_bookmark = {};
    frag_bookmark.pNext = &frag_text;
    frag_bookmark.State.eAction = SPVA_Bookmark;
    frag_bookmark.State.Volume = 100;
    frag_bookmark.pTextStart = bookmark.c_str();
    frag_bookmark.ulTextLen = static_cast<ULONG>(bookmark.size());

    stub_site site(0, 100,
                   SPFEI(SPEI_TTS_BOOKMARK) | SPFEI(SPEI_WORD_BOUNDARY) |
                       SPFEI(SPEI_SENTENCE_BOUNDARY));

    hr = engine->Speak(0, GUID_NULL, nullptr, &frag_bookmark, &site);
    check(SUCCEEDED(hr), "Speak returned success");
    check(!site.audio.empty(), "Speak produced audio");
    std::printf("        %zu bytes of audio (%.2f s), %zu events\n",
                site.audio.size(),
                site.audio.size() / 2.0 / rate, site.events.size());

    int bookmarks = 0;
    int words = 0;
    int sentences = 0;
    for (const SPEVENT& e : site.events) {
        if (e.eEventId == SPEI_TTS_BOOKMARK) {
            ++bookmarks;
        } else if (e.eEventId == SPEI_WORD_BOUNDARY) {
            ++words;
        } else if (e.eEventId == SPEI_SENTENCE_BOUNDARY) {
            ++sentences;
        }
    }
    std::printf("        %d bookmark, %d sentence, %d word events\n", bookmarks,
                sentences, words);
    check(bookmarks == 1, "the bookmark fragment produced one event");
    check(sentences == 2, "each spoken fragment produced a sentence event");
    check(words > 0, "word boundary events were produced");

    // Word events must be in ascending audio order, or a host that highlights
    // text will jump about.
    bool ordered = true;
    ULONGLONG last = 0;
    for (const SPEVENT& e : site.events) {
        if (e.eEventId == SPEI_WORD_BOUNDARY) {
            if (e.ullAudioStreamOffset < last) {
                ordered = false;
            }
            last = e.ullAudioStreamOffset;
        }
    }
    check(ordered, "word events are in ascending stream order");

    // Rate and volume have to actually do something.
    stub_site fast(10, 100, 0);
    SPVTEXTFRAG solo = frag_text;
    solo.pNext = nullptr;
    engine->Speak(0, GUID_NULL, nullptr, &solo, &fast);
    stub_site slow(-10, 100, 0);
    engine->Speak(0, GUID_NULL, nullptr, &solo, &slow);
    std::printf("        rate +10 -> %zu bytes, rate -10 -> %zu bytes\n",
                fast.audio.size(), slow.audio.size());
    check(fast.audio.size() < slow.audio.size(),
          "SAPI rate +10 is faster than -10");

    stub_site quiet(0, 25, 0);
    engine->Speak(0, GUID_NULL, nullptr, &solo, &quiet);
    double loud_rms = 0;
    double quiet_rms = 0;
    const auto rms = [](const std::vector<BYTE>& a) {
        const int16_t* s = reinterpret_cast<const int16_t*>(a.data());
        const size_t n = a.size() / 2;
        double acc = 0;
        for (size_t i = 0; i < n; ++i) {
            acc += double(s[i]) * s[i];
        }
        return n ? std::sqrt(acc / n) : 0.0;
    };
    loud_rms = rms(fast.audio);
    quiet_rms = rms(quiet.audio);
    std::printf("        volume 100 rms %.0f, volume 25 rms %.0f\n", loud_rms,
                quiet_rms);
    check(quiet_rms < loud_rms * 0.6, "SAPI volume 25 is quieter than 100");

    if (!site.audio.empty()) {
        write_wav(out_path, site.audio.data(), site.audio.size(), rate);
        std::wprintf(L"        wrote %ls\n", out_path.c_str());
    }
    engine->Release();
}

// -- mode 2: through a real SAPI SpVoice -------------------------------------

bool register_hkcu(const std::wstring& dll_path, const CLSID& clsid)
{
    const std::wstring path =
        L"Software\\Classes\\CLSID\\" + guid_string(clsid) + L"\\InProcServer32";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    RegSetValueExW(key, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(dll_path.c_str()),
                   static_cast<DWORD>((dll_path.size() + 1) * sizeof(wchar_t)));
    const wchar_t* both = L"Both";
    RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(both), 5 * sizeof(wchar_t));
    RegCloseKey(key);
    return true;
}

void unregister_hkcu(const CLSID& clsid)
{
    const std::wstring base = L"Software\\Classes\\CLSID\\" + guid_string(clsid);
    RegDeleteKeyW(HKEY_CURRENT_USER, (base + L"\\InProcServer32").c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, base.c_str());
}

void run_spvoice(HMODULE dll, const std::wstring& dll_path,
                 const std::wstring& out_path, const std::wstring& text)
{
    std::printf("\n-- spvoice: through a real SAPI ISpVoice --\n");

    check(register_hkcu(dll_path, CLSID_STEngine),
          "registered the engine coclass under HKCU");
    check(register_hkcu(dll_path, CLSID_STEnum),
          "registered the enumerator coclass under HKCU");

    {
        // Build the voice token the same way the enumerator does, then hand
        // it to SAPI.  This is the path a real application takes once the
        // installer has added the HKLM TokenEnums key.
        auto get_class = reinterpret_cast<DllGetClassObjectFn>(
            reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
        IClassFactory* factory = nullptr;
        HRESULT hr = get_class(CLSID_STEnum, IID_IClassFactory,
                               reinterpret_cast<void**>(&factory));
        check(SUCCEEDED(hr) && factory, "class object for the enumerator");
        if (FAILED(hr) || !factory) {
            return;
        }

        IEnumSpObjectTokens* tokens = nullptr;
        hr = factory->CreateInstance(nullptr, __uuidof(IEnumSpObjectTokens),
                                     reinterpret_cast<void**>(&tokens));
        factory->Release();
        check(SUCCEEDED(hr) && tokens, "CreateInstance(IEnumSpObjectTokens)");
        if (FAILED(hr) || !tokens) {
            return;
        }

        ULONG count = 0;
        tokens->GetCount(&count);
        std::printf("        enumerator reports %lu voice(s)\n", count);
        check(count == 1, "exactly one voice is enumerated");

        ISpObjectToken* token = nullptr;
        hr = tokens->Item(0, &token);
        check(SUCCEEDED(hr) && token, "Item(0) returned a token");

        if (token) {
            // SpGetDescription() lives in sphelper.h, which drags in ATL;
            // the token's unnamed value is the same string.
            LPWSTR desc = nullptr;
            if (SUCCEEDED(token->GetStringValue(nullptr, &desc)) && desc) {
                std::wprintf(L"        voice description: \"%ls\"\n", desc);
                CoTaskMemFree(desc);
            }
            ISpDataKey* attrs = nullptr;
            if (SUCCEEDED(token->OpenKey(L"Attributes", &attrs)) && attrs) {
                for (const wchar_t* name :
                     {L"Name", L"Gender", L"Age", L"Language", L"Vendor",
                      L"Version"}) {
                    LPWSTR value = nullptr;
                    if (SUCCEEDED(attrs->GetStringValue(name, &value)) &&
                        value) {
                        std::wprintf(L"          %-9ls = %ls\n", name, value);
                        CoTaskMemFree(value);
                    } else {
                        std::wprintf(L"          %-9ls = (missing)\n", name);
                        ++g_failures;
                    }
                }
                attrs->Release();
            } else {
                check(false, "the token exposes an Attributes key");
            }

            // Now the real test: SAPI drives our engine.
            ISpVoice* voice = nullptr;
            hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                  __uuidof(ISpVoice),
                                  reinterpret_cast<void**>(&voice));
            check(SUCCEEDED(hr) && voice, "created a SAPI SpVoice");
            if (SUCCEEDED(hr) && voice) {
                hr = voice->SetVoice(token);
                check(SUCCEEDED(hr), "SpVoice::SetVoice accepted our token");

                // The sphelper.h wrapper for this is SPBindToFile(); doing it
                // by hand keeps ATL out of the build.
                ISpStream* stream = nullptr;
                WAVEFORMATEX wfex = {};
                wfex.wFormatTag = WAVE_FORMAT_PCM;
                wfex.nChannels = 1;
                wfex.nSamplesPerSec = 22050;
                wfex.wBitsPerSample = 16;
                wfex.nBlockAlign = 2;
                wfex.nAvgBytesPerSec = 44100;
                hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL,
                                      __uuidof(ISpStream),
                                      reinterpret_cast<void**>(&stream));
                if (SUCCEEDED(hr) && stream) {
                    hr = stream->BindToFile(out_path.c_str(),
                                            SPFM_CREATE_ALWAYS,
                                            &SPDFID_WaveFormatEx, &wfex, 0);
                }
                check(SUCCEEDED(hr) && stream, "bound an output .wav stream");
                if (SUCCEEDED(hr) && stream) {
                    voice->SetOutput(stream, TRUE);
                    hr = voice->Speak(text.c_str(), SPF_DEFAULT, nullptr);
                    check(SUCCEEDED(hr), "SpVoice::Speak succeeded");
                    voice->WaitUntilDone(30000);
                    stream->Close();
                    stream->Release();

                    WIN32_FILE_ATTRIBUTE_DATA fad = {};
                    GetFileAttributesExW(out_path.c_str(),
                                         GetFileExInfoStandard, &fad);
                    std::wprintf(L"        wrote %ls (%lu bytes)\n",
                                 out_path.c_str(), fad.nFileSizeLow);
                    check(fad.nFileSizeLow > 10000,
                          "SAPI wrote a plausible amount of audio");
                }
                voice->Release();
            }
            token->Release();
        }
        tokens->Release();
    }

    unregister_hkcu(CLSID_STEngine);
    unregister_hkcu(CLSID_STEnum);
    std::printf("  [ ok ] removed the temporary HKCU registration\n");
}

}

int wmain(int argc, wchar_t** argv)
{
    std::wstring dll_path;
    std::wstring text = L"Smooth Talker is now speaking through sappy five.";
    std::wstring prefix = L"sapi_test";
    bool do_direct = true;
    bool do_spvoice = true;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--direct") {
            do_spvoice = false;
        } else if (a == L"--spvoice") {
            do_direct = false;
        } else if (a == L"--dll" && i + 1 < argc) {
            dll_path = argv[++i];
        } else if (a == L"--text" && i + 1 < argc) {
            text = argv[++i];
        } else if (a == L"--prefix" && i + 1 < argc) {
            prefix = argv[++i];
        }
    }

    // Default to the DLL beside this executable rather than one resolved
    // against the working directory: the build drops both in the same folder,
    // and a test that silently picks up whatever happens to be in the current
    // directory is worse than one that fails.
    if (dll_path.empty()) {
        wchar_t self[MAX_PATH] = {0};
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (n) {
            self[n] = L'\0';
            wchar_t* slash = wcsrchr(self, L'\\');
            if (slash) {
                slash[1] = L'\0';
            }
            dll_path = std::wstring(self) + L"SmoothTalkerSAPI.dll";
        } else {
            dll_path = L"SmoothTalkerSAPI.dll";
        }
    }

    // An absolute path, because the DLL locates engine.bin and unicorn.dll
    // relative to itself.
    wchar_t full[MAX_PATH] = {0};
    if (GetFullPathNameW(dll_path.c_str(), MAX_PATH, full, nullptr)) {
        dll_path = full;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::printf("CoInitializeEx failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }

    std::wprintf(L"SmoothTalker SAPI5 test\n  dll: %ls\n", dll_path.c_str());
    HMODULE dll = LoadLibraryExW(dll_path.c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
    check(dll != nullptr, "the SAPI DLL loaded");
    if (!dll) {
        std::printf("  LoadLibrary failed: %lu\n", GetLastError());
        CoUninitialize();
        return 1;
    }

    if (do_direct) {
        run_direct(dll, prefix + L"_direct.wav", text);
    }
    if (do_spvoice) {
        run_spvoice(dll, dll_path, prefix + L"_spvoice.wav", text);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    CoUninitialize();
    return g_failures ? 1 : 0;
}
