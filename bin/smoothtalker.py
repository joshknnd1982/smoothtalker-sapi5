# -*- coding: utf-8 -*-
"""
NVDA synth driver for the Dr. Sbaitso / First Byte SmoothTalker engine.

Synthesis is done by emulating the original 1990 16-bit DOS speech engine
with Unicorn (see _smoothtalker_engine/core.py); the resulting Sound Blaster
PCM is converted to 16-bit, resampled, and fed to nvwave.

Utterances are synthesized on a worker thread -- roughly 15x faster than
real time, ~25 ms for a short phrase -- so speech starts promptly.
"""

import builtins
import os
import queue
import struct
import sys
import threading

_HERE = os.path.dirname(__file__)
#: The engine package name starts with an underscore on purpose: NVDA scans
#: synthDrivers/ for synthesizers and skips names beginning with "_".  Without
#: it, NVDA tries to load the engine package as a driver and logs
#: "module ... has no attribute 'SynthDriver'".
_ENGINE_DIR = os.path.join(_HERE, '_smoothtalker_engine')
_LIB = os.path.join(_ENGINE_DIR, 'lib')                 # bundled unicorn
#: Appended rather than inserted at the front: neither name here collides with
#: anything NVDA ships, and jumping the queue on sys.path would let this
#: add-on shadow modules for every *other* add-on in the process.
for _path in (_LIB, _HERE):
    if _path not in sys.path:
        sys.path.append(_path)

import config
import nvwave
import synthDriverHandler
from autoSettingsUtils.driverSetting import NumericDriverSetting
from logHandler import log
from speech.commands import IndexCommand
from synthDriverHandler import VoiceInfo, synthDoneSpeaking, synthIndexReached

#: NVDA installs gettext's _ as a builtin; fall back when run standalone.
_ = getattr(builtins, '_', lambda s: s)

#: The engine runs at 8475 Hz, which not every output path accepts, so the
#: audio is resampled to a conventional rate before it reaches nvwave.
OUT_RATE = 22050

IMAGE = os.path.join(_ENGINE_DIR, 'engine.bin')
UNICORN_DLL = os.path.join(_LIB, 'unicorn', 'lib', 'unicorn.dll')


#: The engine's settings are 0-9 with 5 as its own default, so NVDA's sliders
#: are declared over that range directly rather than rescaled from 0-100.
#: There is then no mapping to get wrong, and the settings ring steps by one
#: engine unit (normalStep) instead of NVDA's usual 5.
ENGINE_MIN = 0
ENGINE_MAX = 9
ENGINE_DEFAULT = 5
#: Tone is the one two-position setting; mirrors core.DEFAULT_TONE, which
#: cannot be read here because the engine is imported lazily (see _getCore).
TONE_DEFAULT = 0

#: Set on first use by _getCore().
_core = None


def _getCore():
    """
    Import the emulation core -- and through it unicorn -- on demand.

    Importing it at module scope meant a unicorn that would not load took the
    whole driver module down with it: NVDA logs "Error while importing
    SynthDriver" and the synthesizer is simply absent from the list, with no
    hint as to why, until NVDA is restarted.  Importing here keeps this module
    itself always loadable, so check() can name the real problem and a later
    attempt can still succeed.
    """
    global _core
    if _core is None:
        from _smoothtalker_engine import core
        _core = core
    return _core


#: PE machine types, from winnt.h.
_PE_MACHINE = {0x014C: '32-bit (x86)', 0x8664: '64-bit (x64)',
               0xAA64: 'ARM64'}


def _dllArchProblem():
    """
    Say why unicorn.dll cannot possibly load into this process, or None.

    ctypes reports an architecture mismatch as "%1 is not a valid Win32
    application", which names neither the file nor the mismatch.  NVDA has
    been 64-bit since 2026.1, so a 32-bit unicorn.dll left behind by an older
    build of this add-on is a real possibility and worth saying out loud.
    """
    want = 0x8664 if struct.calcsize('P') == 8 else 0x014C
    try:
        with open(UNICORN_DLL, 'rb') as f:
            head = f.read(0x400)
        offset = struct.unpack_from('<I', head, 0x3C)[0]
        if head[offset:offset + 4] != b'PE\0\0':
            return '%s is not a PE image' % UNICORN_DLL
        machine = struct.unpack_from('<H', head, offset + 4)[0]
    except Exception:
        return '%s could not be read' % UNICORN_DLL
    if machine != want:
        return ('%s is %s but NVDA is %s'
                % (UNICORN_DLL,
                   _PE_MACHINE.get(machine, '0x%04X' % machine),
                   _PE_MACHINE[want]))
    return None


