# World at War Zombies (iOS) guns in Black Ops Zombies' mystery box. tools/mulekick.py, which builds the game data
# pack, calls build_files(); for each map in MAPS it adds to the pack:
#   ingame/weapons/weapons_<map>.group.bin      the gun's model, LOD model, skin, skeleton, textures, materials, gun
#                                                animations and gun state machine, and its CIsEntitySpec (a copy of a
#                                                Black Ops gun's CPlayerWeapon with the World at War gun's stats), plus
#                                                the same for its Pack-a-Punch version
#   ingame/characters/arms/arms_<map>.group.bin  the arm animations and the arms state machine
#   levels/<map>/<map>_statics.group.bin         its spec in every mystery box's m_Weapons
#
# The World at War data comes from the user's IPA (Call of Duty World at War Zombies v1.5.0, `cod waw` next to the
# port, or WAW_IPA=path): _main.dz -> ingame/player/player.group.bin (tools/waw_dz.py). Without it this does nothing.
# The formats are in tools/waw_gun.py (CItemGun), waw_asm.py, waw_anim.py, pvrtc.py and work/waw/README.md.
#
# Each World at War gun is built on a Black Ops gun that Black Ops converted from World at War (BASES: the MP40): the
# Black Ops gun's spec and both of its state machines are copied, which keeps Black Ops' own states, events and
# callbacks, and their animations are swapped for the World at War gun's. Which animations are the gun's own comes
# from comparing the two World at War arms state machines (the base gun's and this gun's have the same states); the
# gun's own animations come from the playGunAnim calls in World at War's arms machine (World at War has no separate gun
# machine). A state's length is scaled with its new animation's.
#
# Units: Black Ops' weapon models are 16 x World at War's (the shared MP40: bounds -1534..2510 vs -96..158), skeletons
# and animation positions 6.4 x (the shared arms skeleton's bind positions; CIwAnimSkel +0x20: World at War 40960 =
# model x 10, Black Ops 16384 = model x 4, so skin positions are 6.4 x too). Textures are PVRTC, which neither the
# phones nor the PC renderer take: decoded and stored uncompressed (GL makes the mipmaps). The model keeps World at
# War's materials: the normal gun gets the plain ones from its material sets (CIsMaterialSet 0: the model, 2: the LOD),
# the Pack-a-Punch gun the model's own (World at War's blue camo texture through the second UV set, as Black Ops'
# Pack-a-Punch materials do with theirs).
#
# The spec: clip and reserve are World at War's (Black Ops kept the MP40's: 32 / 192, Pack-a-Punch 64 / 192); damage
# scales the base gun's by the two World at War guns' damage (and head multipliers). Sounds, HUD icon and the
# third-person state machine stay the base gun's for now.
#
# usage: python tools/wawguns.py --check     converts and prints what would be added (no pack is written)
import copy
import hashlib
import math
import os
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dzpatch  # noqa: E402
import imaadpcm  # noqa: E402
import iwanim  # noqa: E402
import iwasm  # noqa: E402
import iwgfx as gfx  # noqa: E402
import levelpatch as lp  # noqa: E402
import mulekick_hud as hud  # noqa: E402
import pvrtc  # noqa: E402
import waw_anim  # noqa: E402
import waw_asm  # noqa: E402
import waw_dz  # noqa: E402
import waw_gun  # noqa: E402

MAPS = ('kino', 'ascension', 'callofthedead')

# World at War gun -> how it is built:
#   machines: the Black Ops gun whose arms and gun state machines are copied. It must be one Black Ops converted from
#             World at War, so that its states are World at War's: the MP40, or the Colt for pistols.
#   spec:     the Black Ops gun whose CPlayerWeapon is copied (by its spec's hash name), for the kind of gun: fire mode
#             (behaviour), recoil, accuracy, ADS zoom, sounds and HUD icon. upgrade: the Pack-a-Punch spec copied
#             (default: the spec's own Upgrade).
#   damage:   'waw' (default): World at War's, see build_gun; 'base': the copied spec's (shotguns: Black Ops counts
#             damage per pellet)
SMG = {'machines': 'mp40', 'spec': 'mp40'}
AR = {'machines': 'mp40', 'spec': 'galil'}
MG = {'machines': 'mp40', 'spec': 'hk21'}
SEMI = {'machines': 'mp40', 'spec': 'kqv'}  # the M14 (its spec hash; 'kqv' has the same djb2)
# shotguns: Black Ops' shotgun specs load a shell each time the reload calls DoReload (the MP40's reload calls it once,
# which fills a magazine but loads one shell). The double-barrels are built on the Olympia's machines: its empty reload
# calls it twice, its partial reload once; World at War's two reloads are just that (its shell bones: the longer one
# swaps both shells, the shorter one the right one), and go there by length as everywhere.
SHOTGUN = {'machines': 'olympia', 'spec': 'olympia', 'damage': 'base', 'ref': 'shotgun'}  # (the double-barrel)
# bolt and pump guns: World at War works the bolt / pump in states of its own after each shot (#2073c15c from the
# hip, #e1f3dcb5 aimed); Black Ops, like its pump shotgun (the Stakeout), has it in the fire animation and fires
# again only back in idle. So the fire states get World at War's fire and bolt animations joined.
BOLT = {'machines': 'mp40', 'spec': 'kqv', 'bolt': True}
# the Trench Gun reloads a shell at a time, in a loop, until the tube is full or the ammo runs out, as Black Ops'
# Stakeout and HS10 do (their spec's loopedReload; the game sends 'reload' 3 to start, then 6 when done): built on the
# Stakeout (its spec and machines), with World at War's reload script in the loop states (LOOPED); its damage stays the
# Olympia's scaled by World at War's Trench Gun over its double-barrel (damage_spec: whose damage 'base' scales; the
# Stakeout's is twice the Olympia's).
PUMP = {'machines': 'stakeout', 'spec': 'stakeout', 'damage': 'base', 'ref': 'shotgun', 'damage_spec': 'olympia',
        'bolt': True, 'looped': True}
PISTOL = {'machines': 'colt45', 'spec': 'colt45', 'upgrade': 0x0f3d080c}
# the Wunderwaffe: Black Ops has it (World at War's model and animations, converted) on Call of the Dead only, with
# its own fire mode (behaviour 17: the chain lightning) and projectile, and no Pack-a-Punch version. It is built on
# that one, taken from Call of the Dead's groups (source) for every map, its projectile spec copied into the map's
# projectile group (it names only resources every map has: an ingame material and texture, and the
# impactWunderWaffe effect); its Pack-a-Punch version is a second copy of it ('self') with World at War's upgrade.
# the flamethrower: Black Ops has no flamethrower and no flame effect (World at War draws its flame in code), so it is
# an automatic gun firing incendiary rounds (Black Ops' Hades shotgun's: zombies hit catch fire) with the Hades' fiery
# muzzle flash and spread; World at War's model, animations, sounds, damage and fire timing. World at War runs it on
# heat and throws spheres of fire; src/flame.cpp rebuilds that from World at War's code: the clip of 100 is filled
# every frame (emptied while overheated) and there is no reserve (FLAME_AMMO), its shots fire no bullets, World at
# War's flame does the damage through the game's own bullet hit (so the incendiary rounds set zombies alight) and is
# drawn in the 3D world, and the HUD shows the heat gauge in place of the ammo counter.
FLAME = {'machines': 'mp40', 'spec': 'mp40', 'flame': 'hades'}
FLAME_AMMO = {'spec': (100, 0), 'upgrade': (100, 0)}
# Its sound: World at War's fire state plays an ignition (2 s) and the flame (its gun record's fire sound, 6 s) as it
# is entered, and stops the flame as it is left (playSound, playSound, stopSound). Black Ops plays a gun's
# fireSoundSequence with every shot (FUN_02638da0), and the Pack-a-Punch shot (pap_shot_st) after it when
# m_playPAPSound is set: so the flamethrower has neither (0 is no sound), and src/flame.cpp plays these two specs
# (World at War's hashes, converted as fire sounds) as World at War does.
FLAME_SOUNDS = (0xae363ff4, 0x7629b1ba)  # ignition, flame
# Its bolt's explosion (directHitSpecial 3, eSpawnChainLightning) spawns the entity named 'chain_lightning'
# (CExplosion, 0x025da0a6: CChainLightning, 10 jumps within 500 units), which fires a
# 'projectile_wunderwaffe_trail_dummy_bullet' from each zombie to the next (0x02583188) and kills them one by one: both
# are Call of the Dead's (its projectile group) too, so they go with the bolt (CHAIN), or the bolt kills only the zombie it
# hits. (They name nothing else: the dummy bullet's trail material and smoke are the bolt's.)
# Experiment (2026-11-17): Call of the Dead's Wunderwaffe is Black Ops' Lightning Bolt power-up gun (CPickupLightningBolt
# gives it; no mystery box has it), and ours, from the box, acted like one: not kept in a slot, gone after a switch. Its
# fire mode 17 (CWunderWaffeBehaviour) is the one power-up trait copied, so it fires with the Ray Gun's projectile mode
# (behaviour 9) instead; the chain lightning comes from the bolt's explosion, not the fire mode. (Mode 17 also drew
# the bulb glows.)
WONDER = {'machines': 'wunderwaffe', 'spec': 'wunderwaffe', 'source': 'callofthedead', 'upgrade': 'self',
          'damage': 'base', 'projectile': True, 'behaviour': 9}
CHAIN = ('chain_lightning', 'projectile_wunderwaffe_trail_dummy_bullet')
# World at War's Wunderwaffe (0x1d9f2c, 0x148ac0, 0x1da2b8) kills up to 10 zombies (normal and upgraded alike) within
# a ball growing from the hit at 1600 mm/s for 1 s (upgraded 2000 mm/s for 1.5 s), each in sight of the nearest one
# killed before it. Black Ops' chain jumps up to 10 times, each within CChainLightning m_jumpRange (Call of the Dead:
# 500.0): where the chain is copied in (every map but Call of the Dead, whose own Wunderwaffe DG-2 keeps it), each
# jump reaches World at War's 1.6 m (the bolt names the chain, so the upgrade shares it).
CHAIN_JUMP_RANGE = 160.0  # the Python's Pack-a-Punch (the Colt's
#                                                                           is Mustang & Sally, a grenade launcher)
GUNS = {
    'thompson': SMG, 'type100': SMG, 'ppsh': SMG,
    'stg44': AR, 'bar': AR, 'fg42': AR,
    'mg42': MG, 'browning': MG,
    'carbine': SEMI, 'g43': SEMI, 'garand': SEMI, 'ptrs': SEMI,
    'shotgun': SHOTGUN, 'sawnoff': SHOTGUN,
    'magnum': PISTOL,
    'kar98': BOLT, 'springfield': BOLT, 'arisaka': BOLT,
    'trenchgun': PUMP,
    'wunderwaffe': WONDER,
    'flamethrower': FLAME,
}
# The Colt, MP40 and Ray Gun are Black Ops' own already.

