# PVRTC 4 bpp (GL_COMPRESSED_RGB(A)_PVRTC_4BPPV1_IMG, 0x8C00 / 0x8C02) decoder: World at War Zombies (iOS) keeps its
# textures in it, Black Ops Zombies (Android) in ETC1, and neither the phones nor the PC renderer here take PVRTC.
# Follows Imagination's reference decoder (PVRTDecompress):
#   blocks of 4 x 4 pixels, 8 bytes each (u32 modulation bits, u32 colour data), stored in Morton order;
#   colour data: bit 0 = punch-through mode, bits 1-15 colour A (RGB 554 opaque, or ARGB 3443), bits 16-31 colour B
#   (RGB 555, or ARGB 3444); a pixel's A and B are the bilinear blend of the four nearest blocks' (block centres at
#   +2, +2), then mixed by its 2-bit modulation value (0, 3/8, 5/8, 1; punch-through mode: 0, 1/2, 1/2 + transparent, 1).
import struct


def _twiddle(x, y, bw, bh):
    """block (x, y) -> its index in the Morton-ordered data (y bits first, the larger side's extra bits on top)"""
    t, shift, bit = 0, 0, 1
    mn = min(bw, bh)
    while bit < mn:
        if y & bit:
            t |= 1 << (2 * shift)
        if x & bit:
            t |= 1 << (2 * shift + 1)
        bit <<= 1
        shift += 1
    rest = (x if bw > bh else y) >> shift
    return t | (rest << (2 * shift))


def _colour_a(cd):
    if cd & 0x8000:
        return ((cd >> 10) & 0x1f, (cd >> 5) & 0x1f, (cd & 0x1e) | ((cd & 0x1e) >> 4), 0xf)
    return (((cd & 0xf00) >> 7) | ((cd & 0xf00) >> 11), ((cd & 0xf0) >> 3) | ((cd & 0xf0) >> 7),
            ((cd & 0xe) << 1) | ((cd & 0xe) >> 2), (cd & 0x7000) >> 11)


def _colour_b(cd):
    if cd & 0x80000000:
        return ((cd >> 26) & 0x1f, (cd >> 21) & 0x1f, (cd >> 16) & 0x1f, 0xf)
    return (((cd & 0xf000000) >> 23) | ((cd & 0xf000000) >> 27), ((cd & 0xf00000) >> 19) | ((cd & 0xf00000) >> 23),
            ((cd & 0xf0000) >> 15) | ((cd & 0xf0000) >> 19), (cd & 0x70000000) >> 27)


MOD_NORMAL = (0, 3, 5, 8)
MOD_PUNCH = (0, 4, 4, 8)


def decode(data, w, h):
    """PVRTC 4 bpp level 0 -> RGBA bytes (w x h, rows top to bottom as stored)"""
    bw, bh = max(w // 4, 2), max(h // 4, 2)
    ca, cb, mods = [], [], []
    for by in range(bh):
        for bx in range(bw):
            mod, cd = struct.unpack_from('<II', data, _twiddle(bx, by, bw, bh) * 8)
            ca.append(_colour_a(cd))
            cb.append(_colour_b(cd))
            mods.append((mod, MOD_PUNCH if cd & 1 else MOD_NORMAL, cd & 1))
    out = bytearray(w * h * 4)
    o = 0
    for py in range(h):
        y = py - 2
        r0 = ((y >> 2) % bh) * bw
        r1 = (((y >> 2) + 1) % bh) * bw
        fy = y & 3
        mrow = (py >> 2) * bw
        mshift_y = (py & 3) * 4
        for px in range(w):
            x = px - 2
            c0 = (x >> 2) % bw
            c1 = (c0 + 1) % bw
            fx = x & 3
            w00, w10, w01, w11 = (4 - fx) * (4 - fy), fx * (4 - fy), (4 - fx) * fy, fx * fy
            pa, qa, ra, sa = ca[r0 + c0], ca[r0 + c1], ca[r1 + c0], ca[r1 + c1]
            pb, qb, rb, sb = cb[r0 + c0], cb[r0 + c1], cb[r1 + c0], cb[r1 + c1]
            bits, table, punch = mods[mrow + (px >> 2)]
            m2 = (bits >> (2 * (mshift_y + (px & 3)))) & 3
            m = table[m2]
            for k in range(3):
                va = w00 * pa[k] + w10 * qa[k] + w01 * ra[k] + w11 * sa[k]
                vb = w00 * pb[k] + w10 * qb[k] + w01 * rb[k] + w11 * sb[k]
                a8 = (va >> 6) + (va >> 1)
                b8 = (vb >> 6) + (vb >> 1)
                out[o + k] = (a8 * (8 - m) + b8 * m) >> 3
            if punch and m2 == 2:
                out[o + 3] = 0
            else:
                va = w00 * pa[3] + w10 * qa[3] + w01 * ra[3] + w11 * sa[3]
                vb = w00 * pb[3] + w10 * qb[3] + w01 * rb[3] + w11 * sb[3]
                a8 = (va >> 4) + va
                b8 = (vb >> 4) + vb
                out[o + 3] = (a8 * (8 - m) + b8 * m) >> 3
            o += 4
    return bytes(out)
