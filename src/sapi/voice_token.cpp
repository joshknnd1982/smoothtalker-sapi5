#include <new>
#include <comdef.h>
#include "voice_token.hpp"
#include "ISpTTSEngineImpl.hpp"

namespace SmoothTalker {
namespace sapi {

voice_token::voice_token(const voice_attributes& attr)
{
    // The token's default value is what a voice list shows the user; the
    // Name attribute is what SetObjectToken matches on.  They are allowed to
    // differ and here they do, so the list reads well without making the
    // lookup key wordy.
    set(attr.get_description());

    utils::out_ptr<wchar_t> clsid_str(CoTaskMemFree);
    StringFromCLSID(__uuidof(ISpTTSEngineImpl), clsid_str.address());
    set(L"CLSID", clsid_str.get());

    attributes_[L"Name"] = attr.get_name();
    attributes_[L"Age"] = attr.get_age();
    attributes_[L"Gender"] = attr.get_gender();
    attributes_[L"Language"] = attr.get_language();
    attributes_[L"Vendor"] = attr.get_vendor();
    attributes_[L"Version"] = attr.get_version();
}

STDMETHODIMP voice_token::OpenKey(LPCWSTR pszSubKeyName, ISpDataKey** ppSubKey)
{
    if (!pszSubKeyName) {
        return E_INVALIDARG;
    }
    if (!ppSubKey) {
        return E_POINTER;
    }
    *ppSubKey = nullptr;

    try {
        if (!str_equal(pszSubKeyName, L"Attributes")) {
            return SPERR_NOT_FOUND;
        }

        com::object<ISpDataKeyImpl> obj;
        for (const auto& entry : attributes_) {
            obj->set(entry.first, entry.second);
        }

        com::interface_ptr<ISpDataKey> int_ptr(obj);
        *ppSubKey = int_ptr.get();
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP voice_token::EnumKeys(ULONG Index, LPWSTR* ppszSubKeyName)
{
    if (!ppszSubKeyName) {
        return E_POINTER;
    }
    *ppszSubKeyName = nullptr;

    if (Index > 0) {
        return SPERR_NO_MORE_ITEMS;
    }

    try {
        *ppszSubKeyName = com::strdup(L"Attributes");
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

}
}