# a Black Ops machine state -> the World at War arms state whose animation (arms machine) or playGunAnim (gun machine)
# it gets; states both games have match by name. Black Ops' 'reloading' is its longer reload and #0467c906 the shorter
# (MP40, Colt): World at War's two reload states get the same split by length.
EMPTY, PARTIAL = 'longer reload', 'shorter reload'
STATE_MAP = {
    gfx.djb2('shooting'): 0xb93eecb4,  # the gun machine's fire
    0xf1ccdfa0: 0x88413cf8,            # aim in
    0x987c8e59: gfx.djb2('aiming'),    # aimed idle
    0x0486908a: 0xfddafd0d,            # aimed fire (arms machine)
    0xa8c58e88: 0xfddafd0d,            # aimed fire (gun machine)
    0x987a00b3: 0xdc73afe2,            # aim out
    gfx.djb2('reloading'): EMPTY,
    0x0467c906: PARTIAL,
}
WAW_RELOADS = (gfx.djb2('reloading'), 0xaa276068)
# a looped reload: Black Ops' start (loads the first shell: DoReload partway, then on to the loop unless 'reload' 6 says
# the gun is full), loop (a shell each time round) and end states (arms and gun machines alike) -> World at War's
# (its start only opens up, its 'reloading' loads a shell each time round, its end closes up). Black Ops' start gets
# World at War's start and one round of its loop joined, its DoReload moved to that round's.
LOOP_START, LOOP, LOOP_END = 0xf0ee6a49, 0x33b77d13, 0x0ebbce4a
LOOPED = {LOOP_START: 0xab284e07, LOOP: gfx.djb2('reloading'), LOOP_END: 0x2098d650}
C_VARIATION = gfx.djb2('CSoundVariation')  # LFER: one field (a list of sound specs, one picked at random)
WAW_FIRE = 0xb93eecb4
WAW_AIM_FIRE = 0xfddafd0d
WAW_BOLT, WAW_AIM_BOLT = 0x2073c15c, 0xe1f3dcb5  # World at War's bolt / pump states (from the hip, aimed)
BO_FIRE, BO_AIM_FIRE = 0xb93eecb4, 0x0486908a     # the arms machine's fire states
BO_GUN_FIRE, BO_GUN_AIM_FIRE = gfx.djb2('shooting'), 0xa8c58e88  # the gun machine's
F_RATE = gfx.djb2('autoROF')  # CPlayerWeapon's fire rate (rounds a minute: MP40 535, AK74u 750, G11 1250)
RATE_REF = (535, 116)    # Black Ops' MP40: 535 a minute; World at War's MP40 fire state: 116 ms
# World at War's stats on Black Ops' scale, set by the MP40 both games have (Black Ops' is World at War's: damage
# 100 / x4 head, clip 32 / 192):
#   spread: World at War's (0x20ed80-0x20ef68) is the gun record's +0x94 x6: moving-hip and still-hip targets, ADS,
#           a cap, added per shot and recovery. Black Ops' MP40 m_spreadMin / m_spreadMax 0.22 / 0.45 for World at
#           War's MP40 still-hip target 56 / cap 125, the others in proportion (SPREAD_REF); shotguns keep the
#           Olympia's pellet spread
# and World at War's damage function (0x238a40), with Black Ops' equivalents (FUN_0263a8d0, FUN_026389b8):
#   range: the near damage (+0x84) holds to +0x68 mm, then drops +0x6c / 4096 a mm down to +0x78. Black Ops'
#          m_falloff (the Olympia's, copied where missing): full damage to m_startDistance, m_damageMod % of it from
#          m_endDistance on (Black Ops' units are cm)
#   penetration: each zombie a bullet passes through takes +0x64 / 4096 of the near damage off, never below +0x78, up
#          to +0x54 zombies (20; shotguns 10): m_damageFalloff (%), m_damageMin, m_penetrationRating
#   body parts: +0xc8 lists multipliers by part: 0 the head, 3 the chest (the largest after the head on every gun),
#          1-2 and 4-5 the arms and legs in pairs: m_headDamage, m_torsoDamage, m_limbDamage (their mean)
#   fire mode: World at War's full-auto flag (+0x5e); the action timings (fire, reload, raise, put away, aim in / out):
#   World at War's state speeds and lengths
SPREAD_REF = ((0.22, 56), (0.45, 125))  # (Black Ops' MP40, World at War's MP40): still-hip target, cap
BEH_SEMI, BEH_AUTO = 1, 2
# Pack-a-Punch damage, on top of World at War's own upgrade (its upgraded guns mostly keep the normal gun's damage
# numbers; some raise them: FG42, MG42, Browning, G43, Garand, PTRS, Magnum, Kar98k, the shotguns): x3 on the body and
# limbs and x3.75 on the head, Black Ops' biggest Pack-a-Punch step (the M14's: 131 -> 400 / 315 -> 1200; its MP40's
# is x2 / x2.5), so that the upgraded World at War guns hit clearly harder than Black Ops' upgraded ones. Shotguns (per
# pellet) get x3 on the normal gun's damage times World at War's upgrade over normal near damage (Black Ops' Hades
# keeps the Olympia's 80; its Raid and SPAS upgrades do 300 for 160).
PAP_BODY, PAP_HEAD = 3.0, 3.75

MODEL_SCALE = 16    # Black Ops weapon model units per World at War's
SKEL_SCALE = 6.4    # skeleton, skin and animation positions
BO_SKEL_X20 = 0x4000
BO_BONE_X18 = 0x400

IPA_DIRS = [os.path.join(ROOT, '..', '..', 'cod waw'), os.path.join(ROOT, '..', '..')]
CACHE = os.path.join(ROOT, 'out', 'waw')
WAW_PLAYER = 'ingame/player/player.group.bin'
WAW_SFX = 'ingame/sound/sfx.group.bin'
WAW_UI, WAW_FIXED = 'ingame/ui/ui.group.bin', 'fixed/fixed.group.bin'
WAW_FILES = (WAW_PLAYER, WAW_SFX, WAW_UI, WAW_FIXED)
INGAME = 'ingame/ingame.group.bin'  # Black Ops' weapon sounds are here (loaded on every map)
C_ITEMGUN = 0x81c32701
C_MATSET = 0x13d5a75f
C_PLAYERWEAPON = gfx.djb2('CPlayerWeapon')
C_BOX, F_WEAPONS = lp.djb2('CMysteryBox'), lp.djb2('m_Weapons')
ASM, ANIM = gfx.H['CIsAnimStateMachine'], gfx.H['CIwAnim']
MUZZLE = gfx.djb2('muzzleFlash')
# sounds (World at War: ingame/sound/sfx.group.bin; Black Ops: ingame/ingame.group.bin)
C_SEQUENCE = gfx.djb2('CSoundSequence')       # a list of commands:
C_DELAY = gfx.djb2('CSoundSequenceCommandDelay')     # u32 0, u32 ms (both games)
C_PLAY = gfx.djb2('CSoundSequenceCommandPlayAsync')  # u32 0, u32 sound spec; Black Ops adds u32 (CSoundVariation),
#                                                       u8
SOUND_GROUP = gfx.djb2('ingame')
# HUD icons. Black Ops' HUD draws the current weapon with the weapon_icon element (class CUIWeaponIcon, in three HUD
# layouts in ingame.group.bin), whose uv rectangle the game sets from the weapon's m_uv0 / m_uv1 (texels; CPlayerWeapon
# +0x2f0, read by 0x02638f7e for 0x02669664); its texture is hud_icons, whose 5 x 9 grid of 177 x 58 weapon cells is
# full. weapon_icon gets a texture of its own: a copy of hud_icons (the same size, so its texel coordinates mean the
# same) keeping the weapon grid, with World at War's icons in cells below it (hud_icons itself, which other HUD parts
# also draw from, stays as it is). World at War's icons: its hud_guns frame (ingame/ui/ui.group.bin) has a bitmap per
# gun, named after it, with a texel rectangle in its UI texture (or in fixed/fixed.group.bin's for the Type 100,
# Wunderwaffe and Arisaka); the flamethrower has a fuel gauge there instead, so its icon is the silhouette in the UI
# texture no gun names.
ICON_TEXTURE = gfx.djb2('waw_hud_icons')
ICON_CELL = (177, 58)        # Black Ops' weapon cells
ICON_GRID = (885, 522)       # the weapon grid's extent in hud_icons (kept in the copy)
ICON_FIT = (165, 50)         # an icon's largest size in its cell
ICON_EXTRA = {'flamethrower': (0x2f66d01c, 5, 410, 79, 436)}
C_WEAPON_ICON, WEAPON_ICON = gfx.djb2('CUIWeaponIcon'), gfx.djb2('weapon_icon')  # what Black Ops' sound specs name (World at War's: 'sfx')
ARMS_SKEL = 0xf2700b67  # the first-person arms' skeleton (the same in both games)


def F(name):
    return gfx.djb2(name)


def new_hash(name):
    return gfx.djb2('waw_' + name)



def weapons_path(m):
    return 'ingame/weapons/weapons_%s.group.bin' % m


def arms_path(m):
    return 'ingame/characters/arms/arms_%s.group.bin' % m


def projectiles_path(m):
    return 'ingame/weapons/weapon_projectiles_%s.group.bin' % m


def statics_path(m):
    return 'levels/%s/%s_statics.group.bin' % (m, m)


# ---------------------------------------------------------------- World at War data
def find_ipa():
    env = os.environ.get('WAW_IPA')
    if env:
        return env if os.path.exists(env) else None
    for d in IPA_DIRS:
        if os.path.isdir(d):
            for n in sorted(os.listdir(d)):
                if n.lower().endswith('.ipa') and 'world at war' in n.lower():
                    return os.path.join(d, n)
    return None


def ipa_stamp(ipa):
    st = os.stat(ipa)
    return '%s %d' % (os.path.basename(ipa), st.st_size)


def waw_files():
    """{path: bytes} of WAW_FILES from World at War's _main.dz, or None without the IPA (cached in out/waw)"""
    ipa = find_ipa()
    if not ipa:
        return None
    cached = {p: os.path.join(CACHE, os.path.basename(p)) for p in WAW_FILES}
    stamp = os.path.join(CACHE, 'src.txt')
    if all(os.path.exists(c) for c in cached.values()) and os.path.exists(stamp) and \
            open(stamp).read() == ipa_stamp(ipa):
        return {p: open(c, 'rb').read() for p, c in cached.items()}
    os.makedirs(CACHE, exist_ok=True)
    tmp = os.path.join(CACHE, '_main.dz')
    with zipfile.ZipFile(ipa) as z:
        name = next(n for n in z.namelist() if n.endswith('.app/_main.dz'))
        open(tmp, 'wb').write(z.read(name))
    files = waw_dz.files(tmp)
    os.remove(tmp)
    out = {p: files[p] for p in WAW_FILES}
    for p, c in cached.items():
        open(c, 'wb').write(out[p])
    open(stamp, 'w').write(ipa_stamp(ipa))
    return out


# ---------------------------------------------------------------- reflected records (CIsEntitySpec)
class Lfer:
    """An LFER block: 'LFER', class, size, field count, fields (levelpatch.Field). `short`: how much the size field
    undercounts the block (top-level components' sizes leave out a trailing scale field on some transforms)."""

    def __init__(self, data):
        assert data[:4] == b'LFER'
        self.cls, size, nf = struct.unpack_from('<3I', data, 4)
        r = 16
        self.fields = []
        for _ in range(nf):
            fc, t, a, fh, b, ln = struct.unpack_from('<6I', data, r)
            self.fields.append(lp.Field(fc, t, a, fh, b, data[r + 24:r + 24 + ln]))
            r += 24 + ln
        self.short = r - size
        self.length = r

    def pack(self):
        body = b''.join(f.pack() for f in self.fields)
        return b'LFER' + struct.pack('<3I', self.cls, 16 + len(body) - self.short, len(self.fields)) + body

    def field(self, name):
        h = F(name) if isinstance(name, str) else name
        return next((f for f in self.fields if f.fh == h), None)

    def get_u32(self, name):
        return struct.unpack('<I', self.field(name).data)[0]

    def set_u32(self, name, v):
        self.field(name).data = struct.pack('<I', v & 0xffffffff)

    def set_str(self, name, s):
        self.field(name).data = s.encode() + b'\0'

    def sub(self, name):
        return Lfer(self.field(name).data)

    def set_sub(self, name, lf):
        self.field(name).data = lf.pack()


