#pragma once

#include <string>

namespace SmoothTalker {
namespace sapi {

//: SmoothTalker 3.5 has exactly one voice, in one language.
//:
//: This is not an assumption -- it was measured.  The ROM's settings struct
//: has a `gender` field, but rendering the same text with all ten values
//: gives byte-identical audio, and the image's own copyright block says so
//: outright:
//:
//:     SmoothTalker (R), Version 3.5, male voice
//:     Copyright (c) 1983-1990 First Byte, All Rights Reserved
//:
//: The phoneme table embedded next to it (AA AH AX AY AE EH AW EY AO DH DX
//: OY ZH UH OW IH IX IY UW TH TX SH KX PX ER NG UI) is a US English set and
//: the only one present, so there is no second language hiding either.
//:
//: The other axis a listener might call a "voice" is `tone`, which really
//: does change the timbre -- it cuts the low end for a thinner, brighter
//: sound.  It is exposed as an adjustable parameter through the
//: configuration utility rather than as a second voice token, because it is
//: a filter on the one voice rather than a different speaker.
inline constexpr int st_voice_count = 1;

class voice_attributes
{
public:
    explicit voice_attributes(int voice_index = 0) noexcept : index_(0)
    {
        (void)voice_index;
    }

    //: The Name attribute, and how SetObjectToken finds us again.
    [[nodiscard]] std::wstring get_name() const { return L"SmoothTalker"; }

    //: What a voice list shows the user.
    [[nodiscard]] std::wstring get_description() const
    {
        return L"SmoothTalker 3.5 (First Byte)";
    }

    [[nodiscard]] int get_index() const noexcept { return index_; }

    [[nodiscard]] std::wstring get_age() const { return L"Adult"; }

    [[nodiscard]] std::wstring get_gender() const { return L"Male"; }

    //: 409 = LANG_ENGLISH / SUBLANG_ENGLISH_US, hexadecimal, as SAPI wants it.
    [[nodiscard]] std::wstring get_language() const { return L"409"; }

    [[nodiscard]] std::wstring get_vendor() const { return L"First Byte"; }

    [[nodiscard]] std::wstring get_version() const { return L"3.5"; }

private:
    int index_;
};

}
}
