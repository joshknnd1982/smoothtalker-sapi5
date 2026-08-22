SmoothTalker SAPI5
==================

A Microsoft SAPI5 text-to-speech voice built on the original SmoothTalker 3.5
speech engine by First Byte (1990) -- the engine behind Dr. Sbaitso.

The original engine was a 16-bit DOS program that talked to a Sound Blaster.
It is not reimplemented or resynthesized here: the actual 1990 code is run
under CPU emulation, and the audio you hear is the same audio the Sound
Blaster would have played. Nothing goes through SAPI4, and the engine reads
nothing from the Windows registry.


Voices and languages
--------------------

There is one voice: "SmoothTalker 3.5 (First Byte)", a male US English voice.

That is the whole set, and it was checked rather than assumed. The engine's
settings block has a "gender" field, but rendering the same sentence with all
ten of its values produces byte-for-byte identical audio -- the field does
nothing on this build. The engine image says as much itself, in a copyright
string stored inside it:

    SmoothTalker (R), Version 3.5, male voice
    Copyright (c) 1983-1990 First Byte, All Rights Reserved

The pronunciation table stored beside it is a US English phoneme set, and the
only one present, so there is no second language either.

Everything the voice needs -- its vocabulary, pronunciation rules, and the
voice itself -- is inside the single file engine.bin. There are no separate
voice or language files to install.


What gets installed
-------------------

  SmoothTalkerSAPI.dll      the 32-bit SAPI5 engine
  unicorn.dll               the CPU emulator it runs the ROM on (32-bit)
  engine.bin                the SmoothTalker 3.5 ROM image
  SmoothTalkerConfig.exe    the configuration utility
  st_render.exe             command-line renderer (see below)
  x64\                      the same set, 64-bit, on 64-bit Windows

Both engines are registered with Windows, so 32-bit and 64-bit applications
alike will find the voice.


The configuration utility
-------------------------

"SmoothTalker Configuration", on the desktop and in the Start menu, adjusts
the four things the engine can be told:

  Rate    0-9   0 is slowest, 9 is fastest
  Pitch   0-9   0 is about 41 Hz, 9 is about 154 Hz
  Volume  0-9   0 is quietest, 9 is loudest
  Tone    on/off  "bright" cuts the low end for a thinner voice

Changes take effect immediately -- there is nothing to restart and no OK
button to press. The settings are saved to

    %APPDATA%\SmoothTalker\settings.ini

and every SAPI5 engine re-reads that file whenever it changes, so the next
thing anything speaks uses the new value. If your screen reader is set to
this voice, you will hear each change as you make it.

Applications that set their own rate, pitch or volume through SAPI still work:
those adjustments are applied relative to your settings here, so an adjustment
of zero always gives you exactly what you chose.


Logs
----

Everything writes a detailed log to

    %LOCALAPPDATA%\SmoothTalker\Logs\

    sapi5_x86.log    the 32-bit SAPI5 engine
    sapi5_x64.log    the 64-bit SAPI5 engine
    config.log       the configuration utility

The installer keeps its own transcript separately, in the installation folder
as install.log, because Setup runs elevated and would otherwise write it into
the administrator's profile rather than yours.

Each log names the application that loaded the engine, so it is clear which
program a given line came from. Logs are capped at 4 MB and one previous
generation is kept as a .1 file.

For more detail than the default, set LogLevel in settings.ini to 5; for less,
set it to 1 for errors only, or 0 to turn logging off.


st_render.exe
-------------

A command-line renderer, useful for telling apart "the engine is broken" and
"SAPI is broken":

    st_render.exe --text "Hello there" --out hello.wav
    st_render.exe --text "Hello" --rate 9 --pitch 2 --tone 1 --out fast.wav
    st_render.exe --help

It talks to the ROM directly and does not involve SAPI at all.


Uninstalling
------------

Uninstall from Settings, or from the Start menu group. Your settings.ini and
your logs are deliberately left in place -- reinstalling then keeps your voice
exactly as you had it. To remove them too, delete these folders yourself:

    %APPDATA%\SmoothTalker
    %LOCALAPPDATA%\SmoothTalker


Credits and licensing
---------------------

SmoothTalker and Dr. Sbaitso are the work of First Byte and Creative Labs.
This package contains their engine image (engine.bin) unmodified; it is
proprietary and is not covered by the licence below.

CPU emulation is by the Unicorn Engine (https://www.unicorn-engine.org/),
version 2.1.4, which is GPLv2.

The SAPI5 wrapper, the configuration utility, the installer and the tools are
free software under version 2 of the GNU General Public License. They come
with ABSOLUTELY NO WARRANTY. You are free to redistribute them under the terms
of that licence; the full text is in the LICENSE file in the source
repository:

    https://github.com/joshknnd1982/smoothtalker-sapi5

Source code for everything in this package that is under the GPL is available
at that address.
