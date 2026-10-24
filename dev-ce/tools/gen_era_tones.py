#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Writes four short test wavs for the era-switch bank test: Ancient = 1 beep (220 Hz), Medieval = 2 beeps (330 Hz), Industrial = 3 beeps (440 Hz), Modern = 4 beeps (660 Hz).
Usage: gen_era_tones.py OUTDIR"""
import math, os, struct, sys, wave

RATE = 44100


def tone(path, freq, beeps):
    frames = bytearray()
    for _ in range(beeps):
        n = int(RATE * 0.22)
        for i in range(n):
            env = min(1.0, i / 400.0, (n - i) / 400.0)
            frames += struct.pack('<h', int(9000 * env * math.sin(2 * math.pi * freq * i / RATE)))
        frames += bytes(2 * int(RATE * 0.18))
    w = wave.open(path, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(bytes(frames)); w.close()


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    for name, freq, beeps in (('ancient', 220, 1), ('medieval', 330, 2), ('industrial', 440, 3), ('modern', 660, 4)):
        tone(os.path.join(out, 'era_%s.wav' % name), freq, beeps)
    print('wrote 4 wavs to', out)


if __name__ == '__main__':
    main()
