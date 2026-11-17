# World at War Zombies (iOS) CIwAnim, from the game's own code (kiwi.app/kiwi):
#   CIwAnim vtable 0x2b4bc4, Serialise 0x202b6c; CIwAnimKeyFrame vtable 0x2a8818, Serialise 0x163260;
#   rotation read 0x21c564 (data version >= 3.4.10: 0x21c726).
# anim:     u32 skel, u32 nbones (version >= 3.6.4), u32[ceil(nbones/32)] mask, u32 nkeyframes, each keyframe written
#           with its class hash ('CIwAnimKeyFrame'), then i32 (+0x28), i32 (+0x30, version >= 3.6.2), u32 ref (+0x2c)
# keyframe: u32 class hash, u32 hash, i32 time (+0xc), u8 flags (+0x10; bit 0 = rotations only),
#           3 x i16 (+0x12), u8 mask words (+0x11), u32[mask words] bone mask, u32 nbones, u8 (shared index table),
#           positions unless flags & 1: 6 bytes + 6 per bone (written by the descriptor-driven array serialiser
#           0xcb580, descriptor 0x285fd8), then a rotation per bone: u32 "smallest three" quaternion:
#           bits 30-31 = which component is largest (0 x, 1 y, 2 z, 3 w), three 10-bit fields (0-1023 -> -2897..2897,
#           1/4096 units) for the others in x, y, z, w order, the largest rebuilt as sqrt(1 - sum of squares).
import math
import struct

KEYFRAME_CLASS = 0x4b9bb1a1  # djb2('CIwAnimKeyFrame')


def unpack_quat(v):
    """u32 -> (w, x, y, z) int16, 4096 = 1.0, as the engine keeps it"""
    c = [((v >> s) & 0x3ff) * 0x16a2 >> 10 for s in (0, 10, 20)]
    a, b, d = [x - 0xb50 - 1 for x in c]  # 0xfffff4b0 - 1 = -0xb51
    big = int(round(math.sqrt(max(0.0, 1.0 - (a * a + b * b + d * d) / 4096.0 ** 2)) * 4096))
    i = v >> 30
    if i == 0:   # x largest: the others are y, z, w
        return (d, big, a, b)
    if i == 1:   # y largest: x, z, w
        return (d, a, big, b)
    if i == 2:   # z largest: x, y, w
        return (d, a, b, big)
    return (big, a, b, d)  # w largest: x, y, z


class Keyframe:
    pass


