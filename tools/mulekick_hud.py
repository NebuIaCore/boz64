# Mule Kick's HUD icon and purchase sting (tools/mulekick.py builds them into the game data; src/mulekick.cpp shows
# the icon).
#
# The icon: the game shows a perk's icon by creating the IwUI element resource "Perk_<name>_Image" (a CIwUIImage in
# ingame/ingame.group.bin) and adding it to the HUD's perk row (CPerkManager::SetPerkIcon). Each one shows a 57x67
# part (uv0..uv1, in pixels) of the hud_icons texture (1024x1024 RGBA4444). This adds "Perk_Mule_Kick_Image", a copy
# of Double Tap's element showing a free part of hud_icons, where it draws a badge in the phone's style: Double Tap's
# badge with its bullets painted out, turned the dark green of BO1's Mule Kick badge, with the emblem of BO1's Mule
# Kick (three stacked M1911s, from the machine's logo, data/mulekick/logo.png) in the cream of the phone's emblems.
#
# The sting: BO1 PC's mus_mulekick_sting.wav (iw_37.iwd; stereo MS ADPCM at 48 kHz, in BO1's own header), encoded like
# the other perks' stings (MP3, 16 kHz mono, 128 kbps CBR, no tags): data/mulekick/mus_mulekick_sting.mp3 (9.6 s,
# mean -14.9 dB; the phone's own stings are -14.0 to -15.7). It is an APK asset next to the other perks' stings
# (assets/blackops-music), which the engine plays from the APK (tools/package.py adds it; for the Windows version
# tools/mulekick.py writes a copy of the original APK with it added).
#
# usage: python tools/mulekick_hud.py --extract [BO1 folder]   converts the sting (needs BO1 PC, read only, and ffmpeg)
#        python tools/mulekick_hud.py --preview OUT.png         the badge, 6x
import os
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dzpatch  # noqa: E402
import iwgfx as gfx  # noqa: E402
import pngio  # noqa: E402

ASSETS = os.path.join(ROOT, 'data', 'mulekick')
LOGO = os.path.join(ASSETS, 'logo.png')
STING = os.path.join(ASSETS, 'mus_mulekick_sting.mp3')
STING_ASSET = 'assets/blackops-music/mus_mulekick_sting.mp3'
STING_PATH = 'blackops-music/mus_mulekick_sting.mp3'  # as the game names it (CPerk jingle)
# the other perks' stings: MP3, 16 kHz mono, 128 kbps constant bit rate, no ID3 tag or Xing header
STING_FFMPEG = ['-map_metadata', '-1', '-ar', '16000', '-ac', '1', '-c:a', 'libmp3lame', '-b:a', '128k',
                '-id3v2_version', '0', '-write_xing', '0']
BO1 = r'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops'
BO1_STING = ('iw_37.iwd', 'sound/mus/zombie/perksacola/mus_mulekick_sting.wav')

INGAME = 'ingame/ingame.group.bin'
C_TEXTURE, C_UIELEMENT, C_UIIMAGE = gfx.djb2('CIwTexture'), gfx.djb2('CIwUIElement'), gfx.djb2('CIwUIImage')
HUD_ICONS = gfx.djb2('hud_icons')
TEMPLATE = gfx.djb2('Perk_Double_Tap_Image')
ICON = gfx.djb2('Perk_Mule_Kick_Image')
P_UV0, P_UV1, T_SVEC2 = gfx.djb2('uv0'), gfx.djb2('uv1'), gfx.djb2('CIwSVec2')
W, H = 57, 67                      # a badge
DOUBLE_TAP_AT = (114, 522)         # Double Tap's badge in hud_icons
SLOT = None                        # where Mule Kick's goes: the first free part found (see free_slot)
GREEN = (44, 84, 46)               # the badge's mean colour (BO1's Mule Kick badge is dark green)
CREAM = (214, 198, 160)            # the phone's emblems


