# -*- coding: utf-8 -*-
"""Probe the SmoothTalker engine's parameter space by hashing rendered audio."""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from render_samples import core, IMAGE, render  # noqa: E402

TEXT = 'The quick brown fox jumps over the lazy dog.'


def digest(eng, params):
    pcm, rate = render(eng, TEXT, params)
    n = len(pcm) // 2
    import struct
    s = struct.unpack('<%dh' % n, pcm) if n else (0,)
    rms = (sum(v * v for v in s) / float(n or 1)) ** 0.5
    return (hashlib.sha1(pcm).hexdigest()[:12], n, round(n / float(rate), 3),
            round(rms, 1))


if __name__ == '__main__':
    eng = core.Engine(IMAGE)
    base = (0, 0, 5, 5, 5)
    print('=== GENDER 0..9 (tone0 vol5 pitch5 speed5) ===')
    seen = {}
    for g in range(10):
        d = digest(eng, (g, 0, 5, 5, 5))
        seen.setdefault(d[0], []).append(g)
        print('  gender=%d  sha=%s samples=%6d dur=%5.3fs rms=%6.1f' % (g,) + '' if False else
              '  gender=%d  sha=%s samples=%6d dur=%5.3fs rms=%6.1f' % (g, d[0], d[1], d[2], d[3]))
    print('  -> distinct outputs: %d  groups=%s' % (len(seen), list(seen.values())))

    print('=== TONE 0..3 ===')
    for t in range(4):
        d = digest(eng, (0, t, 5, 5, 5))
        print('  tone=%d    sha=%s samples=%6d dur=%5.3fs rms=%6.1f' % (t, d[0], d[1], d[2], d[3]))

    print('=== VOLUME 0..9 ===')
    for v in range(10):
        d = digest(eng, (0, 0, v, 5, 5))
        print('  volume=%d  sha=%s samples=%6d dur=%5.3fs rms=%6.1f' % (v, d[0], d[1], d[2], d[3]))

    print('=== PITCH 0..9 ===')
    for p in range(10):
        d = digest(eng, (0, 0, 5, p, 5))
        print('  pitch=%d   sha=%s samples=%6d dur=%5.3fs rms=%6.1f' % (p, d[0], d[1], d[2], d[3]))

    print('=== SPEED 0..9 ===')
    for s in range(10):
        d = digest(eng, (0, 0, 5, 5, s))
        print('  speed=%d   sha=%s samples=%6d dur=%5.3fs rms=%6.1f' % (s, d[0], d[1], d[2], d[3]))

    print('=== OUT OF RANGE (clamping) ===')
    for label, params in (('volume=15', (0, 0, 15, 5, 5)),
                          ('pitch=15', (0, 0, 5, 15, 5)),
                          ('speed=15', (0, 0, 5, 5, 15)),
                          ('gender=255', (255, 0, 5, 5, 5))):
        d = digest(eng, params)
        print('  %-11s sha=%s samples=%6d dur=%5.3fs rms=%6.1f' % (label, d[0], d[1], d[2], d[3]))
