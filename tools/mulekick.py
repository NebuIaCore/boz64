# Builds the game data pack with Mule Kick, Black Ops PC's 8th perk machine, added to Kino der Toten, Ascension and
# Call of the Dead (the game code side is src/mulekick.cpp). Writes out/data/blackops_etc.dz, a copy of the original
# pack (backup/obb/com.activision.boz/blackops_etc.dz) with these files replaced:
#   levels/<map>/<map>_statics.group.bin    the machine (a copy of the map's Speed Cola machine, perk type 7)
#   levels/<map>/<map>_dynamics.group.bin   its model: BO1's zombie_vending_three_gun (data/mulekick)
#   localisation/*                           its name, "Mule Kick"
#   ingame/sound/snd_player_*.group.bin      more takes of the Kino crew's voice lines, from BO1 PC (tools/voxpatch.py)
#   ingame/ingame.group.bin                  its HUD perk icon (tools/mulekick_hud.py)
#   levels/kino/kino_{statics,dynamics}      also Black Ops PC's hellhound rounds (tools/dogpatch.py)
#   levels/{kino,callofthedead}/*_dynamics   also Black Ops II's Double Tap II machine (tools/doubletap2.py)
#   ingame/weapons, characters/arms, levels/<map>/<map>_statics   also World at War Zombies guns in the mystery box
#                                            (tools/wawguns.py)
# tools/package.py puts it in the APK; for the Windows version it is linked into the game folder (BOZ64 PC).
# Its purchase sting (BO1's, data/mulekick/mus_mulekick_sting.mp3) is an APK asset: tools/package.py adds it, and for
# the Windows version this writes out/pc/base.apk (the original APK with it added) and links it into the game folder.
#
# The Ascension and Call of the Dead spots are where the BO1 PC mod "C Reawakened" puts Mule Kick
# (zombie_<map>_perk_machines.gsc), converted to the phone maps' coordinates: phone = s * (x, z, -y) + t, fitted per map
# on the wall guns and perk machines both versions have. The phone's perk machines turn by the same angle as BO1's (a
# quaternion about the phone's y axis). Kino's is the user's choice (see SPOTS).
#
# The model (data/mulekick, extracted from BO1's zombie_moon zone): LOD1 (1050 triangles; LOD2/3 map the front sign
# upside down), converted the way the phone's own machines are: model = 2.5 * (x, z, -y) (y up, front facing +z),
# counter-clockwise faces, lighting baked into grey vertex colours, uncompressed textures. It is bound to the root
# bone of Speed Cola's skeleton, so Speed Cola's animation state machine drives it unchanged.
#
# usage: python tools/mulekick.py [--force] [--link-pc]
#   rebuilds only when an input changed; --link-pc also points the Windows game folder at the new pack and APK
#   (as a library: build() -> the pack's path)
import hashlib
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dogpatch  # noqa: E402
import doubletap2  # noqa: E402
import dzpatch  # noqa: E402
import iwgfx as gfx  # noqa: E402
import levelpatch as lp  # noqa: E402
import locpatch  # noqa: E402
import mulekick_hud as hud  # noqa: E402
import pngio  # noqa: E402
import voxpatch  # noqa: E402
import wawguns  # noqa: E402

SRC = os.path.join(ROOT, '..', 'backup', 'obb', 'com.activision.boz', 'blackops_etc.dz')
OUT = os.path.join(ROOT, 'out', 'data', 'blackops_etc.dz')
ASSETS = os.path.join(ROOT, 'data', 'mulekick')
PC_GAME = os.path.join(ROOT, '..', '..', 'BOZ64 PC')
PC_PACK = os.path.join(PC_GAME, 'data', 'sdcard', 'Android', 'obb', 'com.activision.boz', 'blackops_etc.dz')
# the PC test copy's own data folder (data3, when it exists) gets the same pack
PC_TEST_PACK = os.path.join(PC_GAME, 'data3', 'sdcard', 'Android', 'obb', 'com.activision.boz', 'blackops_etc.dz')
BASE_APK = os.path.join(ROOT, '..', 'backup', 'apk', 'base.apk')
OUT_APK = os.path.join(ROOT, 'out', 'pc', 'base.apk')
PC_APKS = [os.path.join(PC_GAME, d, 'data', 'app', 'com.activision.boz', 'base.apk') for d in ('data', 'data3')]

