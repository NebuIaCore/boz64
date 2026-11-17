# Adds Black Ops PC's other takes of Kino der Toten's character voice lines to the phone game's voice banks.
#
# The phone game's banks (ingame/sound/snd_player_<tank|nikolai|takeo|richtofen>.group.bin, shared by Kino and
# Ascension) hold two or three takes of each voice event the game picks from at random (a CSoundVariation:
# kill_streak, low_ammo, revived, ...), cut from BO1's Kino recordings (vox_plr_<n>_<event>_<take>). BO1 PC has 5 to
# 14 takes of each. This adds the rest, for every variation whose takes all come from one BO1 event:
#   CIwSoundData     the clip: IMA ADPCM, 11025 Hz mono, 256-byte blocks of 505 samples (like the phone's own)
#   CIwSoundSpec     a copy of the event's first spec (volume, pitch...) pointing at the clip
#   CSoundVariation  the event's spec list, which the game picks from with rand() % count
# Spec and clip are named after BO1's alias (vox_plr_0_kill_streak_5), as the phone's clips are.
#
# Some lines the game plays as one spec, by hash (SINGLES: the explosive kill, the mystery box's poor weapon, and the
# weapons' pickup lines, CPlayerWeapon.playerBuySound). For those this adds a CSoundVariation with the spec's own name
# holding the spec and BO1's other takes; src/vox.cpp makes the game's spec lookup pick from it.
#
# The same alias names exist in several maps' folders, with different recordings: the phone's clips match these
# folders' (checked by correlation), Kino's for all but Ascension's sickle.
#
# Black Ops PC also has lines for events the phone game does not voice (EVENTS: perks, power-ups, "no money", the
# perk limit, some kills, the Ray Gun and Thundergun pickups). Each becomes a new CSoundVariation named
# "boz64_<line>" holding BO1's takes; src/voxev.cpp plays them when BO1's scripts would.
#
# BO1's files (localized_english_iw05.iwd: english/sound/vox/scripted/<map>/plr<n>/*.wav) are not RIFF
# files: a 0x830-byte header (u32 1, u32 frames, u32 ~48000, u32 channels, u32 0x830, ..., u32 format 6 = MS ADPCM,
# u32 at 0x30 = data size) and then MS ADPCM in 262-byte blocks of 512 samples per channel, at 48 kHz.
#
# usage: python tools/voxpatch.py --extract [BO1 folder]
#            converts the takes into data/kino_vox (needs BO1 PC; reads its .iwd files, writes nothing there)
#        python tools/voxpatch.py --extract-events [BO1 folder]
#            the same for EVENTS only (data/kino_vox/events.json)
#        as a library: build_files(src_dz) -> {path: bytes} for dzpatch (tools/mulekick.py calls it)
import json
import math
import operator
import os
import re
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dzpatch  # noqa: E402

CLIPS = os.path.join(ROOT, 'data', 'kino_vox')
MANIFEST = os.path.join(CLIPS, 'manifest.json')
SRC = os.path.join(ROOT, '..', 'backup', 'obb', 'com.activision.boz', 'blackops_etc.dz')
BO1 = r'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops'
CHARS = ['tank', 'nikolai', 'takeo', 'richtofen']  # BO1's plr0..plr3: Dempsey, Nikolai, Takeo, Richtofen
KINO = 'zombie_theater'
# the single lines with variants (spec name hash -> BO1 folder of its takes); src/vox.cpp has the same list
SINGLES = {
    0xb77bc8b8: KINO,                  # kill_explo
    0xf9d67c68: KINO,                  # wpck_crappy (mystery box)
    0x2995494b: KINO,                  # pickup lines: launcher
    0xd88f6a8d: KINO,                  #   machine gun
    0xedf5a161: KINO,                  #   shotgun
    0xea7cd6e0: KINO,                  #   SMG
    0x1e872cd4: 'zombie_cosmodrome',   #   sickle (Ascension's)
}

