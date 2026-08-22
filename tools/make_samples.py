# -*- coding: utf-8 -*-
"""
Render the complete SmoothTalker sample set.

The ROM is SmoothTalker 3.5 "male voice" -- one voice, US English only -- so
"all voices and languages" is that single voice, swept across every value of
every parameter it exposes.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from render_samples import core, IMAGE, render, write_wav, OUT_RATE  # noqa: E402

OUT = os.path.join(ROOT, 'samples')

# (gender, tone, volume, pitch, speed) -- gender is inert on this ROM.
DEF = dict(gender=0, tone=0, volume=5, pitch=5, speed=5)


def params(**kw):
    p = dict(DEF)
    p.update(kw)
    return (p['gender'], p['tone'], p['volume'], p['pitch'], p['speed'])


def main():
    eng = core.Engine(IMAGE)
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.endswith('.wav'):
            os.remove(os.path.join(OUT, f))

    jobs = []
    jobs.append(('00_voice_smoothtalker_default',
                 'This is Smooth Talker, version three point five, male voice. '
                 'The only voice in this rom, speaking American English.',
                 params()))
    for r in range(10):
        jobs.append(('rate_%d' % r,
                     'Rate %d. The quick brown fox jumps over the lazy dog.' % r,
                     params(speed=r)))
    for p in range(10):
        jobs.append(('pitch_%d' % p,
                     'Pitch %d. The quick brown fox jumps over the lazy dog.' % p,
                     params(pitch=p)))
    for v in range(10):
        jobs.append(('volume_%d' % v,
                     'Volume %d. The quick brown fox jumps over the lazy dog.' % v,
                     params(volume=v)))
    for t in range(2):
        jobs.append(('tone_%d' % t,
                     'Tone %d. The quick brown fox jumps over the lazy dog.' % t,
                     params(tone=t)))
    jobs.append(('99_numbers_and_punctuation',
                 'Testing 1 2 3. It costs 45 dollars and 67 cents, or 89 percent '
                 'of the total. Doctor Smith arrived at 10:30 a.m. on May 3rd!',
                 params()))

    all_pcm = []
    rate = None
    for name, text, prm in jobs:
        pcm, r = render(eng, text, prm)
        rate = r
        dur = write_wav(os.path.join(OUT, '%s.wav' % name), pcm, r)
        all_pcm.append(pcm)
        all_pcm.append(b'\0' * (r // 3 * 2))     # ~330 ms gap
        print('  %-34s %5.2f s  (g%d t%d v%d p%d s%d)'
              % (name + '.wav', dur, prm[0], prm[1], prm[2], prm[3], prm[4]))

    joined = b''.join(all_pcm)
    dur = write_wav(os.path.join(OUT, 'ALL_samples_joined.wav'), joined, rate)
    print('  %-34s %5.2f s  (every sample back to back)'
          % ('ALL_samples_joined.wav', dur))
    print('\nnative engine rate %d Hz, written at %d Hz mono 16-bit'
          % (rate, OUT_RATE))


if __name__ == '__main__':
    main()