class Spec:
    """CIsEntitySpec record body: u32 component count, components ([u32 x2, class, size] + LFER), then the rest"""

    def __init__(self, body):
        n = struct.unpack_from('<I', body)[0]
        o = 4
        self.comps = []
        for _ in range(n):
            pre1, pre2, cls, sz = struct.unpack_from('<4I', body, o)
            lf = Lfer(body[o + 16:o + 16 + sz])
            assert lf.length == sz
            self.comps.append((pre1, pre2, lf))
            o += 16 + sz
        self.tail = body[o:]
        assert self.write() == body, 'spec does not round-trip'

    def comp(self, cls):
        return next(lf for _, _, lf in self.comps if lf.cls == cls)

    def write(self):
        out = struct.pack('<I', len(self.comps))
        for pre1, pre2, lf in self.comps:
            blob = lf.pack()
            out += struct.pack('<4I', pre1, pre2, lf.cls, len(blob)) + blob
        return out + self.tail


def set_asset(weapon, name, model):
    if not weapon.field(name):
        return
    a = weapon.sub(name)
    a.set_u32('model', model)
    a.set_u32('skin', model)
    weapon.set_sub(name, a)


def set_lods(weapon, model):
    f = weapon.field('LODs')
    if not f:
        return
    lods = Lfer(f.data)
    lst = lods.field('m_Assets')
    n = struct.unpack_from('<I', lst.data)[0]
    o, items = 4, []
    for _ in range(n):
        it = Lfer(lst.data[o:])
        o += it.length
        it.set_u32('model', model)
        it.set_u32('skin', model)
        items.append(it.pack())
    assert o == len(lst.data)
    lst.data = struct.pack('<I', n) + b''.join(items)
    f.data = lods.pack()


# ---------------------------------------------------------------- conversion
def anim_length(body, waw=False):
    """last keyframe time (1/4096 s)"""
    if waw:
        return waw_anim.Anim(body).keys[-1].time
    a = iwanim.Anim(body)
    return struct.unpack_from('<i', a.blob, a.keys[-1][0])[0]


# ---- re-gripping Black Ops arm animations made for the base gun
# The arms skeleton hangs TAG_WEAPON (where the gun goes) off tag_torso, beside the arms, not off a hand: each gun's
# animations put it where that gun's grip meets the hands. The base gun's own Black Ops-only animations (the MP40's
# sprint set) are kept, with TAG_WEAPON moved by the difference between the two guns' grips: the gun's transform in
# the right wrist's frame in each gun's idle pose. (Transforms: (3x3 rotation, position), as iwgfx.Skel.world.)
def _qmat(q):
    w, x, y, z = [c / 4096.0 for c in q]
    n = (w * w + x * x + y * y + z * z) ** 0.5 or 1.0
    w, x, y, z = w / n, x / n, y / n, z / n
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def _matq(R, like):
    """rotation matrix -> (w, x, y, z) int16 (4096 = 1), on the same side as quaternion `like`"""
    t = R[0][0] + R[1][1] + R[2][2]
    if t > 0:
        s = (t + 1.0) ** 0.5 * 2
        q = (0.25 * s, (R[2][1] - R[1][2]) / s, (R[0][2] - R[2][0]) / s, (R[1][0] - R[0][1]) / s)
    elif R[0][0] > R[1][1] and R[0][0] > R[2][2]:
        s = (1.0 + R[0][0] - R[1][1] - R[2][2]) ** 0.5 * 2
        q = ((R[2][1] - R[1][2]) / s, 0.25 * s, (R[0][1] + R[1][0]) / s, (R[0][2] + R[2][0]) / s)
    elif R[1][1] > R[2][2]:
        s = (1.0 + R[1][1] - R[0][0] - R[2][2]) ** 0.5 * 2
        q = ((R[0][2] - R[2][0]) / s, (R[0][1] + R[1][0]) / s, 0.25 * s, (R[1][2] + R[2][1]) / s)
    else:
        s = (1.0 + R[2][2] - R[0][0] - R[1][1]) ** 0.5 * 2
        q = ((R[1][0] - R[0][1]) / s, (R[0][2] + R[2][0]) / s, (R[1][2] + R[2][1]) / s, 0.25 * s)
    if sum(a * b for a, b in zip(q, like)) < 0:
        q = tuple(-c for c in q)
    return tuple(int(round(c * 4096)) for c in q)


def _mul(a, b):
    (RA, PA), (RB, PB) = a, b
    return ([[sum(RA[i][k] * RB[k][j] for k in range(3)) for j in range(3)] for i in range(3)],
            [PA[i] + sum(RA[i][k] * PB[k] for k in range(3)) for i in range(3)])


def _inv(a):
    R, P = a
    RT = [[R[j][i] for j in range(3)] for i in range(3)]
    return RT, [-sum(RT[i][k] * P[k] for k in range(3)) for i in range(3)]


