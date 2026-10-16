#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""bnktool: read and build Wwise soundbanks (version 113) as used by Civilization VI.

  bnktool.py hash NAME                 FNV-1 32-bit id Wwise uses for NAME (case-insensitive)
  bnktool.py dump BANK.bnk             chunks and HIRC object list
  bnktool.py make-sfx OUTDIR --wav FILE --event NAME [--bank NAME]
                                       builds a one-sound bank (+ Banks.ini) whose event NAME plays FILE (16-bit PCM wav)

make-sfx works by assembling objects from byte templates taken from the game's own banks
(sound, actor-mixer, event, play action; layouts verified against Cinematic_Tutorials_MusicSFX.bnk),
so no Wwise installation is needed. Experimental: whether the game accepts such a bank is what it is for.
"""
import argparse, os, struct, sys, wave

VERSION = 113


def fnv(name):
    h = 2166136261
    for c in name.lower().encode():
        h = (h * 16777619) & 0xFFFFFFFF
        h ^= c
    return h


def u32(v): return struct.pack('<I', v & 0xFFFFFFFF)


# ---------------------------------------------------------------- reading
def read_chunks(data):
    i, out = 0, []
    while i + 8 <= len(data):
        tag = data[i:i+4].decode('latin1'); size = struct.unpack_from('<I', data, i + 4)[0]
        out.append((tag, i + 8, size)); i += 8 + size
    return out


def read_hirc(data):
    objs = []
    for tag, off, size in read_chunks(data):
        if tag == 'HIRC':
            n = struct.unpack_from('<I', data, off)[0]; q = off + 4
            for _ in range(n):
                t = data[q]; s = struct.unpack_from('<I', data, q + 1)[0]
                objs.append((t, data[q + 5:q + 5 + s])); q += 5 + s
    return objs


HIRC_TYPES = {1: 'settings', 2: 'sound', 3: 'action', 4: 'event', 5: 'random/seq', 6: 'switch', 7: 'actor-mixer', 8: 'bus', 9: 'layer',
              10: 'music-segment', 11: 'music-track', 12: 'music-switch', 13: 'music-playlist', 14: 'attenuation', 18: 'state', 19: 'fx', 20: 'aux-bus'}


def cmd_dump(a):
    data = open(a.bank, 'rb').read()
    for tag, off, size in read_chunks(data):
        extra = ''
        if tag == 'BKHD':
            ver, bid = struct.unpack_from('<II', data, off); extra = 'version %d bank id %d' % (ver, bid)
        print('%-4s offset %-9d size %-9d %s' % (tag, off, size, extra))
    for t, body in read_hirc(data):
        print('  %-14s id %-11d %d bytes' % (HIRC_TYPES.get(t, 'type %d' % t), struct.unpack_from('<I', body, 0)[0], len(body)))


# ---------------------------------------------------------------- building
def pcm_wem(wav_path):
    """Wwise PCM media: RIFF/WAVE with an extensible fmt chunk, like the game's own PCM .wem files."""
    w = wave.open(wav_path, 'rb')
    ch, rate, width, frames = w.getnchannels(), w.getframerate(), w.getsampwidth(), w.getnframes()
    if width != 2: sys.exit('only 16-bit PCM wav is supported')
    pcm = w.readframes(frames)
    mask = {1: 4, 2: 3}.get(ch)
    if mask is None: sys.exit('only mono or stereo wav is supported')
    fmt = struct.pack('<HHIIHHH', 0xFFFE, ch, rate, rate * ch * 2, ch * 2, 16, 6) + struct.pack('<HI', 0, mask)
    akd = struct.pack('<ffII', 1.0, 1.0, 0, 0)
    body = b'WAVE' + b'fmt ' + u32(len(fmt)) + fmt + b'akd ' + u32(len(akd)) + akd + b'JUNK' + u32(12) + bytes(12) + b'data' + u32(len(pcm)) + pcm
    return b'RIFF' + u32(len(body)) + body


