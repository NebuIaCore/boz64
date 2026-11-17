# CIwAnim (Marmalade IwAnim), from Black Ops' CIwAnim Serialise (CIwAnim__v5 0x0245c670). The same container in
# World at War's data (version 3.6.4) and Black Ops' (3.7.1):
#   u32 skel (CIwAnimSkel hash), u32 nbones (data version > 3.6.3), u32[ceil(nbones / 32)] animated-bone mask,
#   u32 nkeyframes, u32 size + size bytes: the keyframes, packed as the engine keeps them in memory,
#   i32 (+0x48), i32 (+0x50, version > 3.6.1), u32 ref (+0x4c)
# A keyframe in the blob: a 0x90-byte header (u16 nbones at +4, u8 flags at +6), then per bone 8 bytes (flags & 1) or
# 14 bytes, then nbones more bytes unless flags & 4; the next keyframe starts 4-aligned.
import struct

KF_HEADER = 0x90


def keyframe_size(blob, o):
    n = struct.unpack_from('<H', blob, o + 4)[0]
    flags = blob[o + 6]
    size = KF_HEADER + (n * 8 if flags & 1 else n * 14) + (0 if flags & 4 else n)
    return (size + 3) & ~3, n, flags


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
        self.nkeys = take('I')[0]
        size = take('I')[0]
        self.blob = bytearray(body[p:p + size])
        p += size
        self.x48, self.x50, self.ref4c = take('iiI')
        assert p == len(body), (p, len(body))
        # where each keyframe starts
        self.keys = []
        o = 0
        for _ in range(self.nkeys):
            ks, n, flags = keyframe_size(self.blob, o)
            self.keys.append((o, n, flags))
            o += ks
        self.blob_used = o

    def write(self):
        return (struct.pack('<II', self.skel, self.nbones) + struct.pack('<%dI' % len(self.mask), *self.mask) +
                struct.pack('<II', self.nkeys, len(self.blob)) + bytes(self.blob) +
                struct.pack('<iiI', self.x48, self.x50, self.ref4c))