# lines for events the phone game does not voice: "boz64_<key>" -> (BO1 folder, BO1 event, takes or None for all).
# The same four characters (plr0..3) are in each of these maps. src/voxev.cpp has the same names.
EVENTS = {
    'perk_0': ('zombie_temple', 'perk_deadshot', None),          # perk types as the game numbers them
    'perk_1': (KINO, 'perk_doubletap', None),
    'perk_2': (KINO, 'perk_jugga', None),
    'perk_3': ('zombie_cosmodrome', 'perk_stamin', None),
    'perk_4': ('zombie_cosmodrome', 'perk_phdflopper', None),
    'perk_5': (KINO, 'perk_revive', None),
    'perk_6': (KINO, 'perk_speed', None),
    'perk_7': ('zombie_moon', 'perk_arsenal', None),              # Mule Kick (specialty_additionalprimaryweapon)
    'powerup_0': (KINO, 'powerup_double', None),                  # pickup types as the game numbers them
    'powerup_1': (KINO, 'powerup_insta', None),
    'powerup_2': (KINO, 'powerup_ammo', None),
    'powerup_3': (KINO, 'powerup_nuke', None),
    'powerup_4': (KINO, 'powerup_carp', None),
    'powerup_5': (KINO, 'powerup_firesale', None),
    'nomoney_0': (KINO, 'nomoney', [0]),                          # BO1 forces a take: perks and wall ammo
    'nomoney_1': (KINO, 'nomoney', [1]),                          #   wall guns
    'nomoney_2': (KINO, 'nomoney', [2]),                          #   the mystery box
    'sigh': ('zombie_cosmodrome', 'sigh', None),                  # a 5th perk refused
    'kill_headshot': (KINO, 'kill_headshot', None),
    'kill_insta': (KINO, 'kill_insta', None),
    'kill_ray': (KINO, 'kill_ray', None),
    'kill_hellhound': (KINO, 'kill_hellhound', None),
    'kill_thunder': (KINO, 'kill_thunder', None),
    'wpck_raygun': (KINO, 'wpck_raygun', None),
    'wpck_thunder': (KINO, 'wpck_thunder', None),
}
EVENTS_MANIFEST = os.path.join(CLIPS, 'events.json')
TEMPLATE_VAR = 0xc755b01a  # kill_streak: the new specs copy its first spec (volume, pitch...)


def event_hash(key):
    return djb2('boz64_' + key)


C_DATA, C_SPEC, C_VAR = 0xd5610dab, 0xd569853c, 0x80cb105e
DATA_TAG = 0x430758f0
RATE = 11025
BLOCK = 256
PER_BLOCK = (BLOCK - 4) * 2 + 1  # 505
DATA_TAIL = struct.pack('<IHH', RATE, 2, PER_BLOCK) + bytes([0, 0, 0, 1, 0, 0])  # as the phone's 11025 Hz clips
GAIN = 1.0  # set from the phone's own clips against the same BO1 takes (see --extract's report)


def djb2(s):
    h = 0x1505
    for c in s.lower().encode():
        h = (h * 33 + c) & 0xffffffff
    return h


def bank_path(ch):
    return 'ingame/sound/snd_player_%s.group.bin' % ch


# ---- the phone's bank format ----
# [3d030701][0000][87e08180][u32 n][name\0][1][0xdc3c2177] [u32 size of the rest][u32 section count]
# sections: [class][count][2 flag bytes] records [u32 size incl.]...; then 4 zero bytes
class Bank:
    def __init__(self, d):
        self.d = d
        nl, = struct.unpack_from('<I', d, 10)
        self.head = d[:14 + nl]
        rest, nsec = struct.unpack_from('<II', d, 14 + nl)
        assert 14 + nl + 4 + rest == len(d)
        o = 14 + nl + 8
        self.secs = []
        for _ in range(nsec):
            cls, cnt = struct.unpack_from('<II', d, o)
            flags = d[o + 8:o + 10]
            o += 10
            recs = []
            for _ in range(cnt):
                size, = struct.unpack_from('<I', d, o)
                recs.append(bytearray(d[o:o + size]))
                o += size
            self.secs.append([cls, flags, recs])
        self.tail = d[o:]
        assert self.tail == b'\0\0\0\0', self.tail

    def sec(self, cls):
        return next(s[2] for s in self.secs if s[0] == cls)

    def datas(self):  # hash -> record
        return {struct.unpack_from('<I', r, 8)[0]: r for r in self.sec(C_DATA)}

    def specs(self):  # spec hash -> (data hash, record)
        return {struct.unpack_from('<I', r, 8)[0]: (struct.unpack_from('<I', r, 0x38)[0], r) for r in self.sec(C_SPEC)}

    def variations(self):  # name hash -> (spec hashes, record)
        out = {}
        for r in self.sec(C_VAR):
            n, = struct.unpack_from('<I', r, 0x34)
            out[struct.unpack_from('<I', r, 4)[0]] = (list(struct.unpack_from('<%dI' % n, r, 0x38)), r)
        return out

    def bytes(self):
        body = struct.pack('<I', len(self.secs))
        for cls, flags, recs in self.secs:
            body += struct.pack('<II', cls, len(recs)) + flags + b''.join(recs)
        body += self.tail
        return self.head + struct.pack('<I', len(body)) + body