PERK_TYPE = 7
COST = 4000
NAME_KEY = lp.djb2('S_BLACKOPS_PERK_MULE_KICK')  # the machine's name text (CPerk perkName)
NAME = {'english_us': 'Mule Kick'}
JINGLE = hud.STING_PATH  # the purchase sting (CPerk jingle): BO1's Mule Kick sting

# map: (pos on the phone map, yaw in degrees: the machine's front faces (sin yaw, 0, cos yaw))
SPOTS = {
    # where the user's Black Ops PC screenshot shows it: the portrait room upstairs, in the corner by the chain-link
    # fence, back to the wall, the poster wall to its right. Found in game (BOZ64 PC test copy): the corner the player
    # stops in is (2877.3, 836.8) (back wall at x - 37, poster wall at z - 37), the fence's pillar at z ~ 985.
    'kino': ((2896.6, 784.6, 892.5), 90.0),
    # BO1 (-514, 1225, -485.875) yaw 0
    'ascension': ((-1303.4, -1229.1, -3107.5), 0.0),
    # BO1 (2424.4, -2884.3, 314) yaw 231.6
    'callofthedead': ((6157.6, 796.7, 7314.2), 231.6),
}

# component and field hashes (see levelpatch.py)
C_PERK, F_COST, F_TYPE, F_PERKNAME, F_JINGLE = 0x0f3da13a, 0x3d98668e, 0xf5a0b765, 0x48f4b6f8, 0xc836045c
C_TRANSFORM, F_POS, F_QUAT = 0xfbfcbd60, 0xe198c291, 0xbbe0d0ec
C_NAMED, F_NAME = 0x800a5dc9, 0x7c9b0c46
C_RENDER, F_ASSETS = 0xe44088e9, 0xf2853858  # CIsRenderableModel's [{model, skin}]

# the model
MODEL_SCALE = 2.5                 # BO1 inches -> phone model units
SPEED_COLA_MODEL = 0x9b750eb5     # "sleight": model and skin of the machine copied
SPEED_COLA_SKEL = 0x5857cb59
MODEL = gfx.djb2('mulekick_machine')  # model and skin
# OBJ material -> (texture, downscale factor, mipmaps)
SURFACES = {
    'mc_mtl_zombie_vending_three_gun': ('body', 2),
    'mc_mtl_zombie_vending_three_gun_logo': ('logo', 1),
    'mc_mtl_zombie_vending_three_gun_logo_off': ('logo', 1),
    'mc_global_glass': ('glass', 8),
}
LIGHT = (0.35, 0.75, 0.56)        # baked light direction (phone model space, toward the light)


def statics_path(m):
    return 'levels/%s/%s_statics.group.bin' % (m, m)


def dynamics_path(m):
    return 'levels/%s/%s_dynamics.group.bin' % (m, m)


def machine(level, pos, yaw):
    rec = level.find('SpeedCola')
    rec.hash = lp.djb2('MuleKick')
    rec.set(C_NAMED, F_NAME, lp.cstr('MuleKick'))
    rec.set(C_PERK, F_COST, lp.i32(COST))
    rec.set(C_PERK, F_TYPE, lp.i32(PERK_TYPE))
    rec.set(C_PERK, F_PERKNAME, lp.i32(NAME_KEY))
    rec.set(C_PERK, F_JINGLE, lp.cstr(JINGLE))
    rec.set(C_TRANSFORM, F_POS, lp.f32s(*pos))
    h = math.radians(yaw) / 2
    rec.set(C_TRANSFORM, F_QUAT, lp.f32s(0.0, math.sin(h), 0.0, math.cos(h)))
    assets = rec.field(C_RENDER, F_ASSETS).data
    old = struct.pack('<I', SPEED_COLA_MODEL)
    assert assets.count(old) == 2, 'unexpected CIsRenderableModel assets'
    rec.set(C_RENDER, F_ASSETS, assets.replace(old, struct.pack('<I', MODEL)))
    return rec


