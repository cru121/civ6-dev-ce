#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""bnktool: read and build Wwise soundbanks (version 113) as used by Civilization VI.

  bnktool.py hash NAME                 FNV-1 32-bit id Wwise uses for NAME (case-insensitive)
  bnktool.py dump BANK.bnk             chunks and HIRC object list
  bnktool.py make-sfx OUTDIR --wav FILE --event NAME [--bank NAME] [--control-from OFFICIAL.bnk]
                                       builds a small bank (+ Banks.ini) whose event NAME plays FILE (16-bit PCM wav).
                                       --control-from also copies a known-good in-bank sound from an official bank under a second event,
                                       to tell "our bank structure is wrong" from "our PCM audio is wrong".

make-sfx works by assembling objects from byte templates taken from the game's own banks
(sound, actor-mixer, event, play action; layouts verified against Cinematic_Tutorials_MusicSFX.bnk),
so no Wwise installation is needed. Experimental.
"""
import argparse, os, struct, sys, wave

VERSION = 113
Z = b'\x00'


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
# exact template bodies (hex) from the game
T_MIXER = '04c6312300000000000000a87723210001000000104100c000000100000000000000000000010000002c261d01'
T_WORKUNIT = '5f4c193500000037bcb7e2000000000001000000803f00c30000010000000000000000000001000000a8772321'


def official_sound(bank_path, sound_id=None, min_media=0):
    """Pick the smallest in-bank (stream type 0) sound of an official bank: returns (body, media bytes). Used as a control."""
    data = open(bank_path, 'rb').read()
    didx, dstart = {}, 0
    for tag, off, size in read_chunks(data):
        if tag == 'DIDX':
            for i in range(size // 12):
                mid, o, sz = struct.unpack_from('<III', data, off + i * 12); didx[mid] = (o, sz)
        if tag == 'DATA': dstart = off
    best = None
    for t, body in read_hirc(data):
        if t != 2 or len(body) != 46: continue
        oid, plugin, stream, src, msz = struct.unpack_from('<IIBII', body, 0)
        if stream != 0 or src not in didx or body[18:21] != bytes(3): continue
        o, sz = didx[src]
        if sound_id is not None and oid != sound_id: continue
        if sz < min_media: continue
        if best is None or sz < best[2]: best = (body, data[dstart + o:dstart + o + sz], sz)
    if best is None: sys.exit('no suitable sound found in ' + bank_path)
    return best[0], best[1]


def official_sound_of_event(bank_path, event):
    """(sound body, media bytes) of the sound that an official event plays (the event must have one play action targeting an in-bank sound)."""
    data = open(bank_path, 'rb').read()
    objs = {struct.unpack_from('<I', body, 0)[0]: (t, body) for t, body in read_hirc(data)}
    ev = objs[fnv(event)][1]
    act = objs[struct.unpack_from('<I', ev, 8)[0]][1]
    snd = objs[struct.unpack_from('<I', act, 6)[0]][1]
    src = struct.unpack_from('<I', snd, 9)[0]
    for tag, off, size in read_chunks(data):
        if tag == 'DIDX':
            for i in range(size // 12):
                mid, o, sz = struct.unpack_from('<III', data, off + i * 12)
                if mid == src: didx = (o, sz)
        if tag == 'DATA': dstart = off
    return snd, data[dstart + didx[0]:dstart + didx[0] + didx[1]]


def official_chain(bank_path, event):
    """Bodies of the actor-mixers above the sound that an official event plays (bottom to top), to be reused as the parent chain of our own sounds."""
    data = open(bank_path, 'rb').read()
    objs = {struct.unpack_from('<I', body, 0)[0]: (t, body) for t, body in read_hirc(data)}
    ev = objs[fnv(event)][1]
    act = objs[struct.unpack_from('<I', ev, 8)[0]][1]
    snd = objs[struct.unpack_from('<I', act, 6)[0]][1]
    chain, parent = [], struct.unpack_from('<I', snd, 25)[0]
    while parent:
        body = objs[parent][1]
        chain.append(body)
        parent = mixer_parent(body)[0]
    return chain


def build_bank(name, voices, align=16, chain=None):
    """voices: list of dicts {event, wem, body (optional: official sound body to clone)}.
    Parent objects: our own simple work unit > mixer, or (chain=official_chain(...)) a copy of an official parent chain with new ids."""
    bank_id = fnv(name)
    mixer_id = fnv(name + ('/chain0' if chain else '/mixer'))
    wu_id = fnv(name + '/workunit')
    objs, media, sound_ids, ids = [], [], [], {'bank': bank_id, 'events': {}}
    sounds_l, actions_l, events_l = [], [], []   # official banks list children before their parents: sounds, mixers (bottom to top), actions, events
    for i, v in enumerate(voices):
        media_id = fnv('%s/media%d' % (name, i)); sound_id = fnv('%s/sound%d' % (name, i)); action_id = fnv('%s/play%d' % (name, i))
        wem = v['wem']
        if v.get('body'):
            sb = bytearray(v['body'])
            sb[0:4] = u32(sound_id); sb[9:13] = u32(media_id); sb[25:29] = u32(mixer_id)
            if 'plugin' in v: sb[4:8] = u32(v['plugin']); sb[13:17] = u32(len(wem))   # same sound object, other codec (e.g. PCM)
            sound = bytes(sb)
        else:
            sound = u32(sound_id) + u32(0x00010001) + Z + u32(media_id) + u32(len(wem)) + Z + bytes(3) + u32(0) + u32(mixer_id) + SOUND_TAIL
        action = u32(action_id) + b'\x03\x04' + u32(sound_id) + bytes(3) + b'\x04' + u32(bank_id)
        evt = u32(fnv(v['event'])) + u32(1) + u32(action_id)
        sounds_l.append((2, sound)); actions_l.append((3, action)); events_l.append((4, evt))
        media.append((media_id, wem)); sound_ids.append(sound_id); ids['events'][v['event']] = fnv(v['event'])
    if chain:
        cids = [fnv('%s/chain%d' % (name, i)) for i in range(len(chain))]
        parents = []
        for i, body in enumerate(chain):
            head, _ = split_children(body)
            h = bytearray(head)
            h[0:4] = u32(cids[i])
            orig_parent, pq = mixer_parent(body)
            h[pq:pq + 4] = u32(cids[i + 1] if i + 1 < len(cids) else orig_parent)
            kids = sound_ids if i == 0 else [cids[i - 1]]
            parents.append((7, bytes(h) + u32(len(kids)) + b''.join(u32(x) for x in kids)))
        objs = sounds_l + parents + actions_l + events_l
    else:
        mixer = bytearray(bytes.fromhex(T_MIXER)[:-8])
        mixer[0:4] = u32(mixer_id); mixer[11:15] = u32(wu_id); mixer[18:22] = struct.pack('<f', 0.0)
        mixer = bytes(mixer) + u32(len(sound_ids)) + b''.join(u32(x) for x in sound_ids)
        wu = bytearray(bytes.fromhex(T_WORKUNIT))
        wu[0:4] = u32(wu_id); wu[-4:] = u32(mixer_id)
        objs = sounds_l + [(7, mixer), (7, bytes(wu))] + actions_l + events_l
    return assemble(name, objs, media, align), ids


def assemble(name, objs, media, align=16):
    """objs: [(hirc type, body)], media: [(media id, bytes)] -> bank file bytes."""
    bank_id = fnv(name)
    hirc = u32(len(objs)) + b''.join(bytes([t]) + u32(len(b)) + b for t, b in objs)
    data, entries = b'', []
    for mid, wem in media:
        entries.append((mid, len(data), len(wem)))
        data += wem + bytes((-len(wem)) % align)
    # the media index is sorted by id in official banks (Wwise looks media up by binary search)
    didx = b''.join(u32(m) + u32(o) + u32(s) for m, o, s in sorted(entries))
    bkhd = u32(VERSION) + u32(bank_id) + u32(0) + u32(0) + u32(4)
    # Official banks start the DATA payload at a file offset that is a multiple of 16: Wwise pads the BKHD tail to get there.
    while (8 + len(bkhd) + 8 + len(didx) + 8) % align:
        bkhd += u32(0)

    def chunk(tag, body): return tag.encode() + u32(len(body)) + body
    return chunk('BKHD', bkhd) + chunk('DIDX', didx) + chunk('DATA', data) + chunk('HIRC', hirc)


def node_parent_offset(body):
    """Offset of the DirectParentID in a sound / actor-mixer body (both start: id, [sound fields], then NodeBaseParams)."""
    return None


def split_children(body):
    """Actor-mixer body -> (head, [child ids]); the child list is the tail: u32 count + ids."""
    for k in range(1, 400):
        pos = len(body) - 4 - 4 * k
        if pos >= 0 and struct.unpack_from('<I', body, pos)[0] == k: return body[:pos], [struct.unpack_from('<I', body, pos + 4 + 4 * i)[0] for i in range(k)]
    sys.exit('cannot find the child list')


def mixer_parent(body):
    """DirectParentID of an actor-mixer: id, ovFx, nfx, [bitsBypass + 7 bytes per fx], ovAttach, bus, parent."""
    q = 4; q += 1; nfx = body[q]; q += 1
    if nfx: q += 1 + 7 * nfx
    q += 1 + 4
    return struct.unpack_from('<I', body, q)[0], q


def cmd_extract(a):
    """Minimal bank holding one official event: event, play action, sound, and the sound's real parent mixers (child lists trimmed), plus its media."""
    data = open(a.src, 'rb').read()
    objs = {struct.unpack_from('<I', body, 0)[0]: (t, body) for t, body in read_hirc(data)}
    didx, dstart = {}, 0
    for tag, off, size in read_chunks(data):
        if tag == 'DIDX':
            for i in range(size // 12):
                mid, o, sz = struct.unpack_from('<III', data, off + i * 12); didx[mid] = (o, sz)
        if tag == 'DATA': dstart = off
    ev = objs[fnv(a.event)][1]
    act_id = struct.unpack_from('<I', ev, 8)[0]
    act = objs[act_id][1]
    sound_id = struct.unpack_from('<I', act, 6)[0]
    snd = objs[sound_id][1]
    src = struct.unpack_from('<I', snd, 9)[0]
    out = [(4, struct.pack('<II', fnv(a.new), 1) + u32(act_id)), (3, act), (2, snd)]
    child, parent = sound_id, struct.unpack_from('<I', snd, 25)[0]
    while parent:
        t, body = objs[parent]
        head, kids = split_children(body)
        body = head + u32(1) + u32(child)
        out.append((t, body))
        child = parent
        parent = mixer_parent(body)[0]
    o, sz = didx[src]
    blob = assemble(a.bank, out, [(src, data[dstart + o:dstart + o + sz])])
    os.makedirs(a.outdir, exist_ok=True)
    open(os.path.join(a.outdir, a.bank + '.bnk'), 'wb').write(blob)
    ini_path = os.path.join(a.outdir, a.ini)
    lines = open(ini_path, encoding='utf-8').read().splitlines() if os.path.exists(ini_path) else ['[Global]', '', '[Menu]', '', '[InGame]', '', '[2D]', '', '[3D]', '', '[FMV]']
    if (a.bank + '.bnk') not in lines: lines.insert(lines.index('[%s]' % a.section) + 1, a.bank + '.bnk')
    open(ini_path, 'w', newline='\r\n').write('\n'.join(lines) + '\n')
    print('wrote %s.bnk (%d bytes): %d objects, media %d (%d bytes), event %s' % (a.bank, len(blob), len(out), src, sz, a.new))


def cmd_make_sfx(a):
    voices = [dict(event=a.event, wem=pcm_wem(a.wav))]
    if a.control_from:
        body, wem = official_sound_of_event(a.control_from, a.control_source) if a.control_source else official_sound(a.control_from, min_media=a.control_min)
        if a.chain_from: voices = [dict(event=a.event, wem=pcm_wem(a.wav), body=body, plugin=0x00010001)]   # our PCM sound on an official sound object
        voices.append(dict(event=a.control_event, wem=wem, body=body))
    chain = official_chain(a.chain_from, a.chain_event) if a.chain_from else None
    blob, ids = build_bank(a.bank, voices, chain=chain)
    os.makedirs(a.outdir, exist_ok=True)
    open(os.path.join(a.outdir, a.bank + '.bnk'), 'wb').write(blob)
    ini_path = os.path.join(a.outdir, a.ini)
    lines = open(ini_path, encoding='utf-8').read().splitlines() if os.path.exists(ini_path) else ['[Global]', '', '[Menu]', '', '[InGame]', '', '[2D]', '', '[3D]', '', '[FMV]']
    if (a.bank + '.bnk') not in lines: lines.insert(lines.index('[%s]' % a.section) + 1, a.bank + '.bnk')
    open(ini_path, 'w', newline='\r\n').write('\n'.join(lines) + '\n')
    print('wrote %s.bnk (%d bytes), %s' % (a.bank, len(blob), a.ini))
    print('bank id %d' % ids['bank'])
    for e, i in ids['events'].items(): print('event "%s" id %d' % (e, i))


def cmd_clone_bank(a):
    """Copy an official bank under a new bank id and rename events (old name -> new name). A bisecting aid: a byte-exact clone with only ids changed."""
    data = bytearray(open(a.src, 'rb').read())
    chunks = read_chunks(bytes(data))
    for tag, off, size in chunks:
        if tag == 'BKHD': data[off + 4:off + 8] = u32(fnv(a.bank))
    renames = {fnv(old): fnv(new) for old, new in (r.split('=') for r in a.rename)}
    done = 0
    for tag, off, size in chunks:
        if tag != 'HIRC': continue
        n = struct.unpack_from('<I', data, off)[0]; q = off + 4
        for _ in range(n):
            t = data[q]; s = struct.unpack_from('<I', data, q + 1)[0]
            if t == 4:
                oid = struct.unpack_from('<I', data, q + 5)[0]
                if oid in renames: data[q + 5:q + 9] = u32(renames[oid]); done += 1
            q += 5 + s
    if done != len(renames): sys.exit('only %d of %d events found' % (done, len(renames)))
    os.makedirs(a.outdir, exist_ok=True)
    open(os.path.join(a.outdir, a.bank + '.bnk'), 'wb').write(bytes(data))
    ini_path = os.path.join(a.outdir, a.ini)
    lines = open(ini_path, encoding='utf-8').read().splitlines() if os.path.exists(ini_path) else ['[Global]', '', '[Menu]', '', '[InGame]', '', '[2D]', '', '[3D]', '', '[FMV]']
    if (a.bank + '.bnk') not in lines:
        lines.insert(lines.index('[%s]' % a.section) + 1, a.bank + '.bnk')
    open(ini_path, 'w', newline='\r\n').write('\n'.join(lines) + '\n')
    print('wrote %s.bnk (%d bytes) bank id %d, renamed %d event(s)' % (a.bank, len(data), fnv(a.bank), done))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest='cmd', required=True)
    s = sp.add_parser('hash'); s.add_argument('name'); s.set_defaults(f=lambda a: print(fnv(a.name)))
    s = sp.add_parser('dump'); s.add_argument('bank'); s.set_defaults(f=cmd_dump)
    s = sp.add_parser('make-sfx'); s.add_argument('outdir'); s.add_argument('--wav', required=True); s.add_argument('--event', required=True)
    s.add_argument('--control-from', help='official .bnk to copy a known-good sound from (adds a second event)')
    s.add_argument('--control-event', default='Play_DevCE_BankTest_Control')
    s.add_argument('--control-min', type=int, default=0, help='minimum media size in bytes')
    s.add_argument('--control-source', help='use the sound of this official event of --control-from (instead of the smallest in-bank sound)')
    s.add_argument('--chain-from', help='official .bnk whose parent mixer chain (above --chain-event) is copied as the parent of our sounds')
    s.add_argument('--chain-event', default='Alert_Positive')
    s.add_argument('--bank', default='DevCE_Test_Bank'); s.add_argument('--ini', default='DevCETestBanks.ini'); s.add_argument('--section', default='InGame', help='Banks.ini section: Global, Menu, InGame, 2D, 3D or FMV (FMV banks load only on UI.LoadSoundBankGroup(5))'); s.set_defaults(f=cmd_make_sfx)
    s = sp.add_parser('clone-bank'); s.add_argument('src'); s.add_argument('outdir'); s.add_argument('--bank', required=True)
    s.add_argument('--rename', action='append', default=[], help='OLD_EVENT=NEW_EVENT (repeatable)'); s.add_argument('--ini', default='DevCETestBanks.ini'); s.add_argument('--section', default='InGame', help='Banks.ini section: Global, Menu, InGame, 2D, 3D or FMV (FMV banks load only on UI.LoadSoundBankGroup(5))'); s.set_defaults(f=cmd_clone_bank)
    s = sp.add_parser('extract'); s.add_argument('src'); s.add_argument('outdir'); s.add_argument('--bank', required=True); s.add_argument('--event', required=True); s.add_argument('--new', required=True)
    s.add_argument('--ini', default='DevCETestBanks.ini'); s.add_argument('--section', default='InGame', help='Banks.ini section: Global, Menu, InGame, 2D, 3D or FMV (FMV banks load only on UI.LoadSoundBankGroup(5))'); s.set_defaults(f=cmd_extract)
    a = p.parse_args(); a.f(a)


if __name__ == '__main__':
    main()
