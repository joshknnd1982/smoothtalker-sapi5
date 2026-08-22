# -*- coding: utf-8 -*-
"""
Standalone renderer for the SmoothTalker (First Byte / Dr. Sbaitso) engine.

No NVDA, no SAPI4, no registry: it loads _smoothtalker_engine/core.py directly,
runs the 1990 DOS engine image under Unicorn and writes .wav files.
"""
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, 'bin')
ENGINE_DIR = os.path.join(BIN, '_smoothtalker_engine')
sys.path.insert(0, os.path.join(ENGINE_DIR, 'lib'))
sys.path.insert(0, BIN)

from _smoothtalker_engine import core  # noqa: E402

IMAGE = os.path.join(ENGINE_DIR, 'engine.bin')
OUT_RATE = 22050


def render(engine, text, params):
    """params = (gender, tone, volume, pitch, speed) -> (pcm16, rate)"""
    engine.configure(params)
    chunks = []
    rate = [None]

    def on_block(data, r):
        rate[0] = r
        chunks.append(core.to_pcm16(data))

    # 255-char limit: chunk on sentence/clause/word boundaries
    for piece in split_text(text):
        engine.speak(piece, on_block=on_block)
    return b''.join(chunks), (rate[0] or 11025)


def split_text(text, limit=core.MAX_TEXT):
    text = ' '.join(text.split())
    out = []
    while len(text) > limit:
        window = text[:limit + 1]
        cut = -1
        for seps in ('.!?', ',;:'):
            best = -1
            for sep in seps:
                idx = window.rfind(sep + ' ')
                if idx > best:
                    best = idx
            if best > limit // 4:
                cut = best + 1
                break
        if cut < 0:
            cut = window.rfind(' ')
        if cut <= 0:
            cut = limit
        out.append(text[:cut].strip())
        text = text[cut:].strip()
    if text:
        out.append(text)
    return [c for c in out if c]


def write_wav(path, pcm16, rate, out_rate=OUT_RATE):
    data = core.resample16(pcm16, rate, out_rate) if rate != out_rate else pcm16
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(out_rate)
        w.writeframes(data)
    return len(data) // 2 / float(out_rate)


if __name__ == '__main__':
    eng = core.Engine(IMAGE)
    pcm, rate = render(eng, 'Hello, this is Smooth Talker speaking.',
                       (0, 0, 5, 5, 5))
    print('native rate: %s Hz, %d samples' % (rate, len(pcm) // 2))
    out = os.path.join(ROOT, 'samples', 'smoke.wav')
    print('%.2f s -> %s' % (write_wav(out, pcm, rate), out))
