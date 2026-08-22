#include "unicorn_dyn.hpp"

#include "../common/st_log.hpp"
#include "../common/st_paths.hpp"

#include <mutex>
#include <vector>

namespace st {

namespace {

unicorn_api g_api;
HMODULE g_dll = nullptr;
std::wstring g_error;
std::once_flag g_once;

[[nodiscard]] std::string narrow(const std::wstring& s)
{
    if (s.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

//: PE machine type of `path`, or 0 if it cannot be determined.  ctypes-style
//: "%1 is not a valid Win32 application" tells the user nothing about which
//: file was wrong or how; reading the header ourselves lets the log say
//: "unicorn.dll is 64-bit but this process is 32-bit" in so many words.
[[nodiscard]] WORD pe_machine(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    BYTE head[0x400] = {0};
    DWORD got = 0;
    const BOOL ok = ReadFile(h, head, sizeof(head), &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 0x40) {
        return 0;
    }
    const DWORD off = *reinterpret_cast<const DWORD*>(head + 0x3C);
    if (off + 6 > got || head[off] != 'P' || head[off + 1] != 'E') {
        return 0;
    }
    return *reinterpret_cast<const WORD*>(head + off + 4);
}

[[nodiscard]] const char* machine_name(WORD m)
{
    switch (m) {
    case 0x014C: return "32-bit (x86)";
    case 0x8664: return "64-bit (x64)";
    case 0xAA64: return "ARM64";
    default:     return "unknown";
    }
}

template <typename T>
bool bind(HMODULE dll, T& fn, const char* name)
{
    fn = reinterpret_cast<T>(
        reinterpret_cast<void*>(GetProcAddress(dll, name)));
    if (!fn) {
        ST_ERROR("unicorn: unicorn.dll is missing the export '%s'", name);
        return false;
    }
    return true;
}

//: Where unicorn.dll may live, most specific first.  The installer puts it
//: beside each architecture's SAPI DLL; the extra candidates cover a
//: development tree and a hand-assembled folder.
[[nodiscard]] std::vector<std::wstring> candidates()
{
    const std::wstring dir = module_dir();
    const wchar_t* arch = (sizeof(void*) == 8) ? L"x64\\" : L"x86\\";
    return {
        dir + L"unicorn.dll",
        dir + L"unicorn\\" + arch + L"unicorn.dll",
        dir + L"..\\unicorn\\" + arch + L"unicorn.dll",
        dir + L"..\\unicorn.dll",
    };
}

void do_load()
{
    const WORD want = (sizeof(void*) == 8) ? 0x8664 : 0x014C;

    for (const std::wstring& path : candidates()) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ST_DEBUG("unicorn: not present: %ls", path.c_str());
            continue;
        }
        const WORD machine = pe_machine(path);
        if (machine != want) {
            ST_ERROR("unicorn: %ls is %s but this process is %s -- skipping",
                     path.c_str(), machine_name(machine), machine_name(want));
            continue;
        }
        // LOAD_WITH_ALTERED_SEARCH_PATH makes unicorn.dll's own directory the
        // first place its dependencies are looked for, rather than the host
        // process's directory.
        HMODULE dll = LoadLibraryExW(path.c_str(), nullptr,
                                     LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) {
            const DWORD err = GetLastError();
            ST_ERROR("unicorn: LoadLibrary(%ls) failed: %s", path.c_str(),
                     win32_error_text(err).c_str());
            continue;
        }

        unicorn_api api;
        bool ok = true;
        ok &= bind(dll, api.open, "uc_open");
        ok &= bind(dll, api.close, "uc_close");
        ok &= bind(dll, api.mem_map, "uc_mem_map");
        ok &= bind(dll, api.mem_write, "uc_mem_write");
        ok &= bind(dll, api.mem_read, "uc_mem_read");
        ok &= bind(dll, api.reg_write, "uc_reg_write");
        ok &= bind(dll, api.reg_read, "uc_reg_read");
        ok &= bind(dll, api.emu_start, "uc_emu_start");
        ok &= bind(dll, api.emu_stop, "uc_emu_stop");
        ok &= bind(dll, api.hook_add, "uc_hook_add");
        ok &= bind(dll, api.strerror, "uc_strerror");
        ok &= bind(dll, api.version, "uc_version");
        if (!ok) {
            FreeLibrary(dll);
            continue;
        }

        unsigned int major = 0;
        unsigned int minor = 0;
        api.version(&major, &minor);
        ST_INFO("unicorn: loaded %ls (%s, version %u.%u)", path.c_str(),
                machine_name(machine), major, minor);

        g_dll = dll;
        g_api = api;
        g_error.clear();
        return;
    }

    g_error = L"unicorn.dll could not be loaded. Looked in:";
    for (const std::wstring& path : candidates()) {
        g_error += L"\n  " + path;
    }
    ST_ERROR("unicorn: %s", narrow(g_error).c_str());
}

}

const unicorn_api* unicorn_load()
{
    std::call_once(g_once, do_load);
    return g_api.ok() ? &g_api : nullptr;
}

std::wstring unicorn_last_error()
{
    return g_error;
}

}
