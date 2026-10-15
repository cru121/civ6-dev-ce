# Generates the long test tunes (run with a Python that has numpy, soundfile, lameenc). Not part of the mod itself.
import sys, numpy as np, soundfile as sf, lameenc

RATE = 44100

def render(notes, step, dur_total, wave):
    n = int(RATE * dur_total)
    out = np.zeros((n, 2), dtype=np.float32)
    t_note = np.arange(int(RATE * step * 1.6)) / RATE
    env = np.minimum(1, t_note * 80) * np.exp(-t_note * 3.0)
    for i, (f, pan) in enumerate(notes):
        start = int(i * step * RATE)
        if start >= n: break
        tone = wave(f, t_note) * env * 0.35
        end = min(n, start + len(tone))
        out[start:end, 0] += tone[:end - start] * (1 - pan) * 0.7
        out[start:end, 1] += tone[:end - start] * (1 + pan) * 0.7
    return np.clip(out, -1, 1)

def melody(scale, count, seed):
    rng = np.random.default_rng(seed)
    idx, notes = 0, []
    for i in range(count):
        idx = int(np.clip(idx + rng.integers(-2, 3), 0, len(scale) - 1))
        notes.append((scale[idx], float(np.sin(i / 5.0)) * 0.6))
    return notes

pent = [220 * 2 ** (k / 12) for k in (0, 2, 4, 7, 9, 12, 14, 16, 19, 21)]
minor = [196 * 2 ** (k / 12) for k in (0, 2, 3, 5, 7, 8, 10, 12, 14, 15)]
soft = lambda f, t: np.sin(2 * np.pi * f * t) + 0.3 * np.sin(4 * np.pi * f * t)
bright = lambda f, t: np.sin(2 * np.pi * f * t) + 0.5 * np.sin(6 * np.pi * f * t) + 0.25 * np.sin(10 * np.pi * f * t)

# ogg: ~75 s, calm pentatonic arpeggio; mp3: ~75 s, brighter minor tune
ogg = render(melody(pent, 200, 1), 0.375, 75, soft)
mp3 = render(melody(minor, 150, 2), 0.5, 75, bright)
outdir = sys.argv[1]
import threading

def work():
    with sf.SoundFile(outdir + "/tune_ogg.ogg", "w", RATE, 2, format="OGG", subtype="VORBIS") as f:
        for i in range(0, len(ogg), 8192):   # small blocks: one huge write overflows the encoder
            f.write(ogg[i:i + 8192])
    enc = lameenc.Encoder()
    enc.set_bit_rate(128); enc.set_in_sample_rate(RATE); enc.set_channels(2); enc.set_quality(2)
    pcm = (mp3 * 32767).astype(np.int16).tobytes()
    open(outdir + "/tune_mp3.mp3", "wb").write(enc.encode(pcm) + enc.flush())
    print("ok")

threading.stack_size(64 * 1024 * 1024)   # libsndfile's vorbis encoder overflows the default stack
t = threading.Thread(target=work)
t.start()
t.join()