def load_obj(path):
    """-> unique vertices [(pos, uv, normal)], {material: [(i0, i1, i2)]} in BO1 coordinates, OBJ uv"""
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
                if k not in index:
                    index[k] = len(verts)
                    verts.append((vs[k[0]], vts[k[1]], vns[k[2]]))
                face.append(index[k])
            for i in range(1, len(face) - 1):
                tris.setdefault(mat, []).append((face[0], face[i], face[i + 1]))
    return verts, tris


def build_model_records():
    """-> [(class hash, resource hash, record body)] in the order they must be added"""
    verts, tris = load_obj(os.path.join(ASSETS, 'lod1.obj'))
    L = LIGHT
    ln = math.sqrt(sum(c * c for c in L))
    L = [c / ln for c in L]
    pos, uvs, cols = [], [], []
    for (x, y, z), (u, v), (nx, ny, nz) in verts:
        pos.append(tuple(int(round(MODEL_SCALE * c)) for c in (x, z, -y)))
        uvs.append((int(round(u * 4096)), int(round((1 - v) * 4096))))  # OBJ v is 1 - the game's
        n = (nx, nz, -ny)
        lit = 125 + 130 * max(0.0, sum(n[k] * L[k] for k in range(3))) + 20 * max(0.0, n[1])
        g = max(0, min(255, int(round(lit))))
        cols.append((g, g, g, 255))
    records = []
    textures, materials, tris_by_mat = {}, [], []
    for objmat, (tex, factor) in SURFACES.items():
        if objmat not in tris:
            continue
        if tex not in textures:
            w, h, rgba = pngio.read(os.path.join(ASSETS, tex + '.png'))
            if factor > 1:
                w, h, rgba = pngio.downscale(w, h, rgba, factor)
            th = gfx.djb2('mulekick_tex_' + tex)
            textures[tex] = th
            records.append((gfx.H['CIwTexture'], th, gfx.Texture.build_abgr8888(th, w, h, rgba, mipmaps=True).write()))
            mh = gfx.djb2('mulekick_mat_' + tex)
            records.append((gfx.H['CIwMaterial'], mh, gfx.Material.build(mh, th).write()))
            materials.append(mh)
            tris_by_mat.append([])
        tris_by_mat[materials.index(gfx.djb2('mulekick_mat_' + tex))] += tris[objmat]
    model = gfx.Model.build(MODEL, pos, uvs, tris_by_mat, materials, cols=cols)
    records.append((gfx.H['CIwModel'], MODEL, model.write()))
    return records, pos


def add_model(dyn, records, pos):
    g = gfx.Group(dyn)
    used = g.all_hashes()
    for cls, h, body in records:
        assert h not in used, 'hash %08x is already used' % h
        g.add(cls, h, body)
    skel = gfx.decode(gfx.H['CIwAnimSkel'], g.find(gfx.H['CIwAnimSkel'], SPEED_COLA_SKEL))
    root = [b for b in skel.bones if b.hash == SPEED_COLA_SKEL][0]
    skin = gfx.Skin.build_rigid(MODEL, MODEL, skel, root.index, pos)
    g.add(gfx.H['CIwAnimSkin'], MODEL, skin.write())
    out = g.write()
    assert gfx.Group(out).write() == out
    return out


