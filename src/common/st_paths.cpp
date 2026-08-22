#include "st_paths.hpp"

#include <shlobj.h>

namespace st {

namespace {

//: Address inside this module, for GetModuleHandleEx below.
void anchor() {}

[[nodiscard]] std::wstring with_slash(std::wstring s)
{
    if (!s.empty() && s.back() != L'\\') {
        s.push_back(L'\\');
    }
    return s;
}

[[nodiscard]] std::wstring shell_folder(int csidl)
{
    wchar_t buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl | CSIDL_FLAG_CREATE,
                                   nullptr, SHGFP_TYPE_CURRENT, buf))) {
        return with_slash(buf);
    }
    // Last resort: the temp directory always exists and is always writable.
    wchar_t tmp[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, tmp) > 0) {
        return with_slash(tmp);
    }
    return L".\\";
}

}

std::wstring module_dir()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &self)) {
        return L".\\";
    }
    wchar_t buf[MAX_PATH + 1] = {0};
    const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0) {
        return L".\\";
    }
    buf[n] = L'\0';
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (!slash) {
        return L".\\";
    }
    slash[1] = L'\0';
    return std::wstring(buf);
}

bool ensure_dir(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }
    // SHCreateDirectoryExW builds the whole chain in one call.
    const int rc = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS ||
        rc == ERROR_FILE_EXISTS) {
        return true;
    }
    const DWORD again = GetFileAttributesW(path.c_str());
    return again != INVALID_FILE_ATTRIBUTES &&
           (again & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring settings_dir()
{
    const std::wstring dir = shell_folder(CSIDL_APPDATA) + L"SmoothTalker\\";
    ensure_dir(dir);
    return dir;
}

std::wstring log_dir()
{
    const std::wstring dir =
        shell_folder(CSIDL_LOCAL_APPDATA) + L"SmoothTalker\\Logs\\";
    ensure_dir(dir);
    return dir;
}

std::wstring settings_path()
{
    return settings_dir() + L"settings.ini";
}

std::wstring process_name()
{
    wchar_t buf[MAX_PATH + 1] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) {
        return L"?";
    }
    buf[n] = L'\0';
    const wchar_t* slash = wcsrchr(buf, L'\\');
    return slash ? std::wstring(slash + 1) : std::wstring(buf);
}

}