# ---- hud_icons: RGBA4444 (CIwImage format 5), R in the top 4 bits ----
def rgba4444_to_px(texels, w, h):
    v = struct.unpack('<%dH' % (w * h), texels[:w * h * 2])
    return [((p >> 12) * 17, (p >> 8 & 15) * 17, (p >> 4 & 15) * 17, (p & 15) * 17) for p in v]


def px_to_rgba4444(px):
    return struct.pack('<%dH' % len(px), *[(r * 15 + 127) // 255 << 12 | (g * 15 + 127) // 255 << 8 |
                                           (b * 15 + 127) // 255 << 4 | (a * 15 + 127) // 255
                                           for r, g, b, a in px])


def free_slot(px, w, h, margin=3):
    """the first (x, y) where a badge and a transparent margin around it fit"""
    sa = [[0] * (w + 1) for _ in range(h + 1)]
    for y in range(h):
        row = 0
        for x in range(w):
            row += px[y * w + x][3] != 0
            sa[y + 1][x + 1] = sa[y][x + 1] + row
    bw, bh = W + 2 * margin, H + 2 * margin
    for y in range(0, h - bh):
        for x in range(0, w - bw):
            if sa[y + bh][x + bw] - sa[y][x + bw] - sa[y + bh][x] + sa[y][x] == 0:
                return x + margin, y + margin
    raise ValueError('no room for the badge in hud_icons')


# ---- the badge ----
def lum(c):
    return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]


def emblem_mask():
    """BO1's three M1911s from the machine logo: (w, h, coverage 0..1), white = 1, its green shield = 0"""
    lw, lh, lg = pngio.read(LOGO)
    green = lum((26, 70, 40))
    white = lum((214, 214, 204))
    x0, x1, y0, y1 = 112, 194, 120, 196  # inside the logo's shield border (the V bottom is checked below)
    cov = {}
    for y in range(y0, y1):
        for x in range(x0, x1):
            if x <= 113 + 1.76 * (y - 175) or x >= 191 - 1.7 * (y - 175):  # the shield's white border
                continue
            c = lg[(y * lw + x) * 4:(y * lw + x) * 4 + 3]
            e = (lum(c) - green) / (white - green)
            if e > 0.12:
                cov[(x, y)] = min(1.0, e)
    xs = [x for x, _ in cov]
    ys = [y for _, y in cov]
    bx0, bx1, by0, by1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    return bx0, by0, bx1 - bx0, by1 - by0, cov


def scaled_emblem(tw, mask=None):
    """an emblem mask (default Mule Kick's, emblem_mask()) scaled to tw pixels wide (area average): (w, h, coverage
    list)"""
    bx, by, bw, bh, cov = mask or emblem_mask()
    s = bw / tw
    th = max(1, round(bh / s))
    out = []
    for y in range(th):
        for x in range(tw):
            fx0, fx1, fy0, fy1 = bx + x * s, bx + (x + 1) * s, by + y * s, by + (y + 1) * s
            tot = area = 0.0
            for sy in range(int(fy0), int(fy1) + 1):
                for sx in range(int(fx0), int(fx1) + 1):
                    ox = min(fx1, sx + 1) - max(fx0, sx)
                    oy = min(fy1, sy + 1) - max(fy0, sy)
                    if ox > 0 and oy > 0:
                        tot += cov.get((sx, sy), 0.0) * ox * oy
                        area += ox * oy
            out.append(tot / area if area else 0.0)
    return tw, th, out


