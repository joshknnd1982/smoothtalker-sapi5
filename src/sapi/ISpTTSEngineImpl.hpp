#pragma once

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <comdef.h>
#include <comip.h>

#include <memory>
#include <mutex>

#include "com.hpp"
#include "voice_attributes.hpp"
#include "../common/st_settings.hpp"
#include "../engine/st_engine.hpp"

namespace SmoothTalker {
namespace sapi {

//: What we hand SAPI.  The ROM runs at 8475 Hz, which plenty of audio paths
//: refuse outright, so everything is resampled once to a rate nothing
//: objects to.  22050 was chosen over 11025 because the resampling ratio
//: matters less when you are going up.
inline constexpr WORD AUDIO_CHANNELS = 1;
inline constexpr DWORD AUDIO_SAMPLE_RATE = 22050;
inline constexpr WORD AUDIO_BITS_PER_SAMPLE = 16;

class __declspec(uuid("f8ad08b4-fe63-4162-ac77-865f290388c1")) ISpTTSEngineImpl
    : public ISpTTSEngine, public ISpObjectWithToken
{
public:
    ISpTTSEngineImpl();
    ~ISpTTSEngineImpl();

    ISpTTSEngineImpl(const ISpTTSEngineImpl&) = delete;
    ISpTTSEngineImpl& operator=(const ISpTTSEngineImpl&) = delete;

    STDMETHOD(Speak)(DWORD dwSpeakFlags, REFGUID rguidFormatId,
                     const WAVEFORMATEX* pWaveFormatEx,
                     const SPVTEXTFRAG* pTextFragList,
                     ISpTTSEngineSite* pOutputSite) override;
    STDMETHOD(GetOutputFormat)(const GUID* pTargetFmtId,
                               const WAVEFORMATEX* pTargetWaveFormatEx,
                               GUID* pOutputFormatId,
                               WAVEFORMATEX** ppCoMemOutputWaveFormatEx)
        override;

    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        void* ptr = com::try_primary_interface<ISpTTSEngine>(this, riid);
        return ptr ? ptr : com::try_interface<ISpObjectWithToken>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpDataKey, __uuidof(ISpDataKey));

    //: Bring the ROM up if it is not already.  Caller holds engine_mutex_.
    [[nodiscard]] bool ensure_engine();

    ISpObjectTokenPtr token_;

    //: SAPI serialises Speak() per object, but GetOutputFormat and
    //: SetObjectToken can arrive from elsewhere, and nothing in the contract
    //: promises one thread throughout.  One lock over the ROM keeps it
    //: honest; the ROM is a single 16-bit machine and cannot be re-entered.
    std::mutex engine_mutex_;
    std::unique_ptr<st::engine> engine_;
    bool engine_failed_ = false;

    //: Last parameters actually pushed to the ROM, so an utterance that
    //: changes nothing does not pay for a re-init call.
    st::engine_params applied_{};
    bool have_applied_ = false;

    //: Re-reads the settings file when the configuration utility touches it,
    //: which is what makes a slider move audible on the very next utterance.
    st::settings_watcher settings_;
};

}
}