class Anim:
    def __init__(self, body):
        p = 0

        def take(fmt):
            nonlocal p
            v = struct.unpack_from('<' + fmt, body, p)
            p += struct.calcsize('<' + fmt)
            return v

        self.skel, self.nbones = take('II')
        self.mask = list(take('%dI' % ((self.nbones + 31) // 32)))
        self.keys = []
        for _ in range(take('I')[0]):
            k = Keyframe()
            cls, k.hash, k.time, k.flags = take('IIiB')
            assert cls == KEYFRAME_CLASS, hex(cls)
            k.root = take('3h')
            k.mask = list(take('%dI' % take('B')[0]))
            k.n = take('I')[0]
            k.shared = take('B')[0]
            if k.flags & 1:
                k.pos_head, k.pos = None, None
            else:
                k.pos_head = take('3h')
                k.pos = [take('3h') for _ in range(k.n)]
            k.rot_packed = list(take('%dI' % k.n))
            k.rot = [unpack_quat(v) for v in k.rot_packed]
            self.keys.append(k)
        self.x28, self.x30, self.ref2c = take('iiI')
        assert p == len(body), (p, len(body))


# World at War arm/gun animation -> Black Ops CIwAnim (the record body iwanim.Anim reads).
# WaW keyframes are sparse (a key may carry only some bones, and keys are far apart); Black Ops' own converted copies of
# the same animations carry every bone in every key, about 30 keys a second, with each bone's rotation kept on one side
# (no sign flip from key to key: a player that interpolates the two quaternions as they are turns the bone the long way
# round between such keys, which World at War's sparse keys do often). So the animation is resampled: each bone's track
# is built from the keys that have it, then sampled at RATE keys a second and at every original key time (sharp moves
# stay sharp), positions linearly, rotations by shortest-path slerp, each rotation then put on the same side as the
# bone's previous one. Positions are scaled to Black Ops' units (the arms skeleton's bind positions are 6.4 x World at
# War's). Black Ops keyframe in its blob: i32 time, u16 bones, u8 flags (4: every bone, positions and rotations; 5:
# every bone, rotations only), then 0x89 bytes the engine does not read here (0 / 0xff as Black Ops' own keys have
# them), then per bone 3 x i16 position + (w, x, y, z) i16 rotation, or the rotation only.

POS_SCALE = 6.4
X50 = 16384  # what Black Ops' converted animations have (World at War's say 40960)
RATE = 30    # keys a second (time units: 4096 a second)


def _decode_positions(key):
    """WaW keyframe positions: per component a mean, then one value per bone (the descriptor-driven array writer)"""
    if key.pos_head is None:
        return None
    flat = list(key.pos_head) + [c for t in key.pos for c in t]
    n = key.n
    comps = []
    for c in range(3):
        seg = flat[c * (n + 1):(c + 1) * (n + 1)]
        comps.append([((seg[0] + v + 0x8000) & 0xffff) - 0x8000 for v in seg[1:]])
    return list(zip(*comps))


def _bones(mask, nbones):
    return [i for i in range(nbones) if mask[i // 32] >> (i % 32) & 1]


def _unit(q):
    ln = math.sqrt(sum(x * x for x in q)) or 1.0
    return tuple(x / ln for x in q)


def _slerp(a, b, t):
    """unit quaternions a, b (floats), shortest path"""
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = tuple(-x for x in b), -d
    if d > 0.9995:
        return _unit(tuple(x + (y - x) * t for x, y in zip(a, b)))
    th = math.acos(min(1.0, d))
    s0, s1 = math.sin((1 - t) * th) / math.sin(th), math.sin(t * th) / math.sin(th)
    return tuple(s0 * x + s1 * y for x, y in zip(a, b))


def _track_at(track, t):
    """track: [(time, pos or None, unit quaternion)] sorted -> (pos, rot) at time t"""
    if t <= track[0][0]:
        return track[0][1], track[0][2]
    if t >= track[-1][0]:
        return track[-1][1], track[-1][2]
    lo, hi = 0, len(track) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if track[mid][0] <= t:
            lo = mid
        else:
            hi = mid
    (ta, pa, ra), (tb, pb, rb) = track[lo], track[hi]
    f = (t - ta) / (tb - ta) if tb != ta else 0.0
    p = tuple(x + (y - x) * f for x, y in zip(pa, pb)) if pa is not None and pb is not None else (pa or pb)
    return p, _slerp(ra, rb, f)


def convert(waw, pos_scale=POS_SCALE, rate=RATE):
    """waw: waw_anim.Anim -> Black Ops CIwAnim record body (bytes)"""
    bones = _bones(waw.mask, waw.nbones)
    has_pos = any(k.pos_head is not None for k in waw.keys)
    tracks = {b: [] for b in bones}
    for k in waw.keys:
        kb = _bones(k.mask, len(k.mask) * 32)[:k.n]
        pos = _decode_positions(k)
        for i, b in enumerate(kb):
            if b in tracks and (pos is not None or not has_pos):
                tracks[b].append((k.time, pos[i] if pos else None, _unit(tuple(c / 4096.0 for c in k.rot[i]))))
    for b in bones:
        if not tracks[b]:  # a bone the mask names but no key has: the first key's rotation-only entry if any
            k = next((k for k in waw.keys if b in _bones(k.mask, len(k.mask) * 32)[:k.n]), None)
            i = _bones(k.mask, len(k.mask) * 32)[:k.n].index(b) if k else None
            tracks[b] = [(0, (0, 0, 0) if has_pos else None,
                          _unit(tuple(c / 4096.0 for c in k.rot[i])) if k else (1.0, 0.0, 0.0, 0.0))]
        tracks[b].sort(key=lambda e: e[0])
    t0, t1 = waw.keys[0].time, waw.keys[-1].time
    step = 4096.0 / rate
    times = {k.time for k in waw.keys}
    n = int((t1 - t0) / step)
    times.update(int(round(t0 + i * step)) for i in range(1, n + 1) if t0 + i * step < t1)
    times = sorted(times)
    blob = bytearray()
    last = {}
    for t in times:
        flags = 4 if has_pos else 5
        hdr = bytearray(b'\xff' * 0x90)
        struct.pack_into('<iHBB', hdr, 0, t, len(bones), flags, 0)
        hdr[8:14] = bytes(6)
        blob += hdr
        for b in bones:
            p, r = _track_at(tracks[b], t)
            if b in last and sum(x * y for x, y in zip(r, last[b])) < 0:
                r = tuple(-x for x in r)
            last[b] = r
            if has_pos:
                p = p or (0, 0, 0)
                blob += struct.pack('<3h', *[max(-32768, min(32767, int(round(c * pos_scale)))) for c in p])
            blob += struct.pack('<4h', *[max(-32768, min(32767, int(round(c * 4096)))) for c in r])
        while len(blob) % 4:
            blob += b'\0'
    mask_words = [0] * ((waw.nbones + 31) // 32)
    for b in bones:
        mask_words[b // 32] |= 1 << (b % 32)
    return (struct.pack('<II', waw.skel, waw.nbones) + struct.pack('<%dI' % len(mask_words), *mask_words) +
            struct.pack('<II', len(times), len(blob)) + bytes(blob) + struct.pack('<iiI', waw.x28, X50, waw.ref2c))
