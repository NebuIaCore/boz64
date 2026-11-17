# Black Ops II's Double Tap Root Beer II machine in place of Double Tap's on Kino der Toten and Call of the Dead (the
# perk itself already works like BO2's Double Tap II: src/doubletap.cpp). tools/mulekick.py, which builds the game data
# pack, calls build_files(); in the pack it replaces:
#   levels/<map>/<map>_dynamics.group.bin   the machine's model and skin (CIwModel and CIwAnimSkin 0xfe170185, the
#                                            "Doubletap" entity's CIsRenderableModel assets) with BO2's, plus its
#                                            textures and materials
#   ingame/ingame.group.bin                  Double Tap's HUD perk icon, made the way Mule Kick's is
#                                            (tools/mulekick_hud.py make_badge): the phone's own Double Tap badge, red
#                                            halftone shield and cream frame, with its two bullets painted out and BO2's
#                                            Double Tap II emblem (the starburst and the crossed bullets, the light part
#                                            of BO2's icon) drawn on it in the cream of the phone's emblems, with a drop
#                                            shadow. It goes in Double Tap's own 57x67 part of hud_icons (the only part
#                                            Perk_Double_Tap_Image shows; no other UI image uses it). Runs after
#                                            mulekick_hud.py, which builds Mule Kick's badge from the original.
#
# The purchase sting stays the game's own (blackops-music/mus_doubletap_sting.mp3): BO2's mus_doubletap_sting is the
# same recording (correlation 0.998 after 69 ms of leading silence), and the phone's copy is already in the format of
# the other perk stings (MP3, 16 kHz mono, 128 kbps).
#
# The assets (data/doubletap2), exported with GrayWolf from the user's Black Ops II, zone all/zm_transit.ff:
#   model.obj   zombie_vending_doubletap2_on, LOD0 (2626 triangles): centimetres, z up, front facing -y, origin at the
#               back of the machine
#   body.png    its colour map (~-gzombie_vending_doubletap2_c, 512x512); vent.png: ~-gzombie_vending_doubletap2_vent_on
#   hud_icon.png  BO2's HUD perk icon, specialty_doubletap_zombies (64x64; zone all/zm_prison_patch.ff): only its
#               emblem is used
#
# Conversion: the phone's model units are the export's centimetres (BO1's Double Tap machine, exported the same way,
# matches the phone's model to the unit): model = (x, z, BACK_Z + (back - y)), so the front faces +z and the back sits
# where the old machine's did, against the wall. BO2's machine is 229 units tall (BO1's: 207). Counter-clockwise faces,
# lighting baked into grey vertex colours as for Mule Kick, the lit sign at full brightness, uncompressed textures.
# Every vertex is bound to the root bone of the old machine's skeleton, so its animation state machine drives it
# unchanged.
#
# usage: python tools/doubletap2.py --preview OUT.png   renders the converted model, front and back, to check it
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dzpatch  # noqa: E402
import iwgfx as gfx  # noqa: E402
import levelpatch as lp  # noqa: E402
import mulekick_hud as hud  # noqa: E402
import pngio  # noqa: E402

ASSETS = os.path.join(ROOT, 'data', 'doubletap2')

MAPS = ('kino', 'callofthedead')
ENTITY = 'Doubletap'
MODEL = 0xfe170185  # the machine's CIwModel and CIwAnimSkin
SKEL = 0x4d56a629   # their CIwAnimSkel
BACK_Z = -27        # the old model's back (its z runs -27 .. 37)

# OBJ material -> (texture, lit sign)
SURFACES = {
    'mtl_zombie_vending_doubletap2': ('body', False),
    'mtl_zombie_vending_doubletap2_on': ('body', True),
    'mtl_zombie_vending_doubletap2_vent_on': ('vent', False),
}
LIGHT = (0.35, 0.75, 0.56)  # baked light direction (model space, toward the light), as Mule Kick's

C_RENDER, F_ASSETS = 0xe44088e9, 0xf2853858


def statics_path(m):
    return 'levels/%s/%s_statics.group.bin' % (m, m)


def dynamics_path(m):
    return 'levels/%s/%s_dynamics.group.bin' % (m, m)


def load_obj(path):
    """-> [(pos, uv, normal, material)], {material: [(i0, i1, i2)]}; vertices shared when all four match"""
    vs, vts, vns = [], [], []
    verts, index, tris = [], {}, {}
    mat = None
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == 'v':
            vs.append(tuple(float(x) for x in p[1:4]))
        elif p[0] == 'vt':
            vts.append((float(p[1]), float(p[2])))
        elif p[0] == 'vn':
            vns.append(tuple(float(x) for x in p[1:4]))
        elif p[0] == 'usemtl':
            mat = p[1]
        elif p[0] == 'f':
            face = []
            for c in p[1:]:
                k = tuple(int(x) - 1 for x in c.split('/'))
                v = (vs[k[0]], vts[k[1]], vns[k[2]], mat)
                key = tuple(round(x, 4) for x in v[0] + v[1] + v[2]) + (mat,)
                if key not in index:
                    index[key] = len(verts)
                    verts.append(v)
                face.append(index[key])
            for i in range(1, len(face) - 1):
                tris.setdefault(mat, []).append((face[0], face[i], face[i + 1]))
    return verts, tris