def build_files(src):
    files = {}
    records, pos = build_model_records()
    for m, (spot, yaw) in SPOTS.items():
        path = statics_path(m)
        level = lp.Level(path, dzpatch.read_file(src, path))
        assert not level.has('MuleKick'), m
        level.append(machine(level, spot, yaw))
        files[path] = bytes(level.d)
        files[dynamics_path(m)] = add_model(dzpatch.read_file(src, dynamics_path(m)), records, pos)
    loc = {locpatch.HASHES: dzpatch.read_file(src, locpatch.HASHES)}
    for lang in locpatch.LANGS:
        loc[locpatch.lang_path(lang)] = dzpatch.read_file(src, locpatch.lang_path(lang))
    files.update(locpatch.add(loc, [(NAME_KEY, NAME)]))
    files[dogpatch.STATICS] = dogpatch.patch_statics(files[dogpatch.STATICS])
    files[dogpatch.DYNAMICS] = dogpatch.patch_dynamics(files[dogpatch.DYNAMICS])
    files.update(voxpatch.build_files(src))
    files[hud.INGAME] = hud.patch_ingame(dzpatch.read_file(src, hud.INGAME))[0]
    files.update(doubletap2.build_files(src, files))  # on top of the Mule Kick and dog round edits
    files.update(wawguns.build_files(src, files))  # World at War guns in the box (on top of Mule Kick's statics)
    return files


def inputs_hash():
    h = hashlib.sha256()
    for name in ['mulekick.py', 'mulekick_hud.py', 'dogpatch.py', 'levelpatch.py', 'locpatch.py', 'dzpatch.py', 'iwgfx.py', 'pngio.py']:
        h.update(open(os.path.join(HERE, name), 'rb').read())
    for name in sorted(os.listdir(ASSETS)):
        h.update(name.encode() + open(os.path.join(ASSETS, name), 'rb').read())
    for path in voxpatch.inputs() + doubletap2.inputs() + wawguns.inputs():
        h.update(os.path.basename(path).encode() + open(path, 'rb').read())
    h.update(wawguns.inputs_stamp().encode())
    st = os.stat(SRC)
    h.update(('%d %d' % (st.st_size, st.st_mtime_ns)).encode())
    return h.hexdigest()


def build_pc_apk():
    """out/pc/base.apk: the original APK with Mule Kick's sting added (the Windows version reads assets from it)"""
    stamp = OUT_APK + '.inputs'
    h = hashlib.sha256(open(hud.STING, 'rb').read() + open(os.path.join(HERE, 'mulekick_hud.py'), 'rb').read())
    st = os.stat(BASE_APK)
    h.update(('%d %d' % (st.st_size, st.st_mtime_ns)).encode())
    want = h.hexdigest()
    if os.path.exists(OUT_APK) and os.path.exists(stamp) and open(stamp).read() == want:
        return OUT_APK
    os.makedirs(os.path.dirname(OUT_APK), exist_ok=True)
    hud.apk_with_sting(BASE_APK, OUT_APK)
    open(stamp, 'w').write(want)
    print('mulekick: wrote %s' % os.path.normpath(OUT_APK))
    return OUT_APK


def link_pc():
    # the Windows version reads the pack from data\sdcard\... and the APK from data\data\app\...:
    # point them (and the test copy's, data3) at the new ones
    for src, dst in [(OUT, PC_PACK), (OUT, PC_TEST_PACK)] + [(build_pc_apk(), a) for a in PC_APKS]:
        if not os.path.isdir(os.path.dirname(dst)):
            continue
        if os.path.exists(dst) and os.path.samefile(dst, src):
            continue
        try:
            if os.path.exists(dst):
                os.remove(dst)  # a hard link to the original, an older build or a test copy: the files stay
            os.link(src, dst)
            print('linked', os.path.normpath(dst))
        except OSError as e:
            print('could not link %s (is the game running?): %s' % (os.path.normpath(dst), e))


def build(force=False):
    stamp = OUT + '.inputs'
    want = inputs_hash()
    if not force and os.path.exists(OUT) and os.path.exists(stamp) and open(stamp).read() == want:
        print('mulekick: up to date')
    else:
        os.makedirs(os.path.dirname(OUT), exist_ok=True)
        files = build_files(SRC)
        dzpatch.patch(SRC, OUT, files)
        open(stamp, 'w').write(want)
        print('mulekick: wrote %s (%d files replaced)' % (os.path.normpath(OUT), len(files)))
    return OUT


def main():
    build('--force' in sys.argv)
    if '--link-pc' in sys.argv:
        link_pc()


if __name__ == '__main__':
    main()