def _clamp(value, lo=ENGINE_MIN, hi=ENGINE_MAX, default=ENGINE_DEFAULT):
    try:
        value = int(value)
    except (TypeError, ValueError):
        return default
    return max(lo, min(hi, value))


def _splitText(text, limit=None):
    """
    Break text into pieces the engine can accept in one call.

    The engine takes at most 255 characters (a one-byte length field), so
    anything longer has to be spoken as consecutive utterances.  Prosody
    restarts at each boundary, so split at the strongest break available --
    sentence end, then clause, then word -- and only cut mid-word as a last
    resort.  Splitting at arbitrary points is what made speech sound like it
    had spurious commas in 0.1.0.
    """
    if limit is None:
        limit = _getCore().MAX_TEXT
    text = text.strip()
    if not text:
        return []
    out = []
    while len(text) > limit:
        window = text[:limit + 1]
        cut = -1
        for seps in ('.!?', ',;:'):
            best = -1
            for sep in seps:
                # require the punctuation to be followed by a space, so
                # decimals and abbreviations aren't treated as sentence ends
                idx = window.rfind(sep + ' ')
                if idx > best:
                    best = idx
            if best > limit // 4:              # ignore uselessly early breaks
                cut = best + 1
                break
        if cut < 0:
            cut = window.rfind(' ')
        if cut <= 0:
            cut = limit                        # one enormous word
        out.append(text[:cut].strip())
        text = text[cut:].strip()
    if text:
        out.append(text)
    return [c for c in out if c]


def _setting(sid, label, ringLabel, minVal=ENGINE_MIN, maxVal=ENGINE_MAX,
             defaultVal=ENGINE_DEFAULT, largeStep=3):
    return NumericDriverSetting(
        sid, label,
        availableInSettingsRing=True,
        defaultVal=defaultVal,
        minVal=minVal,
        maxVal=maxVal,
        minStep=1,
        normalStep=1,          # settings ring step
        largeStep=largeStep,   # pageUp/pageDown in the Voice settings dialog
        displayName=ringLabel,
    )


