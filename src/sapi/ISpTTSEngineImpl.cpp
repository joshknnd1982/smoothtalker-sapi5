#include <new>
#include <string>
#include <cmath>
#include <algorithm>
#include <vector>

#include "utils.hpp"
#include "ISpTTSEngineImpl.hpp"
#include "../common/st_log.hpp"
#include "../common/st_paths.hpp"

namespace SmoothTalker {
namespace sapi {

namespace {

//: SAPI's rate and pitch adjustments are documented as -10..+10.
constexpr int SAPI_ADJ_MIN = -10;
constexpr int SAPI_ADJ_MAX = 10;

//: Feed SAPI in slices rather than whole utterances.  Write() blocks while
//: the site drains, so a slice is also how often we get to notice an abort;
//: 4096 frames is ~186 ms, small enough that stopping feels instant.
constexpr size_t FEED_FRAMES = 4096;

[[nodiscard]] int clamp_int(int v, int lo, int hi)
{
    return (std::max)(lo, (std::min)(hi, v));
}

//: Map a SAPI -10..+10 adjustment onto the ROM's 0-9 scale, pivoting on the
//: user's own setting.
//:
//: The obvious mapping -- ignore the setting and spread -10..+10 across 0..9
//: -- would make the configuration utility pointless the moment an
//: application asked for any rate at all.  Pivoting instead means adjustment
//: 0 always gives exactly what the user chose, +10 always reaches the ROM's
//: ceiling and -10 its floor, whatever the base is.
[[nodiscard]] int scale_from_base(int base, int adj, int lo = 0, int hi = 9)
{
    base = clamp_int(base, lo, hi);
    adj = clamp_int(adj, SAPI_ADJ_MIN, SAPI_ADJ_MAX);
    if (adj == 0) {
        return base;
    }
    const double span = (adj > 0) ? (hi - base) : (base - lo);
    const double value = base + span * (adj / 10.0);
    return clamp_int(static_cast<int>(std::lround(value)), lo, hi);
}

struct word_span {
    ULONG offset;  // in the source text
    ULONG length;
    size_t char_pos;  // position within the fragment, for interpolation
};

//: Word starts within a fragment, for SPEI_WORD_BOUNDARY.
[[nodiscard]] std::vector<word_span> find_words(const wchar_t* text,
                                                ULONG len, ULONG src_offset)
{
    std::vector<word_span> out;
    bool in_word = false;
    ULONG start = 0;
    for (ULONG i = 0; i <= len; ++i) {
        const bool is_word_char =
            (i < len) && (iswalnum(text[i]) || text[i] == L'\'' ||
                          text[i] == L'-');
        if (is_word_char && !in_word) {
            start = i;
            in_word = true;
        } else if (!is_word_char && in_word) {
            out.push_back({src_offset + start, i - start, start});
            in_word = false;
        }
    }
    return out;
}

//: Turn "SAPI" into "S A P I" so the ROM reads it out a letter at a time.
//: SPVA_SpellOut otherwise sounds exactly like SPVA_Speak, which is not what
//: the caller asked for.
[[nodiscard]] std::wstring spell_out(const wchar_t* text, ULONG len)
{
    std::wstring out;
    out.reserve(static_cast<size_t>(len) * 2);
    for (ULONG i = 0; i < len; ++i) {
        if (iswspace(text[i])) {
            continue;
        }
        if (!out.empty()) {
            out.push_back(L' ');
        }
        out.push_back(text[i]);
    }
    return out;
}

//: Everything one Speak() call needs to hand audio to SAPI.
class writer
{
public:
    writer(ISpTTSEngineSite* site, int volume_percent)
        : site_(site), volume_(clamp_int(volume_percent, 0, 100))
    {
    }

    [[nodiscard]] ULONGLONG bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool aborted() const noexcept { return aborted_; }

    //: Poll the site for abort/skip.  Returns false once we should stop.
    [[nodiscard]] bool still_running()
    {
        if (aborted_) {
            return false;
        }
        const DWORD actions = site_->GetActions();
        if (actions & SPVES_ABORT) {
            ST_DEBUG("speak: site asked for ABORT");
            aborted_ = true;
            return false;
        }
        if (actions & SPVES_SKIP) {
            ST_DEBUG("speak: site asked for SKIP");
            site_->CompleteSkip(0);
            aborted_ = true;
            return false;
        }
        return true;
    }

