# World at War Zombies (iOS) CItemGun records (class 0x81c32701 in ingame/player/player.group.bin), from the game's
# own Serialise (CItemGun vtable 0x2b515c, entry 5, ARM code at 0x2132c0 in kiwi.app/kiwi). The record is a packed
# little-endian stream (the resource hash comes first, outside the body). Field names are the object offsets the
# Serialise writes from; meanings where known.
import struct

# (name, kind): u32/i32/u8/ref (u32 resource hash)/i32xN/list/str
LAYOUT = [
    ('arms_asm', 'u32'),          # +0x48 CIsAnimStateMachine: the first-person arms (e.g. 'mp40_arms')
    ('char_asm', 'u32'),          # +0x4c CIsAnimStateMachine (multicharsbase): third-person, a character holding it
    ('base_model', 'u32'),        # +0xf4 the gun's base model (set on upgrades: the normal gun's model)
    ('model', 'ref'),             # +0x18 CIwModel: the gun
    ('skin', 'ref'),              # +0x1c CIwAnimSkin of it
    ('model2', 'ref'),            # +0x20 CIwModel (second part: magazine / LOD)
    ('skin2', 'ref'),             # +0x24
    ('model3', 'ref'),            # +0x28 CIwModel (third part)
    ('skin3', 'ref'),             # +0x2c
    ('world_model', 'ref'),       # +0x40 CIwModel (no skin: the world / pickup model)
    ('fire_sound', 'ref'),        # +0x50 CIwSoundSpec (sfx.group.bin)
    ('matset0', 'ref'),           # +0x30 CIsMaterialSet
    ('matset1', 'ref'),           # +0x34
    ('matset2', 'ref'),           # +0x38
    ('matset3', 'ref'),           # +0x3c
    ('upgrade', 'ref'),           # +0x44 CItemGun: its Pack-a-Punch version (0 on upgrades)
    ('xe0', 'u32'),               # +0xe0
    ('xe4', 'u32'),               # +0xe4
    ('effect0', 'u32'),           # +0xe8 CIwEntSpec (effects.group.bin): muzzle flash
    ('effect1', 'u32'),           # +0xec CIwEntSpec: shell / impact
    ('effect2', 'u32'),           # +0xf0 CIwEntSpec
    ('x54', 'u32'),               # +0x54 20 most guns, 10 shotguns, 1 wonder weapons
    ('projectile', 'u32'),        # +0x88 CIwEntSpec: Ray Gun / Wunderwaffe projectile
    ('projectile2', 'u32'),       # +0x8c CIwEntSpec
    ('xd9', 'u8'), ('xd8', 'u8'), ('x5f', 'u8'),
    ('x64', 'i32'), ('x6c', 'i32'), ('x68', 'i32'), ('x104', 'i32'), ('x100', 'i32'), ('x74', 'i32'), ('x70', 'i32'),
    ('x78', 'u32'), ('x58', 'u32'),
    ('x5c', 'u8'), ('x5d', 'u8'), ('x5e', 'u8'), ('xf8', 'u8'), ('x10c', 'u8'), ('x140', 'u8'), ('x141', 'u8'),
    ('x143', 'u8'),
    ('x60', 'i32'),
    ('xfc', 'u32'),
    ('clip', 'u32'),              # +0x7c rounds per clip
    ('ammo', 'u32'),              # +0x80 reserve ammo (max)
    ('x84', 'u32'),
    ('x90', 'i32'),
    ('x94', 'i32x6'),
    ('offset_b8', 'i32x3'),       # +0xb8 a vector
    ('offset_ac', 'i32x3'),       # +0xac a vector
    ('xc4', 'i32'),
    ('xdc', 'i32'),               # +0xdc (an i16 before data version 3.4.5)
    ('x144', 'i32'),
    ('list_c8', 'list'),          # +0xc8 count, then {u32, i32} each
    ('x108', 'u32'),
    ('name', 'str'),              # +0x120 the gun's name (at most 32 bytes)
]


MEANINGS = """field meanings found by comparing the 24 guns (known: clip, ammo; the rest from their values):
  x84 damage near, x78 damage far (Thompson 120/80, Kar98k 150/100, PTRS 1000; shotguns 1200 near / 15 per pellet far)
  x68 effective range (SMGs 8128, rifles 12192, PTRS 40640, shotguns 1732); x64 damage left at range (4096 = 1.0)
  x6c hip-fire spread (shotguns 1050-1791, SMGs 16-23, rifles 4-8)
  x5e full auto (SMGs, STG-44, BAR, FG42, MGs); xf8 pistol (Colt, .357, Ray Gun); x5c shotgun; x5d/x10c flamethrower
  x141 semi-auto / bolt rifle; xd8/x5f Ray Gun (projectile); xdc 1 on the Colt (the starting pistol)
  xc4 movement speed, 512 = normal (pistols 534, SMGs 512, bolt rifles 386, PTRS 364)
  list_c8 body-part damage multipliers {part, 4096 = x1}: part 0 head (SMGs x4, bolt rifles x8, PTRS x10)
  x144 reload style (1, or a hash shared by the bolt rifles / by the shotguns and the Wunderwaffe)
  x94 six timings; offset_ac/offset_b8 view offsets; x58 a name hash shared by gun classes"""


def parse(body):
    """-> ({name: value}, bytes consumed)"""
    p = 0
    out = {}

    def take(fmt):
        nonlocal p
        v = struct.unpack_from('<' + fmt, body, p)
        p += struct.calcsize('<' + fmt)
        return v

    for name, kind in LAYOUT:
        if kind in ('u32', 'ref'):
            out[name] = take('I')[0]
        elif kind == 'i32':
            out[name] = take('i')[0]
        elif kind == 'u8':
            out[name] = take('B')[0]
        elif kind.startswith('i32x'):
            out[name] = list(take('%di' % int(kind[4:])))
        elif kind == 'list':
            n = take('I')[0]
            assert n < 100, ('list count', n, p)
            out[name] = [take('Ii') for _ in range(n)]
        elif kind == 'str':
            rest = body[p:]
            out[name] = rest
            p = len(body)
    return out, p



if __name__ == '__main__':
    # usage: python tools/waw_gun.py PLAYER.group.bin   (World at War's ingame/player/player.group.bin, from waw_dz.py)
    import os
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import iwgfx as gfx
    g = gfx.Group(open(sys.argv[1], 'rb').read())
    print('%-22s %5s %5s %6s %6s %6s %6s %5s %s' % ('gun', 'clip', 'ammo', 'dmg', 'dmgfar', 'range', 'spread', 'auto', 'head x'))
    for c, h, body, _ in g.list_of(0x81c32701).records:
        gu, _ = parse(body)
        head = dict(gu['list_c8']).get(0, 4096) / 4096
        print('%-22s %5d %5d %6d %6d %6d %6d %5d %.1f' % (gu['name'].rstrip(b'\0').decode(), gu['clip'], gu['ammo'],
              gu['x84'], gu['x78'], gu['x68'], gu['x6c'], gu['x5e'], head))
    print(MEANINGS)