class SynthDriver(synthDriverHandler.SynthDriver):
    #: Must match this module's filename -- NVDA imports synthDrivers.<name>.
    #: Renamed from "sbaitso" in 0.6.0 so settings land in a fresh config
    #: section; the old one holds 0-100 values that fail validation against
    #: the current 0-9 range.
    name = 'smoothtalker'
    #: Shown in NVDA's synthesizer selection list.
    description = 'smooth talker'

    #: Order here is the order shown in Voice settings and stepped through in
    #: the settings ring.
    supportedSettings = (
        _setting('rate', _('&Rate'), _('Rate')),
        _setting('pitch', _('&Pitch'), _('Pitch')),
        # "Tone" is First Byte's own name for this: the engine's settings
        # struct is (gender, tone, volume, pitch, speed, startpos, action),
        # and Dr. Sbaitso's PARAM command documents it as Tone(0/1).  It is a
        # slider like the rest, just with only two positions.
        _setting('tone', _('&Tone'), _('Tone'),
                 minVal=0, maxVal=1, defaultVal=TONE_DEFAULT, largeStep=1),
        _setting('volume', _('&Volume'), _('Volume')),
    )
    supportedCommands = {IndexCommand}
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}

    @classmethod
    def check(cls):
        if not os.path.isfile(IMAGE):
            log.warning('smoothtalker: engine image missing at %s' % IMAGE)
            return False
        problem = _dllArchProblem()
        if problem:
            log.warning('smoothtalker: %s' % problem)
            return False
        try:
            _getCore()
        except Exception:
            log.warning('smoothtalker: the bundled unicorn engine did not load',
                        exc_info=True)
            return False
        return True

    def __init__(self):
        super().__init__()
        # Fail here only for what is permanently broken.  Everything that can
        # be transiently unavailable -- the audio device above all -- is left
        # to the worker thread, because anything raising out of this
        # constructor makes NVDA drop the synthesizer and fall back to
        # another one for the rest of the session.
        _getCore()
        self._engine = None
        self._engineFailed = False
        self._player = None
        self._playerFailed = False
        self._playerLock = threading.Lock()
        self._queue = queue.Queue()
        # Utterances carry a generation number.  cancel() bumps the current
        # generation, which permanently invalidates everything already queued
        # or in flight.  A shared "cancelled" flag cannot work here: the next
        # speak() would have to clear it, reviving an utterance that was still
        # being rendered -- which is exactly how stale audio leaked through.
        self._gen = 0
        self._genLock = threading.Lock()
        self._stopping = threading.Event()
        # Engine settings live in a five-word block at buffer+0x200 and are
        # applied by an AL=2 call.  That touches the shared emulator, so it
        # must happen on the worker thread -- setters only record the value
        # and raise a dirty flag.
        self._paramLock = threading.Lock()
        self._rate = ENGINE_DEFAULT
        self._pitch = ENGINE_DEFAULT
        self._volume = ENGINE_DEFAULT
        self._tone = TONE_DEFAULT
        self._paramsDirty = True
        self._thread = threading.Thread(target=self._worker,
                                        name='smoothtalker synth')
        self._thread.daemon = True
        self._thread.start()

    # -- audio output ------------------------------------------------------
    def _getPlayer(self):
        """
        Open the audio output on first use, retrying after a failure.

        WavePlayer's constructor opens the output device and raises if that
        fails.  Building it in __init__ meant a device that was not ready yet
        -- routine in the first seconds after NVDA starts, especially with USB
        or Bluetooth audio -- made the whole driver fail to construct, so NVDA
        logged "setSynth failed" and quietly fell back to another synthesizer.
        That is the intermittent "it didn't come up this time" at startup.
        Opening here, on the worker thread, costs one utterance instead.
        """
        with self._playerLock:
            if self._player is not None:
                return self._player
            try:
                self._player = nvwave.WavePlayer(
                    channels=1, samplesPerSec=OUT_RATE, bitsPerSample=16,
                    outputDevice=config.conf['audio']['outputDevice'])
            except Exception:
                # Retried on every utterance, so say it properly once and stay
                # quiet after that rather than filling the log while a device
                # is missing.
                if self._playerFailed:
                    log.debugWarning('smoothtalker: audio output still '
                                     'unavailable', exc_info=True)
                else:
                    log.error('smoothtalker: could not open the audio output',
                              exc_info=True)
                    self._playerFailed = True
            else:
                self._playerFailed = False
            return self._player

    def _dropPlayer(self):
        """Discard the player so the next utterance opens a fresh one."""
        with self._playerLock:
            player, self._player = self._player, None
        if player is not None:
            try:
                player.close()
            except Exception:
                log.debugWarning('smoothtalker: player.close failed',
                                 exc_info=True)

    def _getEngine(self):
        """Load the engine image on first use.  Worker thread only."""
        if self._engine is None:
            try:
                self._engine = _getCore().Engine(IMAGE)
            except Exception:
                if self._engineFailed:
                    log.debugWarning('smoothtalker: engine image still '
                                     'unusable', exc_info=True)
                else:
                    log.error('smoothtalker: could not load the engine image',
                              exc_info=True)
                    self._engineFailed = True
        return self._engine

    # -- NVDA interface ----------------------------------------------------
    def speak(self, speechSequence):
        """
        Render the whole sequence as ONE utterance.

        Splitting it at index markers and synthesizing each run separately
        restarts the engine's prosody at every boundary, which is audible as
        a spurious pause -- "Start toggle button not pressed" came out as
        "Start, toggle button, not pressed".
        """
        parts, indexes = [], []
        for item in speechSequence:
            if isinstance(item, str):
                parts.append(item)
            elif isinstance(item, IndexCommand):
                indexes.append(item.index)
        text = ' '.join(p.strip() for p in parts if p and p.strip())
        text = ' '.join(text.split())
        if text or indexes:
            # Tag with the generation current at queue time.  speak() must
            # NOT reset anything -- NVDA queues follow-on speech without
            # cancelling, and resetting here would clobber that.
            with self._genLock:
                gen = self._gen
            self._queue.put((gen, text, indexes))

    def _isCurrent(self, gen):
        with self._genLock:
            return gen == self._gen

    def cancel(self):
        with self._genLock:
            self._gen += 1
        try:
            while True:
                self._queue.get_nowait()
                self._queue.task_done()
        except queue.Empty:
            pass
        player = self._player
        if player is not None:
            try:
                player.stop()
            except Exception:
                log.debugWarning('smoothtalker: player.stop failed',
                                 exc_info=True)

    def pause(self, switch):
        player = self._player
        if player is not None:
            try:
                player.pause(switch)
            except Exception:
                pass

    def terminate(self):
        self._stopping.set()
        self.cancel()
        self._queue.put(None)
        self._thread.join(timeout=5)
        self._dropPlayer()
        self._engine = None

    # -- worker ------------------------------------------------------------
    def _worker(self):
        while not self._stopping.is_set():
            job = self._queue.get()
            if job is None:
                break
            try:
                if self._isCurrent(job[0]):
                    self._render(job)
            except Exception:
                log.error('smoothtalker: synthesis failed', exc_info=True)
            finally:
                self._queue.task_done()

    #: Feed audio in slices rather than whole DMA blocks.  A block is 181 ms
    #: of speech; if cancellation lands just after a staleness check, that is
    #: how much stale audio escapes.  Slicing bounds the leak to ~46 ms.
    FEED_SLICE = 1024 * 2                       # bytes = 1024 frames @16-bit

    def _render(self, job):
        gen, text, indexes = job
        core = _getCore()
        spoke = [False]
        resampler = [None]
        # Left None when the audio device is unavailable: synthesis is then
        # skipped, but the index and done notifications below still fire, so
        # NVDA's speech manager moves on instead of waiting for audio that is
        # never coming.
        player = self._getPlayer() if text else None
        engine = self._getEngine() if player is not None else None

        def onAudio(block8, rate):
            # Called as each DMA block appears, so playback starts well
            # before a long utterance has finished synthesizing.
            if not self._isCurrent(gen):
                return
            if resampler[0] is None:
                resampler[0] = core.Resampler(rate, OUT_RATE)
            data = resampler[0].feed(core.to_pcm16(block8))
            for i in range(0, len(data), self.FEED_SLICE):
                if not self._isCurrent(gen):
                    return
                player.feed(data[i:i + self.FEED_SLICE])
                spoke[0] = True
            if not self._isCurrent(gen):
                # Cancelled while we were feeding -- flush what we just queued.
                try:
                    player.stop()
                except Exception:
                    pass

        if engine is not None:
            self._applyParams(engine)
            # Long text has to be spoken in engine-sized pieces; they feed the
            # same player and the same resampler, so playback stays continuous.
            try:
                for piece in _splitText(text):
                    if not self._isCurrent(gen):
                        return
                    engine.speak(
                        piece,
                        should_cancel=lambda: not self._isCurrent(gen),
                        on_block=onAudio)
            except core.EngineError:
                log.error('smoothtalker: the engine faulted', exc_info=True)
            except Exception:
                # A failing output device shows up here, inside feed().  Throw
                # the player away so the next utterance opens a new one rather
                # than going mute for the rest of the session.
                log.error('smoothtalker: playback failed', exc_info=True)
                self._dropPlayer()
        if not self._isCurrent(gen):
            return
        if spoke[0]:
            try:
                player.idle()
            except Exception:
                pass
        # Fire indexes once the audio has actually played, so NVDA's position
        # tracking follows the speech rather than running ahead of it.
        if self._isCurrent(gen):
            for index in indexes:
                self._fireIndex(index)
            synthDoneSpeaking.notify(synth=self)

    def _fireIndex(self, index):
        try:
            synthIndexReached.notify(synth=self, index=index)
        except Exception:
            log.debugWarning('smoothtalker: index notify failed', exc_info=True)

    # -- settings ----------------------------------------------------------
    # Values are engine units (0-9) throughout: NVDA's sliders are declared
    # over that range, so nothing needs rescaling.
    def _applyParams(self, engine):
        """Push pending settings to the engine.  Worker thread only."""
        with self._paramLock:
            if not self._paramsDirty:
                return
            params = (_getCore().DEFAULT_GENDER, self._tone,
                      self._volume, self._pitch, self._rate)
            self._paramsDirty = False
        try:
            engine.configure(params)
        except Exception:
            log.error('smoothtalker: could not apply settings', exc_info=True)

    def _markDirty(self):
        with self._paramLock:
            self._paramsDirty = True

    def _get_rate(self):
        return self._rate

    def _set_rate(self, value):
        self._rate = _clamp(value)
        self._markDirty()

    def _get_pitch(self):
        return self._pitch

    def _set_pitch(self, value):
        self._pitch = _clamp(value)
        self._markDirty()

    def _get_volume(self):
        return self._volume

    def _set_volume(self, value):
        self._volume = _clamp(value)
        self._markDirty()

    def _get_tone(self):
        return self._tone

    def _set_tone(self, value):
        self._tone = _clamp(value, 0, 1, TONE_DEFAULT)
        self._markDirty()

    # -- voices ------------------------------------------------------------
    def _get_availableVoices(self):
        return {'smoothtalker': VoiceInfo('smoothtalker',
                                          'SmoothTalker 3.5 (male)')}

    def _get_voice(self):
        return 'smoothtalker'

    def _set_voice(self, value):
        pass