def convert():
    """-> (positions, uvs, colours, {texture: [triangles]}) in phone model units"""
    verts, tris = load_obj(os.path.join(ASSETS, 'model.obj'))
    unknown = set(tris) - set(SURFACES)
    assert not unknown, 'materials not in SURFACES: %s' % sorted(unknown)
    back = max(v[0][1] for v in verts)
    ln = math.sqrt(sum(c * c for c in LIGHT))
    light = [c / ln for c in LIGHT]
    pos, uvs, cols = [], [], []
    for (x, y, z), (u, v), (nx, ny, nz), mat in verts:
        pos.append((int(round(x)), int(round(z)), int(round(BACK_Z + back - y))))
        uvs.append((int(round(u * 4096)), int(round((1 - v) * 4096))))  # OBJ v is 1 - the game's
        if SURFACES[mat][1]:
            g = 255
        else:
            n = (nx, nz, -ny)
            lit = 125 + 130 * max(0.0, sum(n[k] * light[k] for k in range(3))) + 20 * max(0.0, n[1])
            g = max(0, min(255, int(round(lit))))
        cols.append((g, g, g, 255))
    by_tex = {}
    for mat, t in tris.items():
        by_tex.setdefault(SURFACES[mat][0], []).extend(t)
    return pos, uvs, cols, by_tex


def texture(name):
    w, h, rgba = pngio.read(os.path.join(ASSETS, name + '.png'))
    px = bytearray(rgba)
    px[3::4] = b'\xff' * (w * h)  # opaque: BO2 alpha-tests a few texels of the body map, the phone's machines don't
    return w, h, bytes(px)


def build_records():
    """-> ([(class hash, resource hash, record body)], model positions)"""
    pos, uvs, cols, by_tex = convert()
    records, materials, tris_by_mat = [], [], []
    for tex, t in sorted(by_tex.items()):
        th, mh = gfx.djb2('doubletap2_tex_' + tex), gfx.djb2('doubletap2_mat_' + tex)
        w, h, rgba = texture(tex)
        records.append((gfx.H['CIwTexture'], th, gfx.Texture.build_abgr8888(th, w, h, rgba, mipmaps=True).write()))
        records.append((gfx.H['CIwMaterial'], mh, gfx.Material.build(mh, th).write()))
        materials.append(mh)
        tris_by_mat.append(t)
    model = gfx.Model.build(MODEL, pos, uvs, tris_by_mat, materials, cols=cols)
    records.append((gfx.H['CIwModel'], MODEL, model.write()))
    return records, pos


def patch_dynamics(data, records, pos):
    g = gfx.Group(data)
    used = g.all_hashes()
    assert g.find(gfx.H['CIwModel'], MODEL) and g.find(gfx.H['CIwAnimSkin'], MODEL), 'no Double Tap model here'
    for cls, h, body in records:
        if cls == gfx.H['CIwModel']:
            g.add(cls, h, body, replace=True)
        else:
            assert h not in used, 'hash %08x is already used' % h
            g.add(cls, h, body)
    skel = gfx.decode(gfx.H['CIwAnimSkel'], g.find(gfx.H['CIwAnimSkel'], SKEL))
    root = [b for b in skel.bones if b.hash == SKEL][0]
    skin = gfx.Skin.build_rigid(MODEL, MODEL, skel, root.index, pos)
    g.add(gfx.H['CIwAnimSkin'], MODEL, skin.write(), replace=True)
    out = g.write()
    assert gfx.Group(out).write() == out
    return out


def check_statics(data):
    rec = lp.Level('statics', data).find(ENTITY)
    assets = rec.field(C_RENDER, F_ASSETS).data
    assert assets.count(struct.pack('<I', MODEL)) == 2, 'the machine does not use the expected model'


EMBLEM_W = 40          # the emblem's width on the badge (Mule Kick's three pistols: 44; this one is about square)
EMBLEM_BG, EMBLEM_WHITE = 110, 215  # lightness of BO2's red background (its brighter grain) and of the emblem
EMBLEM_MIN_PIXELS = 20  # light specks smaller than this (the icon's scratches) are not emblem
# rows painted out around Double Tap's bullets: just the bullets (rows 25-36) and their shadows (37-38). Mule Kick's
# wider default repaints clean rows too, a little lighter above and darker below, which its green recolour hides but
# shows as seams on red.
BAND_PAD = (0, 2)


