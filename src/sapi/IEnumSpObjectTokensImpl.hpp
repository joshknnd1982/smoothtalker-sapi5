#pragma once

#include <vector>
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <comdef.h>
#include <comip.h>

#include "com.hpp"
#include "voice_attributes.hpp"
#include "voice_token.hpp"

namespace SmoothTalker {
namespace sapi {

//: The name this enumerator is registered under in
//: HKLM\Software\Microsoft\Speech\Voices\TokenEnums.  SAPI only consults
//: HKLM for token enumerators -- an HKCU registration is accepted without
//: complaint and then simply never read -- so registration needs elevation
//: and the installer is where it happens.
inline constexpr const wchar_t* TOKEN_ENUM_NAME = L"SmoothTalker";

class __declspec(uuid("196ac3ad-aa2f-4dc7-a664-43434b5ca3b3"))
    IEnumSpObjectTokensImpl : public IEnumSpObjectTokens
{
public:
    explicit IEnumSpObjectTokensImpl(bool initialize = true);

    IEnumSpObjectTokensImpl(const IEnumSpObjectTokensImpl&) = delete;
    IEnumSpObjectTokensImpl& operator=(const IEnumSpObjectTokensImpl&) = delete;

    STDMETHOD(Next)(ULONG celt, ISpObjectToken** pelt,
                    ULONG* pceltFetched) override;
    STDMETHOD(Skip)(ULONG celt) override;
    STDMETHOD(Reset)() override;
    STDMETHOD(Clone)(IEnumSpObjectTokens** ppEnum) override;
    STDMETHOD(Item)(ULONG Index, ISpObjectToken** ppToken) override;
    STDMETHOD(GetCount)(ULONG* pulCount) override;

protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        return com::try_primary_interface<IEnumSpObjectTokens>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpObjectTokenInit, __uuidof(ISpObjectTokenInit));

    [[nodiscard]] ISpObjectTokenPtr create_token(
        const voice_attributes& attr) const;

    std::size_t index_;
    std::vector<voice_attributes> sapi_voices_;
};

}
}