def _key_bones(anim, ki):
    """Black Ops CIwAnim keyframe ki -> {bone index: (blob offset of its entry, pos or None, quat)}"""
    o, n, flags = anim.keys[ki]
    if flags & 4:  # every animated bone
        bones = [i for i in range(anim.nbones) if anim.mask[i // 32] >> (i % 32) & 1][:n]
    else:          # some: their bone indices follow the entries
        end = o + iwanim.KF_HEADER + (n * 8 if flags & 1 else n * 14)
        bones = list(anim.blob[end:end + n])
    out, p = {}, o + iwanim.KF_HEADER
    for b in bones:
        if flags & 1:
            out[b] = (p, None, struct.unpack_from('<4h', anim.blob, p))
            p += 8
        else:
            out[b] = (p, struct.unpack_from('<3h', anim.blob, p), struct.unpack_from('<4h', anim.blob, p + 6))
            p += 14
    return out


def _grip(anim_body, skel):
    """the TAG_WEAPON transform in the right wrist's frame, in the animation's first key"""
    anim = iwanim.Anim(anim_body)
    key = _key_bones(anim, 0)
    byhash = {b.hash: b for b in skel.bones}
    byindex = {b.index: b for b in skel.bones}
    world = {}

    def get(i):
        if i not in world:
            b = byindex[i]
            if i in key and key[i][1] is not None:
                local = (_qmat(key[i][2]), list(key[i][1]))
            else:
                local = (_qmat(gfx.quat_unpack(b.rot)), list(b.pos))
            world[i] = _mul(get(byhash[b.parent].index), local) if b.parent in byhash else local
        return world[i]

    tag = byhash[gfx.djb2('TAG_WEAPON')].index
    wrist = byhash[gfx.djb2('j_wrist_ri')].index
    return _mul(_inv(get(wrist)), get(tag))


def regrip(anim_body, skel, delta):
    """the animation with TAG_WEAPON's local transform followed by `delta` (in the gun's frame) in every key; None
    if it does not move TAG_WEAPON"""
    anim = iwanim.Anim(anim_body)
    tag = next(b.index for b in skel.bones if b.hash == gfx.djb2('TAG_WEAPON'))
    if not anim.mask[tag // 32] >> (tag % 32) & 1:
        return None
    for ki in range(anim.nkeys):
        entry = _key_bones(anim, ki).get(tag)
        if entry is None:  # a key that leaves TAG_WEAPON to its neighbours
            continue
        p, pos, q = entry
        if pos is None:    # rotation only
            R, _ = _mul((_qmat(q), [0, 0, 0]), delta)
            struct.pack_into('<4h', anim.blob, p, *_matq(R, q))
            continue
        R, P = _mul((_qmat(q), list(pos)), delta)
        struct.pack_into('<3h', anim.blob, p, *[max(-32768, min(32767, int(round(c)))) for c in P])
        struct.pack_into('<4h', anim.blob, p + 6, *_matq(R, q))
    return anim.write()


# ---- the view
# Black Ops' game camera hangs off the arms' tag_camera bone (the player spec's CIsBoneAttachment, ingame.group.bin), so
# whatever an arms animation does to that bone, or to the bones above it (the root, and the root's child the camera hangs
# from), moves the view. Black Ops' gun animations keep all three at rest (every arms machine of its rifles, SMGs, MGs
# and shotguns: not a degree, not a unit; only its knife lunges and the Colt's reload turn the camera, and nothing moves
# it). World at War's kick and swing it: aimed fire moves it on the Thompson, BAR, shotguns, Magnum, bolt actions and
# Trench Gun (up to 4 degrees and 630 units; the Thompson's and BAR's move the root too), the MG42's aim in turns it,
# hip fire moves it up to 2572 units (shotguns), reloads swing it up to 14 degrees. In Black Ops all that comes on top
# of its own recoil, and the view wanders while aiming. So every converted arms animation gets the three bones at rest
# and the camera's movement put into tag_torso (which carries both arms and TAG_WEAPON) instead: the arms and gun move
# in the view as they did in World at War, the view itself only as Black Ops' guns move it.


def still_camera(anim_body, skel):
    """the arms animation with tag_camera and the bones above it at rest, tag_torso moved to keep its place in the
    camera's frame in every key"""
    anim = iwanim.Anim(anim_body)
    byhash = {b.hash: b for b in skel.bones}
    byindex = {b.index: b for b in skel.bones}
    cam, torso = byhash[gfx.djb2('tag_camera')], byhash[gfx.djb2('tag_torso')]
    chain = []
    b = cam
    while True:
        chain.append(b.index)
        if b.parent not in byhash:
            break
        b = byhash[b.parent]
    root = chain[-1]
    assert byhash[torso.parent].index == root, 'tag_torso does not hang off the root'
    bind = {i: (_qmat(gfx.quat_unpack(byindex[i].rot)), list(byindex[i].pos)) for i in chain + [torso.index]}
    rest_cam = None
    for i in reversed(chain):
        rest_cam = bind[i] if rest_cam is None else _mul(rest_cam, bind[i])
    for ki in range(anim.nkeys):
        key = _key_bones(anim, ki)
        if not any(i in key for i in chain):
            continue

        def local(i):
            if i in key:
                p, pos, q = key[i]
                return _qmat(q), list(pos) if pos is not None else bind[i][1]
            return bind[i]

        w_cam = None
        for i in reversed(chain):
            w_cam = local(i) if w_cam is None else _mul(w_cam, local(i))
        w_torso = _mul(local(root), local(torso.index))
        near = lambda a, c: all(abs(a[0][r][k] - c[0][r][k]) < 1e-4 for r in range(3) for k in range(3)) and all(
            abs(a[1][k] - c[1][k]) < 0.5 for k in range(3))
        moved = not near(w_cam, rest_cam) or (root in key and not near(local(root), bind[root]))
        for i in chain:
            if i in key:
                p, pos, q = key[i]
                if pos is not None:
                    struct.pack_into('<3h', anim.blob, p, *[int(round(c)) for c in bind[i][1]])
                struct.pack_into('<4h', anim.blob, p + (0 if pos is None else 6), *_matq(bind[i][0], q))
        if not moved or torso.index not in key:
            continue
        # tag_torso keeps its place relative to the camera: new = rest camera x old camera^-1 x old (entity space),
        # then into the (resting) root's frame
        new = _mul(_inv(bind[root]), _mul(rest_cam, _mul(_inv(w_cam), w_torso)))
        p, pos, q = key[torso.index]
        if pos is not None:
            struct.pack_into('<3h', anim.blob, p, *[max(-32768, min(32767, int(round(c)))) for c in new[1]])
        struct.pack_into('<4h', anim.blob, p + (0 if pos is None else 6), *_matq(new[0], q))
    return anim.write()


# ---- the aim (ADS) offset
# World at War's engine moves the view by its CItemGun's offset_ac (+0xac, model units) while aiming; Black Ops' does
# not, and has it baked into the aim animations instead: Black Ops' MP40 aims with World at War's animations moved by
# (-336, -123, +4914) in camera space (skeleton units), World at War's MP40 offset being (0, 2, -85) -> -64 x it =
# (0, -128, +5440). So each gun's aim animations get -ADS_SCALE x its offset, through tag_torso (which carries both arms
# and TAG_WEAPON; the camera hangs off the root beside it): in full in the aimed states, ramping in over aim in and out
# over aim out.
ADS_SCALE = 64       # skeleton units (Black Ops) per World at War model unit: 16 x 4
AIM_FULL = (0x987c8e59, 0x0486908a)  # aimed idle, aimed fire
AIM_IN, AIM_OUT = 0xf1ccdfa0, 0x987a00b3


def shift_view(anim_body, skel, offset, ramp=None):
    """the animation with the arms and gun (tag_torso) moved by `offset` (camera space, skeleton units); ramp: 'in'
    (0 at the first key to 1 at the last) or 'out' (1 to 0), else all of it"""
    anim = iwanim.Anim(anim_body)
    byhash = {b.hash: b for b in skel.bones}
    byindex = {b.index: b for b in skel.bones}
    torso = byhash[gfx.djb2('tag_torso')]
    cam = byhash[gfx.djb2('tag_camera')].index
    times = [struct.unpack_from('<i', anim.blob, o)[0] for o, _, _ in anim.keys]
    t0, t1 = times[0], times[-1]
    for ki in range(anim.nkeys):
        key = _key_bones(anim, ki)
        world = {}

        def get(i):
            if i not in world:
                b = byindex[i]
                if i in key and key[i][1] is not None:
                    local = (_qmat(key[i][2]), list(key[i][1]))
                else:
                    local = (_qmat(gfx.quat_unpack(b.rot)), list(b.pos))
                world[i] = _mul(get(byhash[b.parent].index), local) if b.parent in byhash else local
            return world[i]

        if torso.index not in key or key[torso.index][1] is None:
            continue
        k = 1.0
        if ramp and t1 > t0:
            f = (times[ki] - t0) / (t1 - t0)
            k = f if ramp == 'in' else 1.0 - f
        R = get(cam)[0]
        d = [k * sum(R[i][j] * offset[j] for j in range(3)) for i in range(3)]  # world space
        P = get(byhash[torso.parent].index)[0] if torso.parent in byhash else [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
        dl = [sum(P[j][i] * d[j] for j in range(3)) for i in range(3)]          # tag_torso's parent space
        p, pos, q = key[torso.index]
        struct.pack_into('<3h', anim.blob, p, *[max(-32768, min(32767, int(round(pos[i] + dl[i])))) for i in range(3)])
    return anim.write()


# ---- lining up the sights
# Black Ops' own guns aim with the barrel exactly along the view (0.00 degrees yaw and pitch in every aim pose; it turned
# its MP40, converted from World at War, by a fraction of a degree to get there) and the front sight's tip at the middle
# of the screen (within a degree). World at War's aim poses are a degree or two off (up to 2.4: STG-44, BAR,
# shotguns), and the misses show, at Black Ops' narrow aim zoom (30-40 degrees). So each gun's aim animations get, on
# top of the World at War offset, the turn of TAG_WEAPON (the gun about its origin, a couple of degrees at most: the
# hands stay on it) that puts the barrel along the view, then the move of the arms and gun (tag_torso) that puts the
# front sight's tip in the middle, both measured in the aimed pose; in full while aimed, ramping in over aim in and out
# over aim out. KEEP_AIM: guns whose World at War aim pose is kept (with its offset): rear aperture rings close to the
# eye (M1 Carbine, Garand: World at War centres the ring, and centring the front post would push the ring off), the
# Browning's tall rear sight leaf, the PTRS's sights off to the left of the barrel, and the guns with no sights.
KEEP_AIM = ('carbine', 'garand', 'browning', 'ptrs', 'wunderwaffe', 'flamethrower')


def front_sight(verts):
    """the front sight's tip (model units): the highest vertex in the front quarter of the gun near its centre line"""
    zs = [v[2] for v in verts]
    z0, z1 = min(zs), max(zs)
    length = z1 - z0
    xs = sorted(v[0] for v in verts)
    xc = xs[len(xs) // 2]
    front = [v for v in verts if v[2] > z1 - 0.25 * length and abs(v[0] - xc) < 0.012 * length]
    return max(front, key=lambda v: v[1]) if front else None


def _pose(anim_body, skel, ki=0):
    """world transforms {bone index: (R, P)} in key ki"""
    anim = iwanim.Anim(anim_body)
    key = _key_bones(anim, ki)
    byhash = {b.hash: b for b in skel.bones}
    byindex = {b.index: b for b in skel.bones}
    world = {}

    def get(i):
        if i not in world:
            b = byindex[i]
            if i in key and key[i][1] is not None:
                local = (_qmat(key[i][2]), list(key[i][1]))
            else:
                local = (_qmat(gfx.quat_unpack(b.rot)), list(b.pos))
            world[i] = _mul(get(byhash[b.parent].index), local) if b.parent in byhash else local
        return world[i]

    for i in byindex:
        get(i)
    return world


def _rot_between(a, b):
    """the rotation matrix turning unit vector a onto unit vector b"""
    v = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
    c = sum(x * y for x, y in zip(a, b))
    k = 1.0 / (1.0 + c) if c > -0.999999 else 0.0
    vx = [[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]]
    vx2 = [[sum(vx[i][m] * vx[m][j] for m in range(3)) for j in range(3)] for i in range(3)]
    return [[(1.0 if i == j else 0.0) + vx[i][j] + vx2[i][j] * k for j in range(3)] for i in range(3)]


def aim_correction(aimed_body, skel, verts, sights=True):
    """(the gun's turn in TAG_WEAPON's frame, the arms' move in camera space (skeleton units)) that line up the
    sights in the aimed pose"""
    world = _pose(aimed_body, skel)
    byhash = {b.hash: b for b in skel.bones}
    cam, tag = world[byhash[gfx.djb2('tag_camera')].index], world[byhash[gfx.djb2('TAG_WEAPON')].index]
    gun = _mul(_inv(cam), tag)                                  # the gun in camera space
    z = [gun[0][i][2] for i in range(3)]
    turn_cam = _rot_between(_unit3(z), [0.0, 0.0, 1.0])          # barrel onto the view axis (camera space)
    R = gun[0]
    RT = [[R[j][i] for j in range(3)] for i in range(3)]
    turn_gun = [[sum(RT[i][a] * sum(turn_cam[a][b] * R[b][j] for b in range(3)) for a in range(3)) for j in range(3)]
                for i in range(3)]                               # the same turn in the gun's own frame
    move = [0.0, 0.0, 0.0]
    tip = front_sight(verts) if sights else None
    if tip:
        new = ([[sum(turn_cam[i][k] * R[k][j] for k in range(3)) for j in range(3)] for i in range(3)], gun[1])
        p = [new[1][i] + sum(new[0][i][k] * 4 * tip[k] for k in range(3)) for i in range(3)]
        move = [-p[0], -p[1], 0.0]
    return turn_gun, move


def _unit3(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


def _qslerp_ident(R, k):
    """the rotation R scaled to the fraction k of its angle (about the same axis)"""
    if k >= 1.0:
        return R
    q = _matq(R, (4096, 0, 0, 0))
    w, x, y, z = [c / 4096.0 for c in q]
    ang = 2 * math.acos(max(-1.0, min(1.0, w)))
    s = math.sqrt(max(0.0, 1 - w * w))
    if s < 1e-9 or ang < 1e-9:
        return [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
    a2 = ang * k / 2
    q2 = (math.cos(a2), x / s * math.sin(a2), y / s * math.sin(a2), z / s * math.sin(a2))
    return _qmat(tuple(c * 4096 for c in q2))


def line_up(anim_body, skel, turn_gun, move, ramp=None):
    """the animation with TAG_WEAPON turned by turn_gun (its own frame) and the arms moved by `move` (camera space),
    in full or ramping in / out over its keys"""
    anim = iwanim.Anim(anim_body)
    byhash = {b.hash: b for b in skel.bones}
    tag, torso = byhash[gfx.djb2('TAG_WEAPON')], byhash[gfx.djb2('tag_torso')]
    cam = byhash[gfx.djb2('tag_camera')].index
    times = [struct.unpack_from('<i', anim.blob, o)[0] for o, _, _ in anim.keys]
    t0, t1 = times[0], times[-1]
    for ki in range(anim.nkeys):
        k = 1.0
        if ramp and t1 > t0:
            f = (times[ki] - t0) / (t1 - t0)
            k = f if ramp == 'in' else 1.0 - f
        key = _key_bones(anim, ki)
        world = _pose(anim_body, skel, ki) if any(move) else None
        if tag.index in key:
            p, pos, q = key[tag.index]
            Rk = _qslerp_ident(turn_gun, k)
            R = [[sum(_qmat(q)[i][m] * Rk[m][j] for m in range(3)) for j in range(3)] for i in range(3)]
            struct.pack_into('<4h', anim.blob, p + (0 if pos is None else 6), *_matq(R, q))
        if any(move) and torso.index in key and key[torso.index][1] is not None:
            Rc = world[cam][0]
            d = [k * sum(Rc[i][j] * move[j] for j in range(3)) for i in range(3)]
            P = world[byhash[torso.parent].index][0] if torso.parent in byhash else [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
            dl = [sum(P[j][i] * d[j] for j in range(3)) for i in range(3)]
            p, pos, q = key[torso.index]
            struct.pack_into('<3h', anim.blob, p, *[max(-32768, min(32767, int(round(pos[i] + dl[i])))) for i in range(3)])
    return anim.write()


def scale_skin_set(st, k):
    """CIwAnimSkinSet data with every bone-space position scaled by k (vertex index words kept as they are)"""
    d = bytearray(st.data)
    if d[1] == 0xFF:
        nb, p = 1, 2
    else:
        nb = 2 if d[2] == 0xFF else 3 if d[3] == 0xFF else 4
        p = 4
    stride = 2 + nb * 6 + (2 if nb < 3 else 4)
    for _ in range(st.nverts):
        for b in range(nb):
            q = p + 2 + 6 * b
            v = struct.unpack_from('<3h', d, q)
            struct.pack_into('<3h', d, q, *[max(-32768, min(32767, int(round(c * k)))) for c in v])
        p += stride
    return bytes(d)


def icon_cell(gun):
    """the texel rectangle of World at War `gun`'s icon in the weapon icon texture"""
    i = list(GUNS).index(gun)
    w, h = ICON_CELL
    x, y = (i % 5) * w, ICON_GRID[1] + (i // 5) * h
    return (x, y), (x + w, y + h)


def waw_icons(waw):
    """{gun: (texture hash, x0, y0, x1, y1)} from World at War's hud_guns frame"""
    ui = gfx.Group(waw[WAW_UI])
    b = next(r for rl in ui.lists() for r in rl.records if r[1] == gfx.djb2('hud_guns'))[2]
    bm, im = struct.pack('<I', gfx.djb2('CIwGuiBitmap')), struct.pack('<I', gfx.djb2('CIwGuiImage'))
    by_hash = {}
    i = b.find(bm)
    while i >= 0:
        name = struct.unpack_from('<I', b, i + 4)[0]
        j = b.find(im, i)
        mat = struct.unpack_from('<I', b, j + 4)[0]
        xa, ya, xb, yb = struct.unpack_from('<4H', b, j + 12)
        by_hash[name] = (mat, min(xa, xb), min(ya, yb), max(xa, xb), max(ya, yb))
        i = b.find(bm, i + 4)
    out = {g: by_hash[gfx.djb2(g)] for g in GUNS if gfx.djb2(g) in by_hash}
    out.update(ICON_EXTRA)
    return out


def icon_texture(waw, hud_icons_body):
    """the weapon icon texture: hud_icons' weapon grid and World at War's icons (RGBA4444, as hud_icons)"""
    import numpy as np
    tex = gfx.Texture.read(ICON_TEXTURE, hud_icons_body)
    im = tex.image
    assert im.fmt == 5 and im.pitch == im.w * 2, (im.fmt, im.pitch)
    px = np.array(hud.rgba4444_to_px(im.texels, im.w, im.h), np.float64).reshape(im.h, im.w, 4)
    keep = np.zeros((im.h, im.w), bool)
    keep[:ICON_GRID[1], :ICON_GRID[0]] = True
    px[~keep] = 0
    sources = {}
    for gp in (WAW_UI, WAW_FIXED):
        g = gfx.Group(waw[gp])
        for c, h, body, _ in g.list_of(gfx.H['CIwTexture']).records:
            sources[h] = body
    for gun, (th, x0, y0, x1, y1) in waw_icons(waw).items():
        t = gfx.Texture.read(th, sources[th])
        w, h, rgba = t.image.to_rgba()
        src = np.frombuffer(rgba, np.uint8).reshape(h, w, 4).astype(np.float64)[y0:y1, x0:x1]
        sh, sw = src.shape[:2]
        k = min(ICON_FIT[0] / sw, ICON_FIT[1] / sh)
        dw, dh = int(round(sw * k)), int(round(sh * k))
        # bilinear, colour premultiplied by alpha
        prem = src.copy()
        prem[:, :, :3] *= prem[:, :, 3:4] / 255
        ys = (np.arange(dh) + 0.5) / k - 0.5
        xs = (np.arange(dw) + 0.5) / k - 0.5
        y0i = np.clip(np.floor(ys).astype(int), 0, sh - 1)
        x0i = np.clip(np.floor(xs).astype(int), 0, sw - 1)
        y1i, x1i = np.clip(y0i + 1, 0, sh - 1), np.clip(x0i + 1, 0, sw - 1)
        fy, fx = np.clip(ys - y0i, 0, 1)[:, None, None], np.clip(xs - x0i, 0, 1)[None, :, None]
        out = (prem[y0i][:, x0i] * (1 - fy) * (1 - fx) + prem[y0i][:, x1i] * (1 - fy) * fx +
               prem[y1i][:, x0i] * fy * (1 - fx) + prem[y1i][:, x1i] * fy * fx)
        a = out[:, :, 3:4]
        out[:, :, :3] = np.where(a > 0, out[:, :, :3] * 255 / np.maximum(a, 1e-6), 0)
        (cx, cy), _ = icon_cell(gun)
        ox, oy = cx + (ICON_CELL[0] - dw) // 2, cy + (ICON_CELL[1] - dh) // 2
        px[oy:oy + dh, ox:ox + dw] = out
    flat = [tuple(int(round(min(255, max(0, c)))) for c in p) for p in px.reshape(-1, 4)]
    im.texels = hud.px_to_rgba4444(flat) + im.texels[im.w * im.h * 2:]
    tex.hash = ICON_TEXTURE
    return tex.write()


def use_icon_texture(data):
    """ingame.group.bin with the weapon_icon elements drawing from the weapon icon texture (their texture property
    holds hud_icons' hash 90 bytes after the element's class)"""
    g = gfx.Group(data)
    cls = struct.pack('<I', C_WEAPON_ICON)
    n = 0
    for rl in g.lists():
        for r in rl.records:
            b = r[2]
            i = b.find(cls)
            while i >= 0:
                if struct.unpack_from('<I', b, i + 4)[0] == WEAPON_ICON:
                    assert struct.unpack_from('<I', b, i + 90)[0] == hud.HUD_ICONS, 'weapon_icon texture'
                    b = b[:i + 90] + struct.pack('<I', ICON_TEXTURE) + b[i + 94:]
                    n += 1
                i = b.find(cls, i + 4)
            r[2] = b
    assert n == 3, n
    return g.write()


def sound_data(body):
    """World at War CIwSoundDataADPCM -> one Black Ops plays. Both games' (Black Ops' Serialise 0x0252ff38, then the
    ADPCM subclass's): u32 data size, u32 sample count, the data (IMA ADPCM, tools/imaadpcm.py), u32 sample rate,
    u16 format (2), u32 samples a block, u32 block size. Black Ops' sounds are all 11025 Hz in 256-byte blocks or
    22050 Hz in 512; World at War's are 22050 / 512 or 44100 Hz in 1024-byte blocks, which are halved to 22050 Hz (a
    [1 2 1] / 4 low-pass, every second sample) and re-encoded in 512-byte blocks to be in a format Black Ops uses."""
    size, count = struct.unpack_from('<2I', body, 0)
    rate, fmt, per_block, align = struct.unpack_from('<IHII', body, 8 + size)
    assert len(body) == 8 + size + 14 and fmt == 2, (len(body), size, fmt)
    if align in (256, 512):
        return body
    assert align == 1024 and rate == 44100, (align, rate)
    pcm = imaadpcm.decode(body[8:8 + size], align)
    half = [(pcm[max(0, i - 1)] + 2 * pcm[i] + pcm[min(len(pcm) - 1, i + 1)]) // 4 for i in range(0, len(pcm), 2)]
    data = imaadpcm.encode(half, 512)
    return (struct.pack('<2I', len(data), (count + 1) // 2) + data +
            struct.pack('<IHII', rate // 2, 2, (512 - 4) * 2 + 1, 512))


class Converter:
    def __init__(self, waw, sounds):
        self.w = gfx.Group(waw[WAW_PLAYER])
        self.sfx = gfx.Group(waw[WAW_SFX])
        self.sounds = sounds  # (class, new hash) -> body, for ingame.group.bin (shared by the maps)
        self.record_class = sounds.setdefault('classes', {})  # (list class, hash) -> the record's own class
        self.guns = {}
        for c, h, body, _ in self.w.list_of(C_ITEMGUN).records:
            g, _ = waw_gun.parse(body)
            self.guns[g['name'].split(b'\0')[0].decode().lower()] = g
        self.matsets = {h: list(struct.unpack_from('<%dI' % struct.unpack_from('<I', body)[0], body, 4))
                        for c, h, body, _ in self.w.list_of(C_MATSET).records}
        self.weapons = {}   # (class, new hash) -> body, in the order added
        self.arms = {}
        self.lengths = {}   # new animation hash -> length
        self.projectile = None
        self.arms_skel = None  # Black Ops' arms skeleton (build_gun reads it from the map's arms group)

    def waw(self, cls, h):
        rec = self.w.find(cls, h)
        assert rec, 'World at War has no %s %08x' % (gfx.CLS.get(cls, '%08x' % cls), h)
        return rec[2]

    def add(self, where, cls, h, body):
        if (cls, h) in where:
            assert where[cls, h] == body, 'two different records for %08x' % h
        where[cls, h] = body
        return h

    def texture(self, th):
        nh = new_hash('tex_%08x' % th)
        if (gfx.H['CIwTexture'], nh) in self.weapons:
            return nh
        t = gfx.Texture.read(th, self.waw(gfx.H['CIwTexture'], th))
        im = t.image
        if im.fmt == 0x23:
            hd = gfx.gl_blob_header(im.texels)
            assert hd['glfmt'] in (0x8C00, 0x8C02), 'GL format %#x' % hd['glfmt']
            w, h = hd['w'], hd['h']
            rgba = pvrtc.decode(im.texels[72:72 + hd['levels'][0]], w, h)
        else:
            w, h, rgba = im.to_rgba()
        nt = gfx.Texture.build_abgr8888(nh, w, h, rgba, mipmaps=True)
        nt.flags |= t.flags & 0x20000  # as World at War's (the Pack-a-Punch camo has it)
        return self.add(self.weapons, gfx.H['CIwTexture'], nh, nt.write())

    def material(self, mh):
        nh = new_hash('mat_%08x' % mh)
        if (gfx.H['CIwMaterial'], nh) in self.weapons:
            return nh
        m = gfx.Material.read(mh, self.waw(gfx.H['CIwMaterial'], mh))
        m.textures = [self.texture(t) if t else 0 for t in m.textures]
        m.hash = nh
        return self.add(self.weapons, gfx.H['CIwMaterial'], nh, m.write())

    def skel(self, sh, name):
        nh = new_hash(name)
        s = gfx.Skel.read(sh, self.waw(gfx.H['CIwAnimSkel'], sh))
        assert s.x20 == 40960, s.x20
        for b in s.bones:
            b.pos = tuple(max(-32768, min(32767, int(round(c * SKEL_SCALE)))) for c in b.pos)
            b.x18 = BO_BONE_X18
        s.x20 = BO_SKEL_X20
        s.hash = nh
        return self.add(self.weapons, gfx.H['CIwAnimSkel'], nh, s.write())

    def model(self, mh, materials, name, skel):
        """the World at War model (and its skin) as `name`, with `materials` (World at War hashes) or its own"""
        nh = new_hash(name)
        m = gfx.Model.read(mh, self.waw(gfx.H['CIwModel'], mh))
        v = m.block('CIwModelBlockVerts')
        pos = [tuple(c * MODEL_SCALE for c in p) for p in v.unique()]
        v.mean, v.planes = gfx.planar_from_values(pos, 3)
        m.sphere = tuple(c * MODEL_SCALE for c in m.sphere)
        for e in m.exts:
            if isinstance(e, gfx.ExtPos) and e.hash == MUZZLE:
                e.pos = [max(-32768, min(32767, c * MODEL_SCALE)) for c in e.pos]
        mats = materials or m.materials
        assert len(mats) == len(m.materials), (name, len(mats), len(m.materials))
        m.materials = [self.material(x) for x in mats]
        m.hash = nh
        self.add(self.weapons, gfx.H['CIwModel'], nh, m.write())
        s = gfx.Skin.read(mh, self.waw(gfx.H['CIwAnimSkin'], mh))
        s.model, s.skel, s.hash = nh, skel, nh
        for st in s.sets:
            st.data = scale_skin_set(st, SKEL_SCALE)
        self.add(self.weapons, gfx.H['CIwAnimSkin'], nh, s.write())
        return nh

    def anim(self, where, ah, skel=None):
        nh = new_hash('anim_%08x' % ah)
        if (ANIM, nh) not in where:
            body = self.waw(ANIM, ah)
            out = waw_anim.convert(waw_anim.Anim(body), SKEL_SCALE)
            if skel:
                out = struct.pack('<I', skel) + out[4:]
            elif where is self.arms:
                out = still_camera(out, self.arms_skel)
            self.add(where, ANIM, nh, out)
            self.lengths[nh] = anim_length(body, waw=True)
        return nh

    # ---- sounds
    def sound(self, sh, fire=False, priority=None):
        """World at War sound spec (and its sound data) -> Black Ops'. The sound data is the same format in both
        (ADPCM: u32 size, u32, u32, the data, u16 2, u32 samples per block, u32 block size): copied. The spec has
        5 more bytes in Black Ops (3.7.1) than in World at War (3.6.4), before the sound group, and the u32 at +14 is
        100 in every Black Ops spec and 0 in every World at War one; +18 is 6144 on Black Ops' MP40 fire, 1000 on its
        reload sounds. +26 is the priority (CIwSoundSpec +0x30: with every channel busy, a sound only takes the
        channel of a lower one, FUN_02530c02; both games' MP40 fire 10)."""
        nh = new_hash('snd_%08x' % sh)
        if (gfx.H['CIwSoundSpec'], nh) in self.sounds:
            return nh
        rec = self.sfx.find(gfx.H['CIwSoundSpec'], sh)
        assert rec and len(rec[2]) == 43, 'World at War has no sound %08x' % sh
        b = rec[2]
        data = struct.unpack_from('<I', b, 39)[0]
        nd = new_hash('snd_%08x' % data)
        drec = self.sfx.find(gfx.H['CIwSoundData'], data)
        self.record_class[gfx.H['CIwSoundData'], nd] = drec[0]
        self.record_class[gfx.H['CIwSoundSpec'], nh] = rec[0]
        self.add(self.sounds, gfx.H['CIwSoundData'], nd, sound_data(drec[2]))
        mid = b[22:31] if priority is None else b[22:26] + struct.pack('<I', priority) + b[30:31]
        spec = (b[:14] + struct.pack('<II', 100, 6144 if fire else 1000) + mid + bytes.fromhex('0300006400') +
                struct.pack('<I', SOUND_GROUP) + b[35:39] + struct.pack('<I', nd))
        return self.add(self.sounds, gfx.H['CIwSoundSpec'], nh, spec)

    def sequence(self, qh):
        """World at War CSoundSequence -> Black Ops'"""
        nh = new_hash('seq_%08x' % qh)
        if (C_SEQUENCE, nh) in self.sounds:
            return nh
        b = self.sfx.find(C_SEQUENCE, qh)[2]
        n = struct.unpack_from('<I', b, 1)[0]
        out = b[:5]
        for i in range(n):
            cls, zero, v = struct.unpack_from('<3I', b, 5 + 12 * i)
            if cls == C_DELAY:
                out += struct.pack('<3I', cls, zero, v)
            else:
                assert cls == C_PLAY, 'sequence command %08x' % cls
                out += struct.pack('<4IB', cls, zero, self.sound(v), 0, 0)
        assert 5 + 12 * n == len(b)
        return self.add(self.sounds, C_SEQUENCE, nh, out)

    def fire_sequence(self, sh):
        """a Black Ops sequence playing World at War fire sound `sh` (World at War plays it straight from the arms
        machine; Black Ops' specs name a sequence)"""
        nh = new_hash('fire_%08x' % sh)
        body = b'\0' + struct.pack('<I', 1) + struct.pack('<4IB', C_PLAY, 0, self.sound(sh, fire=True), 0, 0)
        return self.add(self.sounds, C_SEQUENCE, nh, body)

    def play_sequence(self, sh):
        """a Black Ops sequence playing World at War sound `sh`"""
        nh = new_hash('play_%08x' % sh)
        body = b'\0' + struct.pack('<I', 1) + struct.pack('<4IB', C_PLAY, 0, self.sound(sh), 0, 0)
        return self.add(self.sounds, C_SEQUENCE, nh, body)

    def variation(self, sh):
        """a Black Ops CSoundVariation of World at War sound `sh` alone (LFER: u32 size, 'LFER', class, size, u32 1,
        class, field (the spec list), u32 1, its type, u32 1, u32 data size, u32 count, the specs)"""
        nh = new_hash('var_%08x' % sh)
        specs = [self.sound(sh)]
        size = 0x2c + 4 * len(specs)
        body = (struct.pack('<I', size) + b'LFER' + struct.pack('<IIII', C_VARIATION, size, 1, C_VARIATION) +
                struct.pack('<IIIII', 0x5040e379, 1, 0x1c3aeea1, 1, 4 + 4 * len(specs)) +
                struct.pack('<I%dI' % len(specs), len(specs), *specs))
        return self.add(self.sounds, C_VARIATION, nh, body)

    def callback_param(self, machine, state, name):
        """the parameter World at War's arms `machine` passes to callback `name` in `state` (either event list;
        events name callbacks counting from the end of the table), or None"""
        cb = machine['callbacks']
        if gfx.djb2(name) not in cb:
            return None
        idx = len(cb) - 1 - cb.index(gfx.djb2(name))
        for item in state['events'] + state['timed']:
            if item[0] == idx and item[1]:
                return item[1] & 0xffffffff
        return None

    def joined_anim(self, where, first, then, speed_first, speed_then, skel=None):
        """World at War animations `first` then `then` as one Black Ops animation, timed so that played at
        speed_first the second part still runs at speed_then"""
        nh = new_hash('join_%08x_%08x' % (first, then))
        if (ANIM, nh) not in where:
            a, b = waw_anim.Anim(self.waw(ANIM, first)), waw_anim.Anim(self.waw(ANIM, then))
            k = speed_first / speed_then
            t0 = a.keys[-1].time + 1
            for key in b.keys:
                key = copy.copy(key)
                key.time = t0 + int(round(key.time * k))
                a.keys.append(key)
            n = max(len(a.mask), len(b.mask))
            a.mask = [(a.mask[i] if i < len(a.mask) else 0) | (b.mask[i] if i < len(b.mask) else 0) for i in range(n)]
            out = waw_anim.convert(a, SKEL_SCALE)
            if skel:
                out = struct.pack('<I', skel) + out[4:]
            elif where is self.arms:
                out = still_camera(out, self.arms_skel)
            self.add(where, ANIM, nh, out)
            self.lengths[nh] = a.keys[-1].time
        return nh

    def arms_machine(self, gun):
        return waw_asm.parse(self.waw(ASM, self.guns[gun]['arms_asm']))

    def build_gun(self, gun, bo_weapons, bo_arms, source=None):
        """converts World at War `gun` and its Pack-a-Punch version: records into self.weapons / self.arms;
        -> {'spec': (hash, body), 'upgrade': (hash, body)} built on Black Ops specs in bo_weapons"""
        cfg = GUNS[gun]
        base = cfg['machines']
        g, up = self.guns[gun], self.guns[gun + '_upgrade']
        bw, ba = gfx.Group(bo_weapons), gfx.Group(bo_arms)  # the map's
        target_arms = ba
        if cfg.get('source'):  # the base gun's records come from another map's groups
            bw, ba = gfx.Group(source[0]), gfx.Group(source[1])
        mp40 = gfx.Group(bo_weapons)

        # models: normal (material set 0 / 2), Pack-a-Punch (the models' own materials)
        skin = gfx.Skin.read(g['skin'], self.waw(gfx.H['CIwAnimSkin'], g['skin']))
        skel = self.skel(skin.skel, 'skel_%08x' % skin.skel)  # (the sawn-off shares the shotgun's, and its animations)
        ms = lambda h: self.matsets.get(h) if h else None
        models = {
            'model': self.model(g['model'], ms(g['matset0']), gun, skel),
            'lod': self.model(g['model3'], ms(g['matset2']), gun + '_lod', skel),
            'up_model': self.model(up['model'], ms(up['matset0']), gun + '_upgrade', skel),
            'up_lod': self.model(up['model3'], ms(up['matset2']), gun + '_upgrade_lod', skel),
        }
        # the aiming model: World at War's model2 (the gun cut down to what shows while aiming; it switches to it with
        # aimModel), like Black Ops' ADS models; material set 1 for the normal gun
        models['ads'], models['up_ads'] = models['model'], models['up_model']
        if g['model2'] and g['model2'] != g['model']:
            n2 = len(gfx.Model.read(g['model2'], self.waw(gfx.H['CIwModel'], g['model2'])).materials)
            m1 = ms(g['matset1']) or (ms(g['matset0']) if g['model2'] == g['model'] else None)
            if m1 and len(m1) == n2:
                models['ads'] = self.model(g['model2'], m1, gun + '_ads', skel)
                models['up_ads'] = self.model(up['model2'], ms(up['matset1']), gun + '_upgrade_ads', skel)

        self.arms_skel = gfx.Skel.read(ARMS_SKEL, ba.find(gfx.H['CIwAnimSkel'], ARMS_SKEL)[2])
        # which World at War arms state each Black Ops state takes its animation from
        wg = self.arms_machine(gun)
        wstates = {s['hash']: s for s in wg['states']}
        by_len = sorted((anim_length(self.waw(ANIM, wstates[h]['anim']), waw=True), i, h)
                        for i, h in enumerate(WAW_RELOADS))
        reloads = {EMPTY: by_len[-1][2], PARTIAL: by_len[0][2]}

        def waw_state(h):
            if cfg.get('looped') and h in LOOPED:
                return wstates.get(LOOPED[h])
            w = STATE_MAP.get(h, h)
            return wstates.get(reloads.get(w, w))

        # arms machine
        arms_name = 'waw_%s_arms' % gun
        base_arms = gfx.djb2(base + '_arms')
        bo_machine = ba.find(ASM, base_arms)[2]
        states = iwasm.read_states(bo_machine)[0]
        subst = {}   # Black Ops state -> new animation
        speeds = {}  # Black Ops state -> World at War's speed for it
        for s in states:
            ws = waw_state(s['hash'])
            if s['anim'] and ws and ws['anim']:
                subst[s['hash']] = self.anim(self.arms, ws['anim'])
                speeds[s['hash']] = ws['speed']
        if cfg.get('looped') and LOOP_START in subst:
            first, then = wstates[LOOPED[LOOP_START]], wstates[LOOPED[LOOP]]
            subst[LOOP_START] = self.joined_anim(self.arms, first['anim'], then['anim'], first['speed'], then['speed'])
        if cfg.get('bolt'):
            for bo, fire, bolt in ((BO_FIRE, WAW_FIRE, WAW_BOLT), (BO_AIM_FIRE, WAW_AIM_FIRE, WAW_AIM_BOLT)):
                if bo in subst and wstates[bolt]['anim']:
                    subst[bo] = self.joined_anim(self.arms, wstates[fire]['anim'], wstates[bolt]['anim'],
                                                 wstates[fire]['speed'], wstates[bolt]['speed'])
        # the aim offset baked into the aim animations (per gun: guns can share World at War aim animations)
        arms_skel = self.arms_skel
        offset = [-ADS_SCALE * c for c in g['offset_ac']]
        if any(offset):
            for sh in AIM_FULL + (AIM_IN, AIM_OUT):
                if sh not in subst:
                    continue
                ramp = 'in' if sh == AIM_IN else 'out' if sh == AIM_OUT else None
                nh = new_hash('%s_ads_%08x' % (gun, subst[sh]))
                if (ANIM, nh) not in self.arms:
                    self.add(self.arms, ANIM, nh, shift_view(self.arms[ANIM, subst[sh]], arms_skel, offset, ramp))
                    self.lengths[nh] = self.lengths[subst[sh]]
                subst[sh] = nh
        # sights lined up as Black Ops' guns aim (barrel along the view, front sight's tip in the middle)
        if AIM_FULL[0] in subst and gun not in KEEP_AIM:
            verts = gfx.Model.read(models['model'], self.weapons[gfx.H['CIwModel'], models['model']]).block(
                'CIwModelBlockVerts').unique()
            turn, move = aim_correction(self.arms[ANIM, subst[AIM_FULL[0]]], arms_skel, verts)
            for sh in AIM_FULL + (AIM_IN, AIM_OUT):
                if sh not in subst:
                    continue
                ramp = 'in' if sh == AIM_IN else 'out' if sh == AIM_OUT else None
                nh = new_hash('%s_aim_%08x' % (gun, subst[sh]))
                if (ANIM, nh) not in self.arms:
                    self.add(self.arms, ANIM, nh, line_up(self.arms[ANIM, subst[sh]], arms_skel, turn, move, ramp))
                    self.lengths[nh] = self.lengths[subst[sh]]
                subst[sh] = nh
        # the base gun's own Black Ops animations (no other arms machine uses them), re-gripped for this gun; the ones
        # every gun shares (knife, grenade, perk drink) hide the gun and stay
        users = {}
        for c, h, body, _ in ba.list_of(ASM).records:
            for s in iwasm.read_states(body)[0]:
                users.setdefault(s['anim'], set()).add(h)
        idle = next(s for s in states if s['hash'] == gfx.djb2('idle'))
        delta = _mul(_inv(_grip(ba.find(ANIM, idle['anim'])[2], arms_skel)),
                     _grip(self.arms[ANIM, subst[idle['hash']]], arms_skel))
        for s in states:
            a = s['anim']
            if a and s['hash'] not in subst and users[a] == {base_arms}:
                nh = new_hash('%s_anim_%08x' % (gun, a))
                if (ANIM, nh) not in self.arms:
                    body = ba.find(ANIM, a)[2]
                    moved = regrip(body, arms_skel, delta)
                    if moved is None:
                        continue  # it leaves TAG_WEAPON where the previous animation put it
                    self.add(self.arms, ANIM, nh, moved)
                    self.lengths[nh] = anim_length(body)
                subst[s['hash']] = nh
        arms_body = clone_machine(bo_machine, subst, self.lengths, ba, speeds)
        if cfg.get('looped'):
            first, then = wstates[LOOPED[LOOP_START]], wstates[LOOPED[LOOP]]
            arms_body = loop_start_reload(arms_body, first, then, self.waw(ANIM, first['anim']),
                                          self.waw(ANIM, then['anim']))
        if not g['x5e']:  # not automatic in World at War: each shot (and bolt / pump) plays out
            arms_body = single_shot(arms_body, (BO_FIRE, BO_AIM_FIRE))
        self.add(self.arms, ASM, gfx.djb2(arms_name), arms_body)
        for s in states:
            if s['anim'] and s['hash'] not in subst and not target_arms.find(ANIM, s['anim']):
                self.add(self.arms, ANIM, s['anim'], ba.find(ANIM, s['anim'])[2])

        # gun machine: every animation becomes one of the World at War gun's, from its arms states' playGunAnim
        # (World at War has no gun machine); states without one get its idle
        cb = wg['callbacks']
        play = len(cb) - 1 - cb.index(gfx.djb2('playGunAnim'))  # events name callbacks counting from the end
        gun_anim = {}
        for s in wg['states']:
            for item in s['events'] + s['timed']:  # (either list: the Magnum's are in the second)
                if item[0] == play and item[1]:
                    gun_anim.setdefault(s['hash'], item[1] & 0xffffffff)
        weapon_name = 'waw_%s' % gun
        bo_gun_machine = bw.find(ASM, gfx.djb2(base))[2]
        gsubst, gspeeds = {}, {}
        for s in iwasm.read_states(bo_gun_machine)[0]:
            assert not s['anim2'], 'gun machine with a second animation'
            if s['anim']:
                ws = waw_state(s['hash'])
                src = (ws and gun_anim.get(ws['hash'])) or gun_anim[gfx.djb2('idle')]
                gsubst[s['hash']] = self.anim(self.weapons, src, skel)
                if ws:
                    gspeeds[s['hash']] = ws['speed']  # in step with the arms
                if cfg.get('looped') and s['hash'] == LOOP_START:
                    first, then = LOOPED[LOOP_START], LOOPED[LOOP]
                    if gun_anim.get(first) and gun_anim.get(then):
                        gsubst[LOOP_START] = self.joined_anim(self.weapons, gun_anim[first], gun_anim[then],
                                                              wstates[first]['speed'], wstates[then]['speed'], skel)
                for bo, fire, bolt in ((BO_GUN_FIRE, WAW_FIRE, WAW_BOLT), (BO_GUN_AIM_FIRE, WAW_AIM_FIRE, WAW_AIM_BOLT)):
                    if cfg.get('bolt') and s['hash'] == bo and gun_anim.get(fire) and gun_anim.get(bolt):
                        gsubst[bo] = self.joined_anim(self.weapons, gun_anim[fire], gun_anim[bolt],
                                                      wstates[fire]['speed'], wstates[bolt]['speed'], skel)
        gun_body = clone_machine(bo_gun_machine, gsubst, self.lengths, bw, gspeeds)
        if not g['x5e']:
            gun_body = single_shot(gun_body, (BO_GUN_FIRE, BO_GUN_AIM_FIRE))
        self.add(self.weapons, ASM, gfx.djb2(weapon_name), gun_body)

        # specs
        base_spec = Spec(bw.find(gfx.H['CIsEntitySpec'], gfx.djb2(cfg['spec']))[2])
        if cfg.get('upgrade') == 'self':
            base_up_hash = gfx.djb2(cfg['spec'])
        else:
            base_up_hash = cfg.get('upgrade') or base_spec.comp(C_PLAYERWEAPON).get_u32('Upgrade')
        base_up = Spec(bw.find(gfx.H['CIsEntitySpec'], base_up_hash)[2])
        spec_hash, up_hash = new_hash(gun), new_hash(gun + '_upgrade')
        # an autoROF field to add to specs that have none (a semi-automatic spec for a gun that is automatic in World
        # at War: the Pack-a-Punched M1 Carbine)
        mp40_spec = Spec(mp40.find(gfx.H['CIsEntitySpec'], gfx.djb2('mp40'))[2]).comp(C_PLAYERWEAPON)
        olympia = Spec(mp40.find(gfx.H['CIsEntitySpec'], gfx.djb2('olympia'))[2]).comp(C_PLAYERWEAPON)
        rate_field = mp40_spec.field(F_RATE)
        ref_waw = self.guns[cfg.get('ref', cfg['spec'])] if cfg.get('ref', cfg['spec']) in self.guns else None
        out = {}
        for key, spec, own, model, ads, lod, h, old_h in (
                ('spec', base_spec, g, models['model'], models['ads'], models['lod'], spec_hash, gfx.djb2(cfg['spec'])),
                ('upgrade', base_up, up, models['up_model'], models['up_ads'], models['up_lod'], up_hash,
                 base_up_hash)):
            w = spec.comp(C_PLAYERWEAPON)
            if w.field('m_name') and w.get_u32('m_name') == old_h:
                w.set_u32('m_name', h)
            if key == 'spec':
                if not w.field('Upgrade'):
                    f = mp40_spec.field('Upgrade')
                    w.fields.append(lp.Field(f.cls, f.typ, f.a, f.fh, f.b, f.data))
                w.set_u32('Upgrade', up_hash)
            elif w.field('Upgrade'):
                w.set_u32('Upgrade', 0)
            w.set_str('m_armsAsm', arms_name)
            w.set_str('m_weaponAsm', weapon_name)
            w.set_u32('m_clipSize', own['clip'])
            w.set_u32('m_ammoSize', own['ammo'])
            if cfg.get('damage', 'waw') == 'base' and cfg.get('ref') in self.guns:
                # the copied spec's damage (Black Ops counts shotgun damage per pellet), scaled by World at War's
                # damage over that of the gun it stands for (the double-barrel for the Olympia)
                # damage over that of the gun it stands for (the double-barrel for the Olympia); the upgrade: the normal
                # gun's (its Black Ops upgrade's would be weaker than it: the Hades keeps the Olympia's 80) times World
                # at War's upgrade over normal, x PAP_BODY
                refg = self.guns[cfg['ref']]
                ds = Spec(bw.find(gfx.H['CIsEntitySpec'], gfx.djb2(cfg.get('damage_spec', cfg['spec'])))[2]).comp(
                    C_PLAYERWEAPON).sub('m_damage')
                k = own['x84'] / refg['x84'] * (PAP_BODY if key == 'upgrade' else 1.0)
                dmg = w.sub('m_damage')
                for n in ('m_headDamage', 'm_torsoDamage', 'm_limbDamage'):
                    dmg.set_u32(n, int(round(ds.get_u32(n) * k)))
                w.set_sub('m_damage', dmg)
            if cfg.get('damage', 'waw') == 'waw':
                # World at War's near damage on the body and limbs, times its head multiplier on the head (Black
                # Ops' MP40 is World at War's: 100 / 400). Pack-a-Punch: x PAP_BODY / PAP_HEAD.
                near = own['x84']
                parts = dict(own['list_c8'])
                mult = lambda i: parts.get(i, 4096) / 4096
                kb, kh = (PAP_BODY, PAP_HEAD) if key == 'upgrade' else (1.0, 1.0)
                dmg = w.sub('m_damage')
                dmg.set_u32('m_headDamage', int(round(near * mult(0) * kh)))
                dmg.set_u32('m_torsoDamage', int(round(near * mult(3) * kb)))
                dmg.set_u32('m_limbDamage', int(round(near * sum(mult(i) for i in (1, 2, 4, 5)) / 4 * kb)))
                w.set_sub('m_damage', dmg)
            # fire mode and rate
            beh = cfg.get('behaviour', w.get_u32('behaviour'))
            if beh != w.get_u32('behaviour'):
                w.set_u32('behaviour', beh)
            if beh in (BEH_SEMI, BEH_AUTO):
                beh = BEH_AUTO if own['x5e'] else BEH_SEMI
                w.set_u32('behaviour', beh)
            if beh == BEH_AUTO and not w.field(F_RATE):
                w.fields.append(lp.Field(rate_field.cls, rate_field.typ, rate_field.a, rate_field.fh, rate_field.b,
                                         rate_field.data))
            if w.field(F_RATE):
                fire = next(s for s in self.arms_machine(own['name'].split(b'\0')[0].decode().lower())['states']
                            if s['hash'] == WAW_FIRE)
                w.set_u32(F_RATE, int(round(RATE_REF[0] * RATE_REF[1] / fire['x14'])))
            if cfg.get('damage', 'waw') == 'waw' and not cfg.get('flame'):
                # hip spread
                acc = w.sub('m_accuracy')
                (bo_min, waw_min), (bo_max, waw_max) = SPREAD_REF
                smin = max(0.05, min(0.9, bo_min * own['x94'][1] / waw_min))
                acc.field('m_spreadMin').data = struct.pack('<f', smin)
                acc.field('m_spreadMax').data = struct.pack('<f', max(smin, min(0.9, bo_max * own['x94'][3] / waw_max)))
                w.set_sub('m_accuracy', acc)
            if not cfg.get('flame') and not cfg.get('projectile'):
                # bullets: damage over range and through zombies
                pk = PAP_BODY if key == 'upgrade' else 1.0
                if own['x6c'] > 0 and own['x84'] > own['x78'] > 0:
                    if not w.field('m_falloff'):
                        f = olympia.field('m_falloff')
                        w.fields.append(lp.Field(f.cls, f.typ, f.a, f.fh, f.b, f.data))
                    fo = w.sub('m_falloff')
                    fo.set_u32('m_startDistance', int(round(own['x68'] / 10)))
                    fo.set_u32('m_endDistance', int(round((own['x68'] + (own['x84'] - own['x78']) * 4096 / own['x6c'])
                                                          / 10)))
                    fo.set_u32('m_damageMod', max(1, min(100, int(round(100 * own['x78'] / own['x84'])))))
                    w.set_sub('m_falloff', fo)
                pen = w.sub('m_penetration')
                pen.field('m_damageFalloff').data = struct.pack('<f', max(0.0, min(100.0, 100.0 * own['x64'] / 4096)))
                pen.set_u32('m_damageMin', max(1, int(round(own['x78'] * pk))))
                if own['x54']:
                    pen.set_u32('m_penetrationRating', own['x54'])
                w.set_sub('m_penetration', pen)
            # sounds: World at War's fire sound and reload sequences (the reload states' playSoundSequence)
            if cfg.get('flame'):
                fire = wstates[WAW_FIRE]
                found = (self.callback_param(wg, fire, 'playSound'), self.callback_param(wg, fire, 'stopSound'))
                assert found == FLAME_SOUNDS and own['fire_sound'] == FLAME_SOUNDS[1], \
                    'flamethrower sounds %s, src/flame.cpp plays %s' % (found, FLAME_SOUNDS)
                for sh in FLAME_SOUNDS:  # (World at War's priority 0 would lose its channel to any zombie)
                    self.sound(sh, fire=True, priority=10)
                w.set_u32('fireSoundSequence', 0)
                if w.field('m_playPAPSound'):
                    w.field('m_playPAPSound').data = b'\0'
            else:
                w.set_u32('fireSoundSequence', self.fire_sequence(own['fire_sound']))
            for field, which in (('reloadSoundSequence', EMPTY), ('reloadPartialSequence', PARTIAL)):
                q = self.callback_param(wg, wstates[reloads[which]], 'playSoundSequence')
                if q and w.field(field) and self.sfx.find(C_SEQUENCE, q):
                    w.set_u32(field, self.sequence(q))
            if cfg.get('looped'):
                # a shell going in (Black Ops plays it with each DoReload) and the close: World at War's loop and end
                # states' sounds
                q = self.callback_param(wg, wstates[LOOPED[LOOP]], 'playSound')
                if q and w.field('m_ReloadSingleSoundVariation'):
                    w.set_u32('m_ReloadSingleSoundVariation', self.variation(q))
                q = self.callback_param(wg, wstates[LOOPED[LOOP_END]], 'playSound')
                if q and w.field('m_ReloadLoopOutSoundSequence'):
                    w.set_u32('m_ReloadLoopOutSoundSequence', self.play_sequence(q))
            (u0, v0), (u1, v1) = icon_cell(gun)
            w.field('m_uv0').data = struct.pack('<hh', u0, v0)
            w.field('m_uv1').data = struct.pack('<hh', u1, v1)
            w.set_u32('skel', skel)
            set_asset(w, 'renderableAsset', model)
            set_asset(w, 'renderableAssetADS', ads)
            set_lods(w, lod)
            if cfg.get('flame'):
                hades = Spec(mp40.find(gfx.H['CIsEntitySpec'], gfx.djb2(cfg['flame']))[2]).comp(C_PLAYERWEAPON)
                w.set_u32('m_clipSize', FLAME_AMMO[key][0])
                w.set_u32('m_ammoSize', FLAME_AMMO[key][1])
                w.set_u32('muzzleFlash', hades.get_u32('muzzleFlash'))
                w.field('m_accuracy').data = hades.field('m_accuracy').data
                if not w.field('incendiaryRounds'):
                    f = hades.field('incendiaryRounds')
                    w.fields.append(lp.Field(f.cls, f.typ, f.a, f.fh, f.b, f.data))
            if cfg.get('projectile') and w.field('m_projectileName'):
                self.projectile = w.get_u32('m_projectileName')
            out[key] = (h, spec.write())
        return out


def machine_events(body):
    """a Black Ops state machine's event names (the table after the states)"""
    states, r = iwasm.read_states(body)
    n = struct.unpack_from('<I', body, r.p)[0]
    return list(struct.unpack_from('<%dI' % n, body, r.p + 4))


def loop_start_reload(body, first, then, first_anim, then_anim):
    """the machine with its looped reload start's timed callbacks (DoReload) at the same point of the joined
    round of the loop (World at War states `first`, `then` and their animations, joined as joined_anim does) as the
    loop state's are of the loop. Timed callbacks (+0x34 list: u16 callback, i32 parameter, i32 time (4096 = the
    state's end), u8) follow the state's 28 fixed bytes and the list's count."""
    states, _ = iwasm.read_states(body)
    by = {s['hash']: s for s in states}
    loop_time = by[LOOP]['l34'][0][2] / 4096.0 if by[LOOP]['l34'] else 0.6
    l1 = anim_length(first_anim, waw=True)
    l2 = anim_length(then_anim, waw=True) * first['speed'] / then['speed']
    t = int(round((l1 + 1 + loop_time * l2) / (l1 + 1 + l2) * 4096))
    out = bytearray(body)
    st = by[LOOP_START]
    for i in range(len(st['l34'])):
        struct.pack_into('<i', out, st['offset'] + 28 + 4 + 11 * i + 6, t)
    assert all(e[2] == t for e in iwasm.read_states(bytes(out))[0][states.index(st)]['l34'])
    return bytes(out)


def single_shot(body, fire_states):
    """a Black Ops state machine whose fire states run to their end before anything else (like its semi-automatic and
    pump guns: the M14, Stakeout, Olympia, Python): the MP40's leave for the idle state when the trigger is let go
    ('shoot', parameter 2, or 'AimShoot' 2 aimed), which would cut a bolt or pump short and let the next shot come as
    fast as the trigger is pulled. That transition becomes 'Always' (taken when the state ends, no blend), as theirs."""
    states, _ = iwasm.read_states(body)
    ev = machine_events(body)
    idx = lambda name: len(ev) - 1 - ev.index(gfx.djb2(name))
    out = bytearray(body)
    for s in states:
        if s['hash'] not in fire_states:
            continue
        for i, (target, blend, event, param, b1, b2) in enumerate(s['l54']):
            if event in (idx('shoot'), idx('AimShoot')) and param == 2:
                struct.pack_into('<HHHi??', out, s['l54_offset'] + 12 * i, target, 0, idx('Always'), 1, False, True)
    return bytes(out)


def clone_machine(body, subst, lengths, group, speeds=None):
    """a Black Ops state machine with new animations (subst: state hash -> animation). speeds: state hash -> the
    speed (+0x14, 4096 = 1) to play it at (World at War's); a timed state's length (ms, +0x18) is then its animation's
    length at that speed. Without one, the length is scaled by the new animation's length over the old one's."""
    states, _ = iwasm.read_states(body)
    out = bytearray(body)
    for s in states:
        new = subst.get(s['hash'])
        if new is None:
            continue
        o = s['offset']
        struct.pack_into('<I', out, o + 4, new)
        speed = (speeds or {}).get(s['hash'])
        if speed:
            struct.pack_into('<i', out, o + 12, speed)
            if s['x18']:
                struct.pack_into('<i', out, o + 16, max(1, int(round(lengths[new] * 1000 / speed))))
        elif s['x18']:
            old_len = anim_length(group.find(ANIM, s['anim'])[2])
            struct.pack_into('<i', out, o + 16, int(round(s['x18'] * lengths[new] / old_len)))
    return bytes(out)


# ---------------------------------------------------------------- the pack
def add_records(data, records, classes=None):
    """records: {(list class, hash): body}; classes: {(list class, hash): record class} for lists that hold a class
    per record (Black Ops' sound lists: CIwSoundDataADPCM, CIwSoundSpecSound). The game makes each record's object
    from that class and reads the record with its Serialise (not by the record's size)."""
    g = gfx.Group(data)
    for (cls, h), body in records.items():
        assert not g.find(cls, h), '%s %08x is already in the group' % (gfx.CLS.get(cls, '%08x' % cls), h)
        rl = g.list_of(cls)
        assert rl is not None, 'no %08x list' % cls
        rc = (classes or {}).get((cls, h), cls)
        if not rl.all_same:
            assert rc in {r[0] for r in rl.records}, 'record class %08x is not one the %08x list has' % (rc, cls)
        rl.records.append([rc, h, body, None])
    out = g.write()
    assert gfx.Group(out).write() == out
    return out


def add_to_boxes(data, spec_hashes):
    level = lp.Level('statics', data)
    n = 0
    key = struct.pack('<I', C_BOX)
    for i in range(len(level.records)):
        o, sz, _ = level.records[i]
        if key not in level.d[o:o + sz]:  # (not every entity parses with levelpatch; the boxes do)
            continue
        rec = lp.parse_record(level.d, o)
        box = next((c for c in rec.comps if c.cls == C_BOX), None)
        if not box:
            continue
        f = next(f for f in box.fields if f.fh == F_WEAPONS)
        cnt = struct.unpack_from('<I', f.data)[0]
        lst = list(struct.unpack_from('<%dI' % cnt, f.data, 4))
        lst += [h for h in spec_hashes if h not in lst]
        f.data = struct.pack('<I%dI' % len(lst), len(lst), *lst)
        level.replace_index(i, rec)
        n += 1
    assert n, 'no mystery boxes'
    return bytes(level.d), n


def build_files(src, files):
    """the pack files to replace, on top of `files` (the ones already patched); {} without World at War's data"""
    waw = waw_files()
    if waw is None:
        print('wawguns: no World at War Zombies IPA found (cod waw next to the port, or WAW_IPA): skipped')
        return {}
    read = lambda p: files[p] if p in files else dzpatch.read_file(src, p)
    out = {}
    sounds = {}
    for m in MAPS:
        conv = Converter(waw, sounds)
        wp, ap, sp = weapons_path(m), arms_path(m), statics_path(m)
        bo_weapons, bo_arms = read(wp), read(ap)
        box = []
        projectiles = {}
        for gun in GUNS:
            srcmap = GUNS[gun].get('source')
            source = (read(weapons_path(srcmap)), read(arms_path(srcmap))) if srcmap else None
            conv.projectile = None
            specs = conv.build_gun(gun, bo_weapons, bo_arms, source)
            for key in ('spec', 'upgrade'):
                h, body = specs[key]
                conv.add(conv.weapons, gfx.H['CIsEntitySpec'], h, body)
            box.append(specs['spec'][0])
            if conv.projectile:
                pg = gfx.Group(read(projectiles_path(m)))
                sg = gfx.Group(read(projectiles_path(srcmap or m)))
                # the bolt, and what its hit spawns (the Wunderwaffe's chain lightning)
                for ph in [conv.projectile] + [gfx.djb2(n) for n in (CHAIN if GUNS[gun] is WONDER else ())]:
                    if not pg.find(gfx.H['CIsEntitySpec'], ph):
                        rec = sg.find(gfx.H['CIsEntitySpec'], ph)
                        assert rec, '%s: projectile %08x not in %s' % (gun, ph, srcmap or m)
                        body = rec[2]
                        if ph == gfx.djb2(CHAIN[0]):
                            chain = Spec(body)
                            jumper = next(lf for _, _, lf in chain.comps if lf.cls == F('CChainLightning'))
                            jumper.field('m_jumpRange').data = struct.pack('<f', CHAIN_JUMP_RANGE)
                            body = chain.write()
                        projectiles[gfx.H['CIsEntitySpec'], ph] = body
        if projectiles:
            out[projectiles_path(m)] = add_records(read(projectiles_path(m)), projectiles)
        out[wp] = add_records(bo_weapons, conv.weapons)
        out[ap] = add_records(bo_arms, conv.arms)
        out[sp], n = add_to_boxes(read(sp), box)
        print('wawguns: %s: %s in %d boxes (%d weapon records, %d arms records)' % (
            m, ', '.join(GUNS), n, len(conv.weapons), len(conv.arms)))
    classes = sounds.pop('classes', {})
    ingame = read(INGAME)
    hud_icons = gfx.Group(ingame).find(gfx.H['CIwTexture'], hud.HUD_ICONS)[2]
    sounds[gfx.H['CIwTexture'], ICON_TEXTURE] = icon_texture(waw, hud_icons)
    out[INGAME] = use_icon_texture(add_records(ingame, sounds, classes))
    print('wawguns: %d sound records in %s' % (len(sounds), INGAME))
    return out


def inputs():
    """what build_files depends on, for tools/mulekick.py's up-to-date check"""
    names = ['wawguns.py', 'pvrtc.py', 'imaadpcm.py', 'waw_anim.py', 'waw_asm.py', 'waw_dz.py', 'waw_gun.py', 'iwanim.py', 'iwasm.py']
    return [os.path.join(HERE, n) for n in names]


def inputs_stamp():
    ipa = find_ipa()
    return ipa_stamp(ipa) if ipa else 'no ipa'


if __name__ == '__main__':
    if '--check' in sys.argv:
        src = os.path.join(ROOT, '..', 'backup', 'obb', 'com.activision.boz', 'blackops_etc.dz')
        res = build_files(src, {})
        for p, d in res.items():
            print('%9d  %s' % (len(d), p))
    else:
        print('usage: python tools/wawguns.py --check   (the pack is built by tools/mulekick.py)')