def data_record(h, adpcm, samples):
    body = struct.pack('<IIII', DATA_TAG, h, len(adpcm), samples) + adpcm + DATA_TAIL
    return bytearray(struct.pack('<I', 4 + len(body)) + body)


def new_variation(bank, name_hash, spec_hash):
    """a CSoundVariation named name_hash holding spec_hash, made from the bank's first variation"""
    recs = bank.sec(C_VAR)
    assert name_hash not in bank.variations()
    v = bytearray(recs[0][:0x30] + struct.pack('<III', 8, 1, spec_hash))
    struct.pack_into('<I', v, 4, name_hash)
    for off, val in ((0, len(v)), (8, len(v) - 12), (20, len(v) - 12)):
        struct.pack_into('<I', v, off, val)
    recs.append(v)


def add_takes(bank, var_hash, clips):
    """clips: [(name, adpcm bytes, samples)] added to the variation var_hash"""
    datas, specs, variations = bank.datas(), bank.specs(), bank.variations()
    lst, vrec = variations[var_hash]
    template = specs[lst[0]][1]
    for name, adpcm, samples in clips:
        h = djb2(name)
        assert h not in datas and h not in specs, name
        bank.sec(C_DATA).append(data_record(h, adpcm, samples))
        spec = bytearray(template)
        struct.pack_into('<I', spec, 8, h)
        struct.pack_into('<I', spec, 0x38, h)
        bank.sec(C_SPEC).append(spec)
        lst.append(h)
    # the variation: [size][name][size - 12]["LFER"][class][size - 12][...][list size at 0x30][count][spec hashes]
    new = vrec[:0x30] + struct.pack('<II', 4 + 4 * len(lst), len(lst)) + struct.pack('<%dI' % len(lst), *lst)
    assert struct.unpack_from('<I', vrec, 8)[0] == struct.unpack_from('<I', vrec, 20)[0] == len(vrec) - 12
    struct.pack_into('<I', new, 0, len(new))
    struct.pack_into('<I', new, 8, len(new) - 12)
    struct.pack_into('<I', new, 20, len(new) - 12)
    recs = bank.sec(C_VAR)
    recs[next(i for i, r in enumerate(recs) if r is vrec)] = bytearray(new)


def add_event(bank, var_hash, clips):
    """a new CSoundVariation var_hash holding clips [(name, adpcm bytes, samples)]; clips already in the bank (another
    line's) are shared"""
    datas, specs, variations = bank.datas(), bank.specs(), bank.variations()
    assert var_hash not in variations
    template = specs[variations[TEMPLATE_VAR][0][0]][1]
    lst = []
    for name, adpcm, samples in clips:
        h = djb2(name)
        if h not in datas:
            bank.sec(C_DATA).append(data_record(h, adpcm, samples))
            datas[h] = None
        if h not in specs:
            spec = bytearray(template)
            struct.pack_into('<I', spec, 8, h)
            struct.pack_into('<I', spec, 0x38, h)
            bank.sec(C_SPEC).append(spec)
            specs[h] = (h, spec)
        lst.append(h)
    recs = bank.sec(C_VAR)
    v = bytearray(recs[0][:0x30] + struct.pack('<II', 4 + 4 * len(lst), len(lst)) + struct.pack('<%dI' % len(lst), *lst))
    struct.pack_into('<I', v, 4, var_hash)
    for off, val in ((0, len(v)), (8, len(v) - 12), (20, len(v) - 12)):
        struct.pack_into('<I', v, off, val)
    recs.append(v)


def read_clip(name):
    raw = open(os.path.join(CLIPS, name + '.ima'), 'rb').read()
    samples, = struct.unpack_from('<I', raw, 0)
    return name, raw[4:], samples