def emblem_mask():
    """BO2's Double Tap II emblem from its icon: (x, y, w, h, {(x, y): coverage 0..1}) as mulekick_hud's"""
    iw, ih, px = pngio.read(os.path.join(ASSETS, 'hud_icon.png'))
    cov = {}
    for y in range(4, ih - 4):        # inside the frame (its light edges are the outer 4 pixels)
        for x in range(5, iw - 5):
            c = px[(y * iw + x) * 4:(y * iw + x) * 4 + 4]
            if c[3] < 128 or (max(c[:3]) - min(c[:3])) > 0.5 * max(c[:3]):  # the emblem is pale: skip red/brown
                continue
            e = (hud.lum(c) - EMBLEM_BG) / (EMBLEM_WHITE - EMBLEM_BG)
            if e > 0.2:
                cov[(x, y)] = min(1.0, e)
    # keep the emblem's shapes, drop the scratches: connected pieces of at least EMBLEM_MIN_PIXELS
    keep, seen = {}, set()
    for p in cov:
        if p in seen:
            continue
        piece, todo = [], [p]
        seen.add(p)
        while todo:
            x, y = todo.pop()
            piece.append((x, y))
            for q in ((x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
                if q in cov and q not in seen:
                    seen.add(q)
                    todo.append(q)
        if len(piece) >= EMBLEM_MIN_PIXELS:
            keep.update((q, cov[q]) for q in piece)
    xs = [x for x, _ in keep]
    ys = [y for _, y in keep]
    bx, by = min(xs), min(ys)
    return bx, by, max(xs) + 1 - bx, max(ys) + 1 - by, keep


def patch_ingame(data):
    """ingame.group.bin with the Double Tap II badge in Double Tap's part of hud_icons"""
    g = gfx.Group(data)
    rec = g.find(hud.C_TEXTURE, hud.HUD_ICONS)
    tex = gfx.Texture.read(hud.HUD_ICONS, rec[2])
    im = tex.image
    assert im.fmt == 5 and im.pitch == im.w * 2, (im.fmt, im.pitch)
    atlas = hud.rgba4444_to_px(im.texels, im.w, im.h)
    x0, y0 = hud.DOUBLE_TAP_AT
    badge = hud.make_badge(atlas, im.w, emblem=hud.scaled_emblem(EMBLEM_W, emblem_mask()), colour=None,
                           band_pad=BAND_PAD)
    for y in range(hud.H):
        for x in range(hud.W):
            atlas[(y0 + y) * im.w + x0 + x] = badge[y * hud.W + x]
    im.texels = hud.px_to_rgba4444(atlas) + im.texels[im.w * im.h * 2:]
    rec[2] = tex.write()
    return g.write()


def build_files(src, files):
    """the pack files to replace, on top of `files` (the ones already patched)"""
    out = {}
    records, pos = build_records()
    for m in MAPS:
        sp, dp = statics_path(m), dynamics_path(m)
        check_statics(files[sp] if sp in files else dzpatch.read_file(src, sp))
        out[dp] = patch_dynamics(files[dp] if dp in files else dzpatch.read_file(src, dp), records, pos)
    out[hud.INGAME] = patch_ingame(files[hud.INGAME] if hud.INGAME in files else dzpatch.read_file(src, hud.INGAME))
    return out


def inputs():
    """files build_files depends on, for tools/mulekick.py's up-to-date check"""
    return [os.path.join(HERE, 'doubletap2.py')] + [os.path.join(ASSETS, n) for n in sorted(os.listdir(ASSETS))]


def preview(out_png, size=360):
    """front and back views of the converted model (orthographic, textured, back faces culled) side by side"""
    import numpy as np
    pos, uvs, cols, by_tex = convert()
    P = np.array(pos, float)
    UV = np.array(uvs, float) / 4096
    C = np.array(cols, float)[:, 0] / 255
    texs = {}
    for name in by_tex:
        w, h, rgba = texture(name)
        texs[name] = np.frombuffer(rgba, np.uint8).reshape(h, w, 4)[:, :, :3]
    lo, hi = P.min(0), P.max(0)
    scale = (size - 20) / max(hi[0] - lo[0], hi[1] - lo[1])
    W = int((hi[0] - lo[0]) * scale) + 20
    views = []
    for sign in (1, -1):  # front: looking from +z; back: turned 180 degrees about y
        img = np.full((size, W, 3), 40, np.uint8)
        zb = np.full((size, W), -1e9)
        sx = (sign * P[:, 0] - (lo[0] if sign > 0 else -hi[0])) * scale + 10
        sy = (hi[1] - P[:, 1]) * scale + 10
        sz = sign * P[:, 2]
        for name, tris in by_tex.items():
            tex = texs[name]
            th, tw = tex.shape[:2]
            for i0, i1, i2 in tris:
                x = sx[[i0, i1, i2]]
                y = sy[[i0, i1, i2]]
                area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
                if area >= 0:  # screen y points down: counter-clockwise faces have negative area here
                    continue
                x0, x1 = int(max(0, x.min())), int(min(W - 1, x.max())) + 1
                y0, y1 = int(max(0, y.min())), int(min(size - 1, y.max())) + 1
                if x0 >= x1 or y0 >= y1:
                    continue
                gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
                w0 = ((x[1] - gx) * (y[2] - gy) - (x[2] - gx) * (y[1] - gy)) / area
                w1 = ((x[2] - gx) * (y[0] - gy) - (x[0] - gx) * (y[2] - gy)) / area
                w2 = 1 - w0 - w1
                inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
                if not inside.any():
                    continue
                idx = [i0, i1, i2]
                depth = w0 * sz[i0] + w1 * sz[i1] + w2 * sz[i2]
                sub = zb[y0:y1, x0:x1]
                draw = inside & (depth > sub)
                if not draw.any():
                    continue
                u = w0 * UV[i0, 0] + w1 * UV[i1, 0] + w2 * UV[i2, 0]
                v = w0 * UV[i0, 1] + w1 * UV[i1, 1] + w2 * UV[i2, 1]
                shade = np.minimum(1.0, w0 * C[i0] + w1 * C[i1] + w2 * C[i2])
                tx = (np.mod(u, 1.0) * tw).astype(int).clip(0, tw - 1)
                ty = (np.mod(v, 1.0) * th).astype(int).clip(0, th - 1)  # game v runs down the image
                rgb = tex[ty, tx] * shade[..., None]
                sub[draw] = depth[draw]
                img[y0:y1, x0:x1][draw] = rgb[draw].astype(np.uint8)
        views.append(img)
    both = np.concatenate([views[0], np.full((size, 8, 3), 0, np.uint8), views[1]], axis=1)
    h, w = both.shape[:2]
    rgba = np.concatenate([both, np.full((h, w, 1), 255, np.uint8)], axis=2)
    pngio.write(out_png, w, h, rgba.tobytes())
    print('front | back ->', out_png)


def preview_icons(out_png, k=5):
    """the HUD badges as the pack has them (Double Tap, Mule Kick and their neighbours in the perk row), k x"""
    src = os.path.join(ROOT, 'out', 'data', 'blackops_etc.dz')
    g = gfx.Group(dzpatch.read_file(src, hud.INGAME))
    im = gfx.Texture.read(hud.HUD_ICONS, g.find(hud.C_TEXTURE, hud.HUD_ICONS)[2]).image
    atlas = hud.rgba4444_to_px(im.texels, im.w, im.h)
    els = g.list_of(hud.C_UIELEMENT)
    slots = []
    for name in ('Perk_Double_Tap_Image', 'Perk_Mule_Kick_Image', 'Perk_Juggernog_Image', 'Perk_Speed_Cola_Image',
                 'Perk_Quick_Revive_Image'):
        rec = els.find(gfx.djb2(name))
        if rec:
            b = rec[2]
            i = next(i for i in range(len(b) - 11) if struct.unpack_from('<II', b, i) == (hud.P_UV0, hud.T_SVEC2))
            slots.append(struct.unpack_from('<hh', b, i + 8))
    w, h = (hud.W + 4) * len(slots) * k, hud.H * k
    img = bytearray()
    for y in range(h):
        for x in range(w):
            n, sx = divmod(x // k, hud.W + 4)
            r = g_ = b = a = 0
            if sx < hud.W:
                r, g_, b, a = atlas[(slots[n][1] + y // k) * im.w + slots[n][0] + sx]
            img += bytes((r * a // 255 + 50 * (255 - a) // 255, g_ * a // 255 + 50 * (255 - a) // 255,
                          b * a // 255 + 50 * (255 - a) // 255, 255))
    pngio.write(out_png, w, h, bytes(img))
    print('badges ->', out_png)


if __name__ == '__main__':
    if '--preview' in sys.argv:
        preview(sys.argv[sys.argv.index('--preview') + 1])
    elif '--preview-icons' in sys.argv:
        preview_icons(sys.argv[sys.argv.index('--preview-icons') + 1])
    else:
        print('usage: python tools/doubletap2.py --preview OUT.png        the converted machine, front and back\n'
              '       python tools/doubletap2.py --preview-icons OUT.png  the HUD perk badges in the built pack\n'
              '(the pack is built by tools/mulekick.py)')