# Byte templates from Cinematic_Tutorials_MusicSFX.bnk. Ids/links are patched in by build_bank().
SOUND_TAIL = bytes.fromhex('000000c000040101000000000000000000')   # after the parent id
WORK_BUS = 0xE2B7BC37   # bus the game's cinematic work unit routes to (exists in Init.bnk)


# exact template bodies (hex) from the game
T_MIXER = '04c6312300000000000000a87723210001000000104100c000000100000000000000000000010000002c261d01'
T_WORKUNIT = '5f4c193500000037bcb7e2000000000001000000803f00c30000010000000000000000000001000000a8772321'


def build_bank(name, event, wem, align=16):
    bank_id = fnv(name)
    media_id = fnv(name + '/media')
    sound_id = fnv(name + '/sound')
    mixer_id = fnv(name + '/mixer')
    wu_id = fnv(name + '/workunit')
    action_id = fnv(name + '/play')
    event_id = fnv(event)

    sound = (u32(sound_id) + u32(0x00010001) + b'\x00' + u32(media_id) + u32(len(wem)) + b'\x00' + b'\x00\x00\x00' + u32(0) + u32(mixer_id) + SOUND_TAIL)
    mixer = bytearray(bytes.fromhex(T_MIXER))
    mixer[0:4] = u32(mixer_id); mixer[11:15] = u32(wu_id); mixer[18:22] = struct.pack('<f', 0.0); mixer[-4:] = u32(sound_id)
    wu = bytearray(bytes.fromhex(T_WORKUNIT))
    wu[0:4] = u32(wu_id); wu[-4:] = u32(mixer_id)
    action = u32(action_id) + b'\x03\x04' + u32(sound_id) + b'\x00\x00\x00\x04' + u32(bank_id)
    evt = u32(event_id) + u32(1) + u32(action_id)

    objs = [(7, bytes(wu)), (7, bytes(mixer)), (2, sound), (3, action), (4, evt)]
    hirc = u32(len(objs)) + b''.join(bytes([t]) + u32(len(b)) + b for t, b in objs)

    pad = (-len(wem)) % align
    data = wem + bytes(pad)
    didx = u32(media_id) + u32(0) + u32(len(wem))
    bkhd = u32(VERSION) + u32(bank_id) + u32(0) + u32(0) + u32(4) + u32(0)

    def chunk(tag, body): return tag.encode() + u32(len(body)) + body
    return chunk('BKHD', bkhd) + chunk('DIDX', didx) + chunk('DATA', data) + chunk('HIRC', hirc), dict(bank=bank_id, event=event_id, media=media_id)


def cmd_make_sfx(a):
    bank = a.bank
    wem = pcm_wem(a.wav)
    blob, ids = build_bank(bank, a.event, wem)
    os.makedirs(a.outdir, exist_ok=True)
    open(os.path.join(a.outdir, bank + '.bnk'), 'wb').write(blob)
    ini = '; Banks.ini written by bnktool\n[Global]\n\n[Menu]\n\n[InGame]\n%s.bnk\n\n[2D]\n\n[3D]\n\n[FMV]\n' % bank
    open(os.path.join(a.outdir, a.ini), 'w', newline='\r\n').write(ini)
    print('wrote %s.bnk (%d bytes), %s' % (bank, len(blob), a.ini))
    print('bank id %d, event "%s" id %d, media id %d' % (ids['bank'], a.event, ids['event'], ids['media']))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest='cmd', required=True)
    s = sp.add_parser('hash'); s.add_argument('name'); s.set_defaults(f=lambda a: print(fnv(a.name)))
    s = sp.add_parser('dump'); s.add_argument('bank'); s.set_defaults(f=cmd_dump)
    s = sp.add_parser('make-sfx'); s.add_argument('outdir'); s.add_argument('--wav', required=True); s.add_argument('--event', required=True)
    s.add_argument('--bank', default='DevCE_Test_Bank'); s.add_argument('--ini', default='DevCETestBanks.ini'); s.set_defaults(f=cmd_make_sfx)
    a = p.parse_args(); a.f(a)


if __name__ == '__main__':
    main()