    //: Write 16-bit mono samples, applying SAPI's volume as a straight
    //: digital scale.  The ROM's own volume knob is a coarse 0-9 with a
    //: character of its own, so it stays where the user put it and SAPI's
    //: continuous 0-100 rides on top.
    bool write(const int16_t* data, size_t count)
    {
        if (!count) {
            return true;
        }
        scratch_.assign(data, data + count);
        if (volume_ != 100) {
            for (int16_t& s : scratch_) {
                s = static_cast<int16_t>(s * volume_ / 100);
            }
        }
        const BYTE* p = reinterpret_cast<const BYTE*>(scratch_.data());
        size_t remaining = scratch_.size() * sizeof(int16_t);
        while (remaining) {
            if (!still_running()) {
                return false;
            }
            const ULONG chunk = static_cast<ULONG>(
                (std::min)(remaining, FEED_FRAMES * sizeof(int16_t)));
            ULONG written = 0;
            const HRESULT hr = site_->Write(p, chunk, &written);
            if (FAILED(hr)) {
                ST_ERROR("speak: site->Write failed: 0x%08X",
                         static_cast<unsigned>(hr));
                aborted_ = true;
                return false;
            }
            if (written == 0 || written > chunk) {
                ST_ERROR("speak: site->Write returned %lu of %lu -- giving up",
                         written, chunk);
                aborted_ = true;
                return false;
            }
            bytes_ += written;
            remaining -= written;
            p += written;
        }
        return true;
    }

    bool write_silence(ULONG milliseconds)
    {
        const size_t frames =
            static_cast<size_t>(AUDIO_SAMPLE_RATE) * milliseconds / 1000;
        if (!frames) {
            return true;
        }
        const std::vector<int16_t> quiet(
            (std::min)(frames, static_cast<size_t>(AUDIO_SAMPLE_RATE)), 0);
        size_t left = frames;
        while (left) {
            const size_t n = (std::min)(left, quiet.size());
            if (!write(quiet.data(), n)) {
                return false;
            }
            left -= n;
        }
        return true;
    }

