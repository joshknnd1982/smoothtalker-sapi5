# -*- coding: utf-8 -*-
"""
Prove the C++ engine is the same engine as the Python reference.

Renders the same text with the same parameters through both and compares the
PCM sample for sample.  Both are asked for native-rate output so the
comparison is of the ROM's own samples, with no resampling in between.
"""
import hashlib
import os
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from render_samples import core, IMAGE, render  # noqa: E402

TEXTS = [
    'Hello, this is Smooth Talker speaking.',
    'The quick brown fox jumps over the lazy dog.',
    'Testing 1 2 3. It costs 45 dollars and 67 cents.',
    'She sells sea shells by the sea shore, and the shells she sells are '
    'surely sea shells, so if she sells shells on the seashore, the shells '
    'she sells are seashore shells for sure. That is a long one that has to '
    'be split into more than one engine call to get through.',
]

# (gender, tone, volume, pitch, speed)
CASES = [(0, 0, 5, 5, 5), (0, 1, 5, 5, 5), (0, 0, 0, 5, 5), (0, 0, 9, 5, 5),
         (0, 0, 5, 0, 5), (0, 0, 5, 9, 5), (0, 0, 5, 5, 0), (0, 0, 5, 5, 9),
         (0, 1, 9, 0, 9), (0, 1, 0, 9, 0)]


def cpp_render(exe, text, prm, out):
    gender, tone, volume, pitch, speed = prm
    subprocess.run([exe, '--text', text, '--out', out, '--out-rate', '0',
                    '--quiet', '--gender', str(gender), '--tone', str(tone),
                    '--volume', str(volume), '--pitch', str(pitch),
                    '--rate', str(speed)], check=True,
                   stdout=subprocess.DEVNULL)
    with wave.open(out, 'rb') as w:
        return w.readframes(w.getnframes()), w.getframerate()


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, 'build_probe', 'x64', 'st_render.exe')
    scratch = os.path.join(ROOT, 'build_probe', '_cmp.wav')
    eng = core.Engine(IMAGE)

    total = 0
    bad = 0
    for text in TEXTS:
        label = (text[:40] + '...') if len(text) > 43 else text
        print('\n"%s"' % label)
        for prm in CASES:
            py_pcm, py_rate = render(eng, text, prm)
            cpp_pcm, cpp_rate = cpp_render(exe, text, prm, scratch)
            total += 1
            same = (py_pcm == cpp_pcm and py_rate == cpp_rate)
            if not same:
                bad += 1
                n = min(len(py_pcm), len(cpp_pcm))
                first = next((i for i in range(n) if py_pcm[i] != cpp_pcm[i]),
                             n)
                print('  MISMATCH  g%dt%dv%dp%ds%d  py=%d/%dHz cpp=%d/%dHz '
                      'first diff at byte %d'
                      % (prm[0], prm[1], prm[2], prm[3], prm[4],
                         len(py_pcm), py_rate, len(cpp_pcm), cpp_rate, first))
            else:
                print('  ok  g%dt%dv%dp%ds%d  %6d bytes @ %d Hz  sha=%s'
                      % (prm[0], prm[1], prm[2], prm[3], prm[4], len(py_pcm),
                         py_rate, hashlib.sha1(py_pcm).hexdigest()[:12]))

    if os.path.exists(scratch):
        os.remove(scratch)
    print('\n%d/%d cases identical' % (total - bad, total))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
