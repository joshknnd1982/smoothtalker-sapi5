# SmoothTalker SAPI5

A 32-bit and 64-bit Microsoft SAPI5 text-to-speech engine built on **SmoothTalker 3.5**
by First Byte (1990) — the speech engine behind Dr. Sbaitso.

The engine is not reimplemented. `engine.bin` is a snapshot of DOS conventional
memory taken with the original SBTALKER TSR resident, and it is executed under
CPU emulation exactly as it was in 1990, down to the Sound Blaster DSP and DMA
controller it talks to. The audio you hear is the audio the Sound Blaster would
have played.

No SAPI4. No DOSBox. The engine reads nothing from the registry.

## Voices and languages

**One voice, one language: a male US English voice.**

This was measured, not assumed. The ROM's settings block has a `gender` field,
but rendering the same sentence with all ten of its values produces
byte-for-byte identical audio — the field is inert on this build. The image
says so itself, in a copyright string stored inside it:

```
SmoothTalker (R), Version 3.5, male voice
Copyright (c) 1983-1990 First Byte, All Rights Reserved
```

The phoneme table stored beside it (`AA AH AX AY AE EH AW EY AO DH DX OY ZH UH
OW IH IX IY UW TH TX SH KX PX ER NG UI`) is a US English set, and the only one
present. There is no second language and no second voice to expose.

The one axis that genuinely changes the timbre is **tone**, which cuts the low
end for a thinner, brighter sound. It is a filter on the one voice rather than
a different speaker, so it is exposed as an adjustable parameter rather than as
a second voice token.

## Engine parameters

Every parameter the ROM honours, all on its native 0–9 scale:

| Parameter | Range | Effect |
|-----------|-------|--------|
| Rate      | 0–9   | 1.91 s … 0.81 s for a fixed phrase |
| Pitch     | 0–9   | about 41 Hz … 154 Hz |
| Volume    | 0–9   | RMS 2.0 … 31.4, no clipping at the top |
| Tone      | 0–1   | 1 cuts the low end |
| ~~Gender~~ | —    | present in the struct, inert on this ROM |

SAPI's own rate, pitch and volume are applied **relative** to the user's
settings: an adjustment of 0 gives exactly what the user chose, +10 reaches the
ROM's ceiling and −10 its floor. SAPI volume additionally scales the output
digitally, because 0–100 is finer than the ROM's 0–9.

## Layout

```
src/common/    paths, logging, settings file (shared by everything)
src/engine/    the SmoothTalker ROM under Unicorn, and the unicorn loader
src/sapi/      the SAPI5 COM server (adapted from the BSTSpeech wrapper)
src/config/    the configuration utility
src/tools/     st_render, st_sapi_test, st_a11y_dump
bin/           the ROM image, both unicorn builds, and the NVDA driver this
               was derived from (kept as the reference implementation)
tools/         Python reference harnesses -- see "Verification" below
installer/     Inno Setup script and the shipped README
```

Both architectures build from identical sources. The ROM runs under Unicorn,
which is pure computation with no 32-bit-only dependency, so unlike the
BSTSpeech wrapper this needs no out-of-process bridge — the 64-bit engine
synthesizes in-process just like the 32-bit one.

`unicorn.dll` is loaded by absolute path from beside our own module rather than
through the import table. A SAPI5 engine is loaded into whatever process wants
to speak, and those processes have their own DLL search order; an import-table
dependency would be resolved against a path we do not control, and a failure
would surface as "the specified module could not be found" naming nothing.

## Building

```bash
build_all.bat
```

Requires Visual Studio 2022 (or Build Tools), CMake 3.15+, and Inno Setup 6.
It builds both architectures, runs the SAPI5 self-test on each, and produces
`output\SmoothTalkerSAPI5_Setup.exe`.

## Verification

The C++ engine is checked against the Python reference implementation it was
ported from (`bin/_smoothtalker_engine/core.py`) — same text, same parameters,
native-rate output on both sides, compared sample for sample:

```bash
python tools/compare_engines.py build_x64/bin/Release/st_render.exe
```

40 cases (4 texts × 10 parameter combinations, including one long enough to
require multi-chunk splitting) are byte-identical on both x86 and x64.

`st_sapi_test.exe` covers the SAPI5 surface in two modes. `--direct` drives
`ISpTTSEngine::Speak` through a stub site with no registry involvement at all.
`--spvoice` registers the coclasses under `HKCU\Software\Classes\CLSID` (no
elevation needed), builds a voice token the way the enumerator does, hands it
to a real SAPI `SpVoice`, and removes the temporary keys again afterwards.

`st_a11y_dump.exe` reports the MSAA view of the configuration dialog — what a
screen reader actually sees — and fails if any focusable control lacks a name
or a role.

## Two things worth knowing

**SAPI reads token enumerators only from HKLM.** Registering the identical key
under HKCU succeeds, looks completely correct in the registry, and is then
ignored without a word. Installation therefore needs elevation.

**Trackbars report their position as a percentage.** The configuration utility
originally used sliders for the 0–9 values, and MSAA announced a setting of 5
as "55" and 6 as "66" — measured with `st_a11y_dump`, not guessed. They are
drop-down lists now, which report the selected item verbatim while still
stepping with the arrow keys.

## Credits and licensing

This repository contains several things with different origins, and they are
worth keeping straight:

| Component | Origin | Terms |
|-----------|--------|-------|
| `src/`, `installer/`, `tools/`, `CMakeLists.txt` | This project | **GPLv2** — see [LICENSE](LICENSE) |
| `bin/unicorn/*/unicorn.dll` | [Unicorn Engine](https://www.unicorn-engine.org/) 2.1.4 | **GPLv2** |
| `bin/_smoothtalker_engine/engine.bin` | SmoothTalker 3.5, First Byte, 1983–1990 | Proprietary. Redistributed unmodified as abandonware, as it already is in the NVDA add-on this was derived from. Not this project's to license. |
| `bin/smoothtalker.py`, `bin/_smoothtalker_engine/core.py` | The NVDA add-on this was ported from | Kept verbatim as the reference implementation `tools/compare_engines.py` checks against |

This project is free software: you can redistribute it and/or modify it under
the terms of version 2 of the GNU General Public License as published by the
Free Software Foundation. It is distributed in the hope that it will be
useful, but **without any warranty** — without even the implied warranty of
merchantability or fitness for a particular purpose. See the
[LICENSE](LICENSE) file for the full text.

GPLv2 is the honest choice rather than a preference: the shipped installer
distributes `unicorn.dll` alongside these binaries, and Unicorn is GPLv2, so
the distribution as a whole carries GPLv2 obligations regardless — even though
unicorn is loaded dynamically at run time rather than linked.

Two provenance notes, so nobody has to guess:

- The COM plumbing (`src/sapi/com.*`, `registry.*`, `utils.hpp`,
  `ISpDataKeyImpl.*`) is **adapted from the BSTSpeech SAPI5 wrapper** and is
  not original to this project. Those files carry no per-file copyright header
  for that reason — relicensing someone else's code by fiat is not something a
  downstream adapter gets to do. If you are reusing them, go and check
  BSTSpeech's own terms.
- `engine.bin` is **not** covered by the GPL and is not this project's to
  relicense. The GPL here covers the wrapper, the emulation harness, the
  installer and the tools — not First Byte's engine.

SmoothTalker and Dr. Sbaitso are the work of First Byte and Creative Labs.
Nothing here modifies their engine; it is executed exactly as shipped in 1990.