    void add_event(SPEVENTENUM id, ULONGLONG offset, WPARAM wparam,
                   LPARAM lparam,
                   SPEVENTLPARAMTYPE type = SPET_LPARAM_IS_UNDEFINED)
    {
        SPEVENT ev = {};
        // SPEVENT declares these as 16-bit enum bitfields, so they take the
        // enum values directly -- narrowing them to WORD first will not compile.
        ev.eEventId = id;
        ev.elParamType = type;
        ev.ulStreamNum = 0;
        ev.ullAudioStreamOffset = offset;
        ev.wParam = wparam;
        ev.lParam = lparam;
        const HRESULT hr = site_->AddEvents(&ev, 1);
        if (FAILED(hr)) {
            ST_WARN("speak: AddEvents(%d) failed: 0x%08X", static_cast<int>(id),
                    static_cast<unsigned>(hr));
        }
    }

private:
    ISpTTSEngineSite* site_;
    int volume_;
    ULONGLONG bytes_ = 0;
    bool aborted_ = false;
    std::vector<int16_t> scratch_;
};

}

// ---------------------------------------------------------------------------

ISpTTSEngineImpl::ISpTTSEngineImpl()
{
    ST_DEBUG("engine object: created");
}

ISpTTSEngineImpl::~ISpTTSEngineImpl()
{
    ST_DEBUG("engine object: destroyed");
}

bool ISpTTSEngineImpl::ensure_engine()
{
    if (engine_) {
        return true;
    }
    if (engine_failed_) {
        // Retried anyway -- a missing file can be put back while we are
        // loaded -- but only complain about it once.
        ST_DEBUG("engine object: retrying a previously failed engine load");
    }
    std::wstring error;
    engine_ = st::engine::create(&error);
    if (!engine_) {
        if (!engine_failed_) {
            ST_ERROR("engine object: the SmoothTalker ROM did not start: %ls",
                     error.c_str());
            engine_failed_ = true;
        }
        return false;
    }
    engine_failed_ = false;
    have_applied_ = false;
    return true;
}

STDMETHODIMP ISpTTSEngineImpl::SetObjectToken(ISpObjectToken* pToken)
{
    if (!pToken) {
        return E_INVALIDARG;
    }
    try {
        ISpDataKeyPtr attr;
        if (SUCCEEDED(pToken->OpenKey(L"Attributes", &attr))) {
            utils::out_ptr<wchar_t> name(CoTaskMemFree);
            if (SUCCEEDED(attr->GetStringValue(L"Name", name.address()))) {
                ST_INFO("engine object: bound to voice \"%ls\"", name.get());
            }
        }
        // There is only one voice, so there is nothing to select -- holding
        // the token is all this needs to do.
        token_ = pToken;
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP ISpTTSEngineImpl::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) {
        return E_POINTER;
    }
    *ppToken = nullptr;
    if (!token_) {
        return E_UNEXPECTED;
    }
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::GetOutputFormat(
    const GUID* /*pTargetFmtId*/, const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
    GUID* pOutputFormatId, WAVEFORMATEX** ppCoMemOutputWaveFormatEx)
{
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) {
        return E_POINTER;
    }
    *pOutputFormatId = SPDFID_WaveFormatEx;
    *ppCoMemOutputWaveFormatEx = nullptr;

    auto* wfex = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wfex) {
        return E_OUTOFMEMORY;
    }
    wfex->wFormatTag = WAVE_FORMAT_PCM;
    wfex->nChannels = AUDIO_CHANNELS;
    wfex->nSamplesPerSec = AUDIO_SAMPLE_RATE;
    wfex->wBitsPerSample = AUDIO_BITS_PER_SAMPLE;
    wfex->nBlockAlign =
        static_cast<WORD>(wfex->nChannels * wfex->wBitsPerSample / 8);
    wfex->nAvgBytesPerSec = wfex->nSamplesPerSec * wfex->nBlockAlign;
    wfex->cbSize = 0;