def make_badge(atlas, aw, emblem_w=44, emblem=None, colour=GREEN, band_pad=(2, 4)):
    """a badge from Double Tap's: its bullets painted out, the background turned `colour` (None keeps Double Tap's
    red), and `emblem` (w, h, coverage 0..1 list; default Mule Kick's at emblem_w wide) drawn on it in the cream of
    the phone's emblems with a drop shadow. Used for Mule Kick here and for Double Tap II by tools/doubletap2.py.
    band_pad: rows painted out above and below the bullets (more covers their outline and shadow, which the green
    recolour hides on Mule Kick's but would show on red)."""
    bx0, by0 = DOUBLE_TAP_AT
    px = [list(atlas[(by0 + y) * aw + bx0 + x]) for y in range(H) for x in range(W)]

    def at(x, y):
        return px[y * W + x]

    def red(c):  # Double Tap's background (the frame and the bullets are cream; it fades to alpha 11/15 at the bottom)
        return c[3] > 120 and c[0] > 1.3 * c[1] and c[0] > 1.2 * c[2]

    def cream(c):
        return c[3] > 120 and lum(c) > 100 and not red(c)

    # 1. the bullets, their trails and shadows: every pixel right of the bullets' left end in the rows around them
    cr = [(x, y) for y in range(8, 48) for x in range(6, 51) if cream(at(x, y))]
    band_y0, band_y1 = min(y for _, y in cr) - band_pad[0], max(y for _, y in cr) + band_pad[1]
    band_x0 = min(x for x, _ in cr) - 2
    # the background: from the centre over red and dark pixels (the halftone's dark dots, the frame's inner shadow)
    inside = set()
    todo = [(W // 2, 12)]
    while todo:
        x, y = todo.pop()
        if (x, y) in inside or not (4 <= x < W - 4 and 0 <= y < H):  # (the black outline is outside)
            continue
        c = at(x, y)
        in_band = band_y0 <= y <= band_y1 and x >= band_x0 and c[3] > 120 and not (cream(c) and x > 48)
        if not (red(c) or in_band or (c[3] > 120 and lum(c) < 45)):
            continue
        inside.add((x, y))
        todo += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    bad = {(x, y) for x, y in inside if band_y0 <= y <= band_y1 and x >= band_x0}
    # each row's lightness, from its clean pixels (interpolated across the band)
    row_l = {}
    for y in range(H):
        v = sorted(lum(at(x, y)) for x in range(W) if (x, y) in inside and (x, y) not in bad and red(at(x, y)))
        if len(v) > 8:
            row_l[y] = v[len(v) // 2]
    ys = sorted(row_l)
    for y in range(H):
        if y not in row_l:
            lo = max((r for r in ys if r < y), default=None)
            hi = min((r for r in ys if r > y), default=None)
            row_l[y] = (row_l[lo] if hi is None else row_l[hi] if lo is None else
                        row_l[lo] + (row_l[hi] - row_l[lo]) * (y - lo) / (hi - lo))
    # paint the band from the same column 14..24 rows away (the texture keeps going; lightness per row)
    src = [c[:] for c in px]
    for x, y in sorted(bad):
        for dy in (-16, 16, -20, 20, -18, 18, -22, 22, -14, 14, -24, 24, -12, 12, -26, 26):
            sx, sy = x, y + dy
            if (sx, sy) in inside and (sx, sy) not in bad and red(src[sy * W + sx]):
                c = src[sy * W + sx]
                k = row_l[y] / row_l[sy]
                px[y * W + x] = [min(255, round(c[0] * k)), min(255, round(c[1] * k)), min(255, round(c[2] * k)),
                                 c[3]]
                break
        else:
            raise ValueError('nothing to paint %d,%d from' % (x, y))
    # 2. the new colour (dark green for Mule Kick): the background keeps its lightness (halftone, shading), the frame
    # stays
    if colour:
        mean_l = sum(lum(at(x, y)) for x, y in inside) / len(inside)
        for x, y in inside:
            c = at(x, y)
            k = lum(c) / mean_l
            c[0:3] = [min(255, round(colour[i] * k)) for i in range(3)]
        # the frame's inner edge: warm next to red, olive next to green (as Speed Cola's)
        for y in range(H):
            for x in range(W):
                c = at(x, y)
                if (x, y) in inside or c[3] <= 120 or lum(c) >= 160:
                    continue
                if any((x + dx, y + dy) in inside for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                    v = lum(c)
                    c[0:3] = [min(255, round(v * 1.04)), min(255, round(v * 1.04)), round(v * 0.74)]
    # 3. the emblem, with a drop shadow, centred in the shield
    ew, eh, cov = emblem or scaled_emblem(emblem_w)
    ox, oy = (W - ew) // 2, 30 - eh // 2
    for y in range(eh):
        for x in range(ew):
            a = cov[y * ew + x]
            for (dx, dy, k) in ((1, 2, 0.45), (2, 2, 0.25), (1, 1, 0.3)):  # shadow, down and right
                tx, ty = ox + x + dx, oy + y + dy
                if a and (tx, ty) in inside:
                    c = px[ty * W + tx]
                    c[0:3] = [round(v * (1 - k * a)) for v in c[0:3]]
    for y in range(eh):
        shade = 1.06 - 0.14 * y / max(1, eh - 1)  # lighter at the top, as the phone's emblems
        for x in range(ew):
            a = cov[y * ew + x]
            if not a:
                continue
            c = px[(oy + y) * W + ox + x]
            c[0:3] = [round(c[i] * (1 - a) + min(255, CREAM[i] * shade) * a) for i in range(3)]
            c[3] = max(c[3], 238)
    return [tuple(c) for c in px]


# ---- ingame.group.bin ----
def icon_record(template_body, slot):
    """Double Tap's element renamed Perk_Mule_Kick_Image, showing slot (x, y)"""
    b = bytearray(template_body)
    n = 0
    for i in range(len(b) - 3):
        if struct.unpack_from('<I', b, i)[0] == TEMPLATE:
            struct.pack_into('<I', b, i, ICON)
            n += 1
    assert n == 1, n  # its name inside the record (the record's own hash is outside the body)
    for prop, (x, y) in ((P_UV0, slot), (P_UV1, (slot[0] + W, slot[1] + H))):
        i = next(i for i in range(len(b) - 11) if struct.unpack_from('<II', b, i) == (prop, T_SVEC2))
        struct.pack_into('<hh', b, i + 8, x, y)
    return bytes(b)


def patch_ingame(data):
    """ingame.group.bin with the badge in hud_icons and the Perk_Mule_Kick_Image element"""
    g = gfx.Group(data)
    rec = g.find(C_TEXTURE, HUD_ICONS)
    tex = gfx.Texture.read(HUD_ICONS, rec[2])
    im = tex.image
    assert im.fmt == 5 and im.pitch == im.w * 2, (im.fmt, im.pitch)
    atlas = rgba4444_to_px(im.texels, im.w, im.h)
    slot = SLOT or free_slot(atlas, im.w, im.h)
    badge = make_badge(atlas, im.w)
    for y in range(H):
        for x in range(W):
            atlas[(slot[1] + y) * im.w + slot[0] + x] = badge[y * W + x]
    im.texels = px_to_rgba4444(atlas) + im.texels[im.w * im.h * 2:]
    rec[2] = tex.write()
    els = g.list_of(C_UIELEMENT)
    assert not els.find(ICON)
    tpl = els.find(TEMPLATE)
    els.records.append([C_UIIMAGE, ICON, icon_record(tpl[2], slot), None])
    return g.write(), slot, badge


# ---- the sting ----
MS_C1 = [256, 512, 0, 192, 240, 460, 392]
MS_C2 = [0, -256, 0, 64, 0, -208, -232]
MS_AD = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230]


def bo1_decode(raw):
    """BO1 PC sound file (MS ADPCM, 512 frames per block) -> (interleaved int16 frames, channels)"""
    ver, frames, _rate, ch = struct.unpack_from('<IIII', raw, 0)
    fmt, = struct.unpack_from('<I', raw, 0x1c)
    size, = struct.unpack_from('<I', raw, 0x30)
    assert ver == 1 and fmt == 6 and ch in (1, 2), (ver, fmt, ch)
    d = raw[len(raw) - size:]
    block = 262 * ch
    out = []
    for b in range(0, len(d) - 7 * ch + 1, block):
        hdr = struct.unpack_from('<%dB%dh' % (ch, 3 * ch), d, b)
        st = []
        for c in range(ch):
            p = hdr[c]
            delta, s1, s2 = hdr[ch + c], hdr[2 * ch + c], hdr[3 * ch + c]
            st.append([MS_C1[p], MS_C2[p], delta, s1, s2])
        for c in range(ch):
            out.append(st[c][4])
        for c in range(ch):
            out.append(st[c][3])
        nib = []
        for byte in d[b + 7 * ch:b + block]:
            nib += (byte >> 4, byte & 15)
        for i, n in enumerate(nib):
            s = st[i % ch]
            c1, c2, delta, s1, s2 = s
            v = ((s1 * c1 + s2 * c2) >> 8) + (n - 16 if n & 8 else n) * delta
            v = -32768 if v < -32768 else 32767 if v > 32767 else v
            s[2] = max(16, (MS_AD[n] * delta) >> 8)
            s[4], s[3] = s1, v
            out.append(v)
    return out[:frames * ch], ch


def wav_bytes(samples, ch, rate):
    data = struct.pack('<%dh' % len(samples), *samples)
    return (b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
            struct.pack('<IHHIIHH', 16, 1, ch, rate, rate * ch * 2, ch * 2, 16) + b'data' +
            struct.pack('<I', len(data)) + data)


def extract(folder):
    with zipfile.ZipFile(os.path.join(folder, 'main', BO1_STING[0])) as z:
        raw = z.read(BO1_STING[1])
    pcm, ch = bo1_decode(raw)
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, 'sting.wav')
        open(wav, 'wb').write(wav_bytes(pcm, ch, 48000))
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-i', wav] + STING_FFMPEG + [STING],
                       check=True)
    print('wrote %s (%.2f s, %d bytes)' % (STING, len(pcm) / ch / 48000, os.path.getsize(STING)))


def apk_with_sting(src_apk, dst_apk, extra=()):
    """a copy of the original APK with the sting added, plus `extra` [(asset name, file)] (for the Windows version,
    which reads the game's assets from it)"""
    added = [(STING_ASSET, STING)] + list(extra)
    tmp = dst_apk + '.tmp'
    with zipfile.ZipFile(src_apk) as s, zipfile.ZipFile(tmp, 'w') as d:
        for info in s.infolist():
            if info.filename.startswith('META-INF/') or info.filename in dict(added):
                continue
            d.writestr(info, s.read(info))
        for name, path in added:
            zi = zipfile.ZipInfo(name, date_time=(2014, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED  # the engine plays assets from the APK by byte range
            zi.external_attr = 0o644 << 16
            d.writestr(zi, open(path, 'rb').read())
    os.replace(tmp, dst_apk)


if __name__ == '__main__':
    if '--extract' in sys.argv:
        rest = [a for a in sys.argv[1:] if a != '--extract']
        extract(rest[0] if rest else BO1)
    elif '--preview' in sys.argv:
        out = sys.argv[sys.argv.index('--preview') + 1]
        src = os.path.join(ROOT, '..', 'backup', 'obb', 'com.activision.boz', 'blackops_etc.dz')
        _, slot, badge = patch_ingame(dzpatch.read_file(src, INGAME))
        k = 6
        img = bytearray()
        for y in range(H * k):
            for x in range(W * k):
                r, g, b, a = badge[(y // k) * W + x // k]
                img += bytes((r * a // 255 + 60 * (255 - a) // 255, g * a // 255 + 60 * (255 - a) // 255,
                              b * a // 255 + 60 * (255 - a) // 255, 255))
        pngio.write(out, W * k, H * k, bytes(img))
        print('badge at', slot, '->', out)
    else:
        print('usage: python tools/mulekick_hud.py --extract [BO1 folder] | --preview OUT.png')