def build_files(src):
    """the four banks with the extra takes from data/kino_vox"""
    man = json.load(open(MANIFEST))
    events = json.load(open(EVENTS_MANIFEST)) if os.path.exists(EVENTS_MANIFEST) else {}
    files = {}
    for ch in CHARS:
        bank = Bank(dzpatch.read_file(src, bank_path(ch)))
        for e in man[ch]:
            var = int(e['var'], 16)
            if e['single']:
                new_variation(bank, var, var)
            clips = []
            for name in e['clips']:
                raw = open(os.path.join(CLIPS, name + '.ima'), 'rb').read()
                samples, = struct.unpack_from('<I', raw, 0)
                clips.append((name, raw[4:], samples))
            add_takes(bank, var, clips)
        for key, names in sorted(events.get(ch, {}).items()):
            if names:
                add_event(bank, event_hash(key), [read_clip(n) for n in names])
        files[bank_path(ch)] = bank.bytes()
    return files


def inputs():
    """files build_files depends on, for tools/mulekick.py's up-to-date check"""
    out = [os.path.join(HERE, 'voxpatch.py'), MANIFEST]
    man = json.load(open(MANIFEST))
    out += [os.path.join(CLIPS, n + '.ima') for ch in CHARS for e in man[ch] for n in e['clips']]
    if os.path.exists(EVENTS_MANIFEST):
        ev = json.load(open(EVENTS_MANIFEST))
        out += [EVENTS_MANIFEST] + [os.path.join(CLIPS, n + '.ima') for ch in ev for names in ev[ch].values()
                                    for n in names]
    return out


# ---- audio ----
MS_C1 = [256, 512, 0, 192, 240, 460, 392]
MS_C2 = [0, -256, 0, 64, 0, -208, -232]
MS_AD = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230]


def bo1_decode(raw):
    """BO1 PC sound file -> (mono int16 samples, rate)"""
    ver, frames, rate, chans = struct.unpack_from('<IIII', raw, 0)
    fmt, = struct.unpack_from('<I', raw, 0x1c)
    size, = struct.unpack_from('<I', raw, 0x30)
    assert ver == 1 and fmt == 6 and chans == 1, (ver, fmt, chans)
    d = raw[len(raw) - size:]
    out = []
    for b in range(0, len(d) - 6, 262):
        p, delta, s1, s2 = struct.unpack_from('<Bhhh', d, b)
        c1, c2 = MS_C1[p], MS_C2[p]
        out += (s2, s1)
        for byte in d[b + 7:b + 262]:
            for nib in (byte >> 4, byte & 15):
                v = ((s1 * c1 + s2 * c2) >> 8) + (nib - 16 if nib & 8 else nib) * delta
                v = -32768 if v < -32768 else 32767 if v > 32767 else v
                delta = max(16, (MS_AD[nib] * delta) >> 8)
                s2, s1 = s1, v
                out.append(v)
    return out[:frames], 48000  # the header's rate field wanders around 48000 (47975..48146); the audio is 48 kHz


def resample(x, sr_in, sr_out=RATE, half=48, phases=256):
    """windowed-sinc (Blackman) low-pass and resample, cutoff 0.45 * sr_out"""
    r = sr_in / sr_out
    fc = 0.45 * sr_out / sr_in
    table = []
    for p in range(phases):
        f = p / phases
        k = []
        for j in range(-half + 1, half + 1):
            t = j - f
            s = 2 * fc * (math.sin(2 * math.pi * fc * t) / (2 * math.pi * fc * t) if t else 1.0)
            w = 0.42 + 0.5 * math.cos(math.pi * t / half) + 0.08 * math.cos(2 * math.pi * t / half)
            k.append(s * w)
        sk = sum(k)
        table.append([v / sk for v in k])
    xp = [0] * half + list(x) + [0] * (half + 2)
    out = []
    n_out = int(len(x) / r)
    for n in range(n_out):
        c = n * r
        i = int(c)
        p = int((c - i) * phases + 0.5)
        if p == phases:
            i, p = i + 1, 0
        out.append(sum(map(operator.mul, xp[i + 1:i + 1 + 2 * half], table[p])))
    return out


IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
            97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
            724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
            4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
            18500, 20350, 22385, 24623, 27086, 29794, 32767]
IMA_IDX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def _ima_step(code, pred, idx):
    s = IMA_STEP[idx]
    diff = s >> 3
    if code & 4:
        diff += s
    if code & 2:
        diff += s >> 1
    if code & 1:
        diff += s >> 2
    pred = pred - diff if code & 8 else pred + diff
    return max(-32768, min(32767, pred)), max(0, min(88, idx + IMA_IDX[code]))


