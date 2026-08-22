#include <new>
#include <sapi.h>

#include "com.hpp"
#include "registry.hpp"
#include "ISpTTSEngineImpl.hpp"
#include "IEnumSpObjectTokensImpl.hpp"
#include "../common/st_log.hpp"
#include "../common/st_paths.hpp"
#include "../common/st_settings.hpp"

namespace {

HINSTANCE g_dll_handle = nullptr;
SmoothTalker::com::class_object_factory g_cls_obj_factory;

//: SAPI only ever reads token enumerators out of HKLM.  Registering the same
//: key under HKCU succeeds, looks completely correct in the registry, and is
//: then silently ignored -- so this needs an elevated regsvr32, which is what
//: the installer arranges.
const std::wstring token_enums_path =
    L"Software\\Microsoft\\Speech\\Voices\\TokenEnums";

[[nodiscard]] std::wstring clsid_to_string(const GUID& clsid)
{
    wchar_t buf[64];
    StringFromGUID2(clsid, buf, 64);
    return std::wstring(buf);
}

void register_token_enumerator()
{
    using namespace SmoothTalker::sapi;
    using namespace SmoothTalker::registry;

    const std::wstring clsid_str =
        clsid_to_string(__uuidof(IEnumSpObjectTokensImpl));

    key enums_key(HKEY_LOCAL_MACHINE, token_enums_path,
                  KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);
    key enum_key(enums_key, TOKEN_ENUM_NAME, KEY_SET_VALUE, true);

    enum_key.set(L"SmoothTalker Voices");
    enum_key.set(L"CLSID", clsid_str);

    ST_INFO("register: token enumerator HKLM\\%ls\\%ls -> %ls",
            token_enums_path.c_str(), TOKEN_ENUM_NAME, clsid_str.c_str());
}

void unregister_token_enumerator() noexcept
{
    using namespace SmoothTalker::sapi;
    using namespace SmoothTalker::registry;

    try {
        key enums_key(HKEY_LOCAL_MACHINE, token_enums_path, KEY_ALL_ACCESS);
        enums_key.delete_subkey(TOKEN_ENUM_NAME);
        ST_INFO("register: token enumerator removed");
    }
    catch (...) {
        ST_WARN("register: token enumerator was not present");
    }
}

[[nodiscard]] const wchar_t* component_name()
{
    return (sizeof(void*) == 8) ? L"sapi5_x64" : L"sapi5_x86";
}

}

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_dll_handle = hInstance;
        DisableThreadLibraryCalls(hInstance);

        st::log_open(component_name());
        // Pick the user's logging level up straight away, so a level of 0
        // really does mean a quiet log rather than "quiet after the first
        // utterance".
        st::log_set_level(st::settings_load().log_level);
        ST_INFO("dll: attached");

        try {
            g_cls_obj_factory
                .register_class<SmoothTalker::sapi::IEnumSpObjectTokensImpl>();
            g_cls_obj_factory
                .register_class<SmoothTalker::sapi::ISpTTSEngineImpl>();
        }
        catch (...) {
            ST_ERROR("dll: could not register the class factories");
            return FALSE;
        }
    } else if (dwReason == DLL_PROCESS_DETACH) {
        ST_INFO("dll: detached");
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    const HRESULT hr = g_cls_obj_factory.create(rclsid, riid, ppv);
    if (FAILED(hr)) {
        ST_WARN("dll: DllGetClassObject(%ls) -> 0x%08X",
                clsid_to_string(rclsid).c_str(), static_cast<unsigned>(hr));
    }
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return SmoothTalker::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    st::log_set_level(st::LOG_DEBUG);
    ST_INFO("register: DllRegisterServer from %ls", st::module_dir().c_str());
    try {
        SmoothTalker::com::class_registrar r(g_dll_handle);
        r.register_class<SmoothTalker::sapi::IEnumSpObjectTokensImpl>();
        r.register_class<SmoothTalker::sapi::ISpTTSEngineImpl>();
        register_token_enumerator();
        ST_INFO("register: succeeded");
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        ST_ERROR("register: out of memory");
        return E_OUTOFMEMORY;
    }
    catch (const std::exception& e) {
        ST_ERROR("register: failed: %s", e.what());
        return SELFREG_E_CLASS;
    }
    catch (...) {
        ST_ERROR("register: failed with an unexpected exception");
        return SELFREG_E_CLASS;
    }
}

STDAPI DllUnregisterServer()
{
    st::log_set_level(st::LOG_DEBUG);
    ST_INFO("register: DllUnregisterServer");
    try {
        unregister_token_enumerator();
        SmoothTalker::com::class_registrar r(g_dll_handle);
        r.unregister_class<SmoothTalker::sapi::IEnumSpObjectTokensImpl>();
        r.unregister_class<SmoothTalker::sapi::ISpTTSEngineImpl>();
        ST_INFO("register: unregistered");
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        // Unregistering something that was never registered is not a
        // failure worth reporting to the uninstaller.
        ST_WARN("register: unregister was incomplete (already gone?)");
        return S_OK;
    }
}