    *ppCoMemOutputWaveFormatEx = wfex;
    ST_DEBUG("engine object: output format %lu Hz, %u-bit, %u channel(s)",
             wfex->nSamplesPerSec, wfex->wBitsPerSample, wfex->nChannels);
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::Speak(DWORD dwSpeakFlags,
                                     REFGUID /*rguidFormatId*/,
                                     const WAVEFORMATEX* /*pWaveFormatEx*/,
                                     const SPVTEXTFRAG* pTextFragList,
                                     ISpTTSEngineSite* pOutputSite)
{
    if (!pTextFragList || !pOutputSite) {
        return E_INVALIDARG;
    }

    std::lock_guard<std::mutex> lock(engine_mutex_);

    try {
        const st::settings cfg = settings_.get();

        if (!ensure_engine()) {
            // Returning a failure here makes some hosts drop the voice for
            // the rest of the session.  Report success with no audio instead:
            // the reason is in the log, and the voice recovers by itself once
            // the missing file is back.
            return S_OK;
        }

        long sapi_rate = 0;
        pOutputSite->GetRate(&sapi_rate);
        USHORT sapi_volume = 100;
        pOutputSite->GetVolume(&sapi_volume);

        ULONGLONG interest = 0;
        pOutputSite->GetEventInterest(&interest);
        const bool want_sentence =
            (interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;
        const bool want_word = (interest & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
        const bool want_bookmark = (interest & SPFEI(SPEI_TTS_BOOKMARK)) != 0;

        ST_INFO("speak: flags=0x%08X rate=%ld volume=%u events=0x%llX "
                "(sentence=%d word=%d bookmark=%d) base rate=%d pitch=%d "
                "tone=%d volume=%d",
                dwSpeakFlags, sapi_rate, sapi_volume,
                static_cast<unsigned long long>(interest), want_sentence,
                want_word, want_bookmark, cfg.rate, cfg.pitch, cfg.tone,
                cfg.volume);

        writer out(pOutputSite, sapi_volume);

        // One resampler for the whole utterance: it carries its phase across
        // blocks and across fragments, so no boundary leaves a click.
        std::unique_ptr<st::resampler> rs;
        std::vector<int16_t> pcm16;
        std::vector<int16_t> resampled;

        int frag_index = 0;
        for (const SPVTEXTFRAG* frag = pTextFragList; frag; frag = frag->pNext) {
            ++frag_index;
            if (!out.still_running()) {
                break;
            }

            // Rate and volume can be changed mid-stream by the application.
            const DWORD actions = pOutputSite->GetActions();
            if (actions & SPVES_RATE) {
                pOutputSite->GetRate(&sapi_rate);
                ST_DEBUG("speak: rate changed to %ld mid-stream", sapi_rate);
            }
            if (actions & SPVES_VOLUME) {
                pOutputSite->GetVolume(&sapi_volume);
                ST_DEBUG("speak: volume changed to %u mid-stream", sapi_volume);
            }

            if (frag->State.eAction == SPVA_Bookmark) {
                std::wstring text;
                if (frag->ulTextLen && frag->pTextStart) {
                    text.assign(frag->pTextStart, frag->ulTextLen);
                }
                long id = 0;
                try {
                    id = std::stol(text);
                }
                catch (...) {
                }
                ST_DEBUG("speak: bookmark \"%ls\" (id %ld) at byte %llu",
                         text.c_str(), id, out.bytes());
                if (want_bookmark) {
                    out.add_event(SPEI_TTS_BOOKMARK, out.bytes(),
                                  static_cast<WPARAM>(id),
                                  reinterpret_cast<LPARAM>(text.c_str()),
                                  SPET_LPARAM_IS_STRING);
                }
                continue;
            }

            if (frag->State.eAction == SPVA_Silence) {
                const ULONG ms = frag->State.SilenceMSecs;
                ST_DEBUG("speak: silence %lu ms", ms);
                if (!out.write_silence(ms)) {
                    break;
                }
                continue;
            }

            if (frag->State.eAction != SPVA_Speak &&
                frag->State.eAction != SPVA_SpellOut) {
                ST_DEBUG("speak: fragment %d skipped, action %d not supported "
                         "by this ROM",
                         frag_index, static_cast<int>(frag->State.eAction));
                continue;
            }
            if (!frag->ulTextLen || !frag->pTextStart) {
                continue;
            }

            std::wstring source;
            if (frag->State.eAction == SPVA_SpellOut) {
                source = spell_out(frag->pTextStart, frag->ulTextLen);
            } else {
                source.assign(frag->pTextStart, frag->ulTextLen);
            }

            const std::string cp437 =
                st::to_cp437(source.c_str(), source.size());
            if (cp437.empty()) {
                continue;
            }

            // -- parameters for this fragment ---------------------------
            const int rate_adj =
                clamp_int(static_cast<int>(sapi_rate) + frag->State.RateAdj,
                          SAPI_ADJ_MIN, SAPI_ADJ_MAX);
            const int pitch_adj = clamp_int(frag->State.PitchAdj.MiddleAdj,
                                            SAPI_ADJ_MIN, SAPI_ADJ_MAX);

            st::engine_params want;
            want.gender = 0;
            want.tone = clamp_int(cfg.tone, 0, 1);
            want.volume = clamp_int(cfg.volume, 0, 9);
            want.speed = scale_from_base(cfg.rate, rate_adj);
            want.pitch = scale_from_base(cfg.pitch, pitch_adj);

            if (!have_applied_ || want.tone != applied_.tone ||
                want.volume != applied_.volume || want.pitch != applied_.pitch ||
                want.speed != applied_.speed) {
                ST_DEBUG("speak: configuring ROM tone=%d volume=%d pitch=%d "
                         "speed=%d (rate adj %d, pitch adj %d)",
                         want.tone, want.volume, want.pitch, want.speed,
                         rate_adj, pitch_adj);
                if (!engine_->configure(want)) {
                    ST_ERROR("speak: could not configure the ROM");
                    engine_.reset();
                    break;
                }
                applied_ = want;
                have_applied_ = true;
            }

            // SAPI's per-fragment volume is a percentage on top of the
            // stream volume.
            const int frag_volume =
                clamp_int(sapi_volume, 0, 100) *
                clamp_int(static_cast<int>(frag->State.Volume), 0, 100) / 100;

            ST_DEBUG("speak: fragment %d, %lu chars at source offset %lu, "
                     "volume %d%%",
                     frag_index, frag->ulTextLen, frag->ulTextSrcOffset,
                     frag_volume);

            if (want_sentence) {
                out.add_event(SPEI_SENTENCE_BOUNDARY, out.bytes(),
                              frag->ulTextLen, frag->ulTextSrcOffset);
            }

            const ULONGLONG frag_start = out.bytes();
            bool failed = false;

            // -- synthesis ----------------------------------------------
            // Two paths on purpose.  Streaming keeps the delay before the
            // first sound as short as the ROM allows, which is what a screen
            // reader needs.  But a word-boundary event has to name the byte
            // offset where its word is heard, and that is only knowable once
            // the fragment's total length is known -- so when a host asks for
            // word events, and only then, the fragment is rendered first and
            // the events are placed by interpolation before the audio goes
            // out.  Hosts that do not ask never pay for it.
            const st::engine::cancel_fn cancel = [&]() -> bool {
                return !out.still_running();
            };

            if (want_word) {
                std::vector<int16_t> whole;
                st::engine::block_fn collect =
                    [&](const uint8_t* data, size_t count, int rate) -> bool {
                    st::to_pcm16(data, count, pcm16);
                    if (!rs) {
                        rs = std::make_unique<st::resampler>(
                            rate, static_cast<int>(AUDIO_SAMPLE_RATE));
                    }
                    rs->feed(pcm16.data(), pcm16.size(), resampled);
                    whole.insert(whole.end(), resampled.begin(),
                                 resampled.end());
                    return true;
                };
                for (const std::string& piece :
                     st::split_for_engine(cp437)) {
                    if (!out.still_running()) {
                        failed = true;
                        break;
                    }
                    if (!engine_->speak(piece, collect, cancel)) {
                        ST_ERROR("speak: the ROM faulted; dropping the engine");
                        engine_.reset();
                        failed = true;
                        break;
                    }
                }
                if (!failed) {
                    const ULONGLONG total_bytes =
                        static_cast<ULONGLONG>(whole.size()) * sizeof(int16_t);
                    const std::vector<word_span> words = find_words(
                        source.c_str(), static_cast<ULONG>(source.size()),
                        frag->ulTextSrcOffset);
                    for (const word_span& w : words) {
                        const ULONGLONG at =
                            frag_start +
                            (source.empty()
                                 ? 0
                                 : total_bytes * w.char_pos / source.size()) /
                                sizeof(int16_t) * sizeof(int16_t);
                        out.add_event(SPEI_WORD_BOUNDARY, at, w.length,
                                      w.offset);
                    }
                    if (!out.write(whole.data(), whole.size())) {
                        failed = true;
                    }
                }
            } else {
                st::engine::block_fn stream =
                    [&](const uint8_t* data, size_t count, int rate) -> bool {
                    st::to_pcm16(data, count, pcm16);
                    if (!rs) {
                        rs = std::make_unique<st::resampler>(
                            rate, static_cast<int>(AUDIO_SAMPLE_RATE));
                    }
                    rs->feed(pcm16.data(), pcm16.size(), resampled);
                    return out.write(resampled.data(), resampled.size());
                };
                for (const std::string& piece :
                     st::split_for_engine(cp437)) {
                    if (!out.still_running()) {
                        failed = true;
                        break;
                    }
                    if (!engine_->speak(piece, stream, cancel)) {
                        ST_ERROR("speak: the ROM faulted; dropping the engine");
                        engine_.reset();
                        failed = true;
                        break;
                    }
                }
            }

            if (failed || out.aborted()) {
                break;
            }
        }

        ST_INFO("speak: finished, %llu bytes written%s", out.bytes(),
                out.aborted() ? " (stopped early)" : "");
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        ST_ERROR("speak: out of memory");
        return E_OUTOFMEMORY;
    }
    catch (...) {
        ST_ERROR("speak: unexpected exception");
        return E_UNEXPECTED;
    }
}

}
}