def ima_encode(x):
    """mono int16 -> (IMA ADPCM bytes, samples): 256-byte blocks [pred s16][index u8][0] + 252 bytes, low nibble
    first; the end is padded with silence to a whole block"""
    n = (len(x) + PER_BLOCK - 1) // PER_BLOCK * PER_BLOCK
    x = list(x) + [0] * (n - len(x))
    out = bytearray()
    idx = 0
    for b in range(0, n, PER_BLOCK):
        pred = x[b]
        out += struct.pack('<hBB', pred, idx, 0)
        codes = []
        for s in x[b + 1:b + PER_BLOCK]:
            st = IMA_STEP[idx]
            diff = s - pred
            code = 0
            if diff < 0:
                code, diff = 8, -diff
            if diff >= st:
                code |= 4
                diff -= st
            if diff >= st >> 1:
                code |= 2
                diff -= st >> 1
            if diff >= st >> 2:
                code |= 1
            pred, idx = _ima_step(code, pred, idx)
            codes.append(code)
        for i in range(0, len(codes), 2):
            out.append(codes[i] | codes[i + 1] << 4)
    return bytes(out), n


def ima_decode(d, samples):
    out = []
    for b in range(0, len(d), BLOCK):
        pred, idx = struct.unpack_from('<hB', d, b)
        out.append(pred)
        for byte in d[b + 4:b + BLOCK]:
            for code in (byte & 15, byte >> 4):
                pred, idx = _ima_step(code, pred, idx)
                out.append(pred)
    return out[:samples]


def convert(raw, gain=GAIN):
    """BO1 file -> (IMA ADPCM bytes, samples) at 11025 Hz, quiet ends trimmed (40 ms kept)"""
    x, sr = bo1_decode(raw)
    y = [max(-32768, min(32767, int(round(v * gain)))) for v in resample(x, sr)]
    loud = [i for i, v in enumerate(y) if abs(v) > 300]
    if loud:
        keep = RATE * 40 // 1000
        y = y[max(0, loud[0] - keep):loud[-1] + keep]
    return ima_encode(y)


# ---- --extract ----
def bo1_index(folder):
    """(map folder, BO1 voice file name) -> (.iwd, member); the last .iwd in name order wins, as in the game"""
    main = os.path.join(folder, 'main')
    maps = {KINO} | set(SINGLES.values()) | {f for f, _, _ in EVENTS.values()}
    idx = {}
    for iwd in sorted(f for f in os.listdir(main) if f.lower().endswith('.iwd')):
        with zipfile.ZipFile(os.path.join(main, iwd)) as z:
            for n in z.namelist():
                m = re.search(r'/sound/vox/scripted/([^/]+)/plr\d/([^/]+)\.wav$', n.lower())
                if m and m.group(1) in maps:
                    idx[(m.group(1), m.group(2))] = (os.path.join(main, iwd), n)
    return idx


def plan(src, idx):
    """{char: [{var, event, folder, single, clips}]} (the new takes) and (phone clip, BO1 take) pairs"""
    takes = {}
    for folder, name in idx:
        m = re.match(r'vox_plr_(\d)_(.+)_(\d+)$', name)
        if m:
            takes.setdefault((folder, int(m.group(1)), m.group(2)), set()).add(int(m.group(3)))
    names = {djb2('vox_plr_%d_%s_%d' % (n, e, t)): (n, e, t) for (f, n, e), ts in takes.items() for t in ts}
    out, pairs = {}, []
    for n, ch in enumerate(CHARS):
        bank = Bank(dzpatch.read_file(src, bank_path(ch)))
        specs = bank.specs()
        clips_in_bank = {d for d, _ in specs.values()}
        out[ch] = []

        def add(var, folder, single, lst):
            known = [names[specs[s][0]] for s in lst if specs[s][0] in names]
            events = {e for _, e, _ in known}
            if len(events) != 1:
                return  # takes from somewhere else (not from these BO1 folders): left alone
            ev = events.pop()
            if folder == KINO:
                pairs.extend((specs[s][0], names[specs[s][0]]) for s in lst if specs[s][0] in names)
            new = ['vox_plr_%d_%s_%d' % (n, ev, t) for t in sorted(takes.get((folder, n, ev), ()))
                   if djb2('vox_plr_%d_%s_%d' % (n, ev, t)) not in clips_in_bank]
            if new:
                out[ch].append({'var': '%08x' % var, 'event': ev, 'folder': folder, 'single': single, 'clips': new})

        for vh, (lst, _) in bank.variations().items():
            add(vh, KINO, False, lst)
        for sh, folder in SINGLES.items():
            add(sh, folder, True, [sh])
    return out, pairs


def extract(folder):
    idx = bo1_index(folder)
    assert idx, 'no Kino voice files in ' + folder
    man, pairs = plan(SRC, idx)

    def raw(name, folder=KINO):
        iwd, member = idx[(folder, name.lower())]
        with zipfile.ZipFile(iwd) as z:
            return z.read(member)

    # loudness: the phone's clips against the same BO1 takes
    banks = {ch: Bank(dzpatch.read_file(SRC, bank_path(ch))) for ch in CHARS}
    ratios = []
    for dh, (n, e, t) in pairs[::4]:
        r = banks[CHARS[n]].datas()[dh]
        buf, samples = struct.unpack_from('<II', r, 12)
        phone = ima_decode(bytes(r[20:20 + buf]), samples)
        x, sr = bo1_decode(raw('vox_plr_%d_%s_%d' % (n, e, t)))
        pc = resample(x, sr)
        rms = lambda v: math.sqrt(sum(a * a for a in v) / max(1, len(v)))  # noqa: E731
        ratios.append(rms(phone) / max(1.0, rms(pc)))
    ratios.sort()
    print('phone / BO1 loudness over %d takes: median %.3f (min %.3f, max %.3f); GAIN is %.3f' % (
        len(ratios), ratios[len(ratios) // 2], ratios[0], ratios[-1], GAIN))

    os.makedirs(CLIPS, exist_ok=True)
    total = 0
    for ch in CHARS:
        for e in man[ch]:
            for name in e['clips']:
                adpcm, samples = convert(raw(name, e['folder']))
                open(os.path.join(CLIPS, name + '.ima'), 'wb').write(struct.pack('<I', samples) + adpcm)
                total += len(adpcm)
            print('%-9s %s %-16s %-17s +%d' % (ch, e['var'], e['event'], e['folder'], len(e['clips'])))
    with open(MANIFEST, 'w') as f:
        json.dump(man, f, indent=1)
    print('%d clips, %d bytes, in %s' % (sum(len(e['clips']) for c in man.values() for e in c), total, CLIPS))


def extract_events(folder):
    idx = bo1_index(folder)
    man, total = {}, 0
    os.makedirs(CLIPS, exist_ok=True)
    for n, ch in enumerate(CHARS):
        man[ch] = {}
        for key, (mapf, ev, want) in EVENTS.items():
            pat = re.compile(r'vox_plr_%d_%s_(\d+)$' % (n, re.escape(ev)))
            takes = sorted(int(m.group(1)) for (f, name) in idx if f == mapf for m in [pat.match(name)] if m)
            if want is not None:
                takes = [t for t in takes if t in want]
            names = []
            for t in takes:
                name = 'vox_plr_%d_%s_%d' % (n, ev, t)
                iwd, member = idx[(mapf, name)]
                with zipfile.ZipFile(iwd) as z:
                    adpcm, samples = convert(z.read(member))
                open(os.path.join(CLIPS, name + '.ima'), 'wb').write(struct.pack('<I', samples) + adpcm)
                total += len(adpcm)
                names.append(name)
            man[ch][key] = names
            print('%-9s boz64_%-15s %-17s %s' % (ch, key, mapf, ' '.join(str(t) for t in takes) or 'NONE'))
            sys.stdout.flush()
    with open(EVENTS_MANIFEST, 'w') as f:
        json.dump(man, f, indent=1)
    print('%d clips, %d bytes' % (sum(len(v) for c in man.values() for v in c.values()), total))


if __name__ == '__main__':
    if '--extract-events' in sys.argv:
        rest = [a for a in sys.argv[1:] if a != '--extract-events']
        extract_events(rest[0] if rest else BO1)
    elif '--extract' in sys.argv:
        rest = [a for a in sys.argv[1:] if a != '--extract']
        extract(rest[0] if rest else BO1)
    else:
        print(__doc__ if __doc__ else 'usage: python tools/voxpatch.py --extract [BO1 folder]')
