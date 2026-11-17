# Kino der Toten's hellhound rounds as Black Ops PC plays them (_zombiemode_ai_dogs.gsc). The game code side
# (when dog rounds come, dog health past the third round) is src/dogs.cpp; this edits the level data, in place
# (same sizes):
#   levels/kino/kino_statics.group.bin   the 18 dog waves (CWaveSpeciality: 3 per player count, the first dog round,
#                                        the second, and every later one)
#       dogs per round      6 per player for dog rounds 1 and 2, 8 per player after (was 6/12/16/18, 8/16/18/20)
#       dogs alive at once  2 per player (was 3, then 5, whatever the player count)
#       spawning            one at a time (was 2 at the start), 2.5 / 2 / 1.5 s apart (was 3 s)
#       health              400, 900, 1300 (src/dogs.cpp: 1600 from the 4th dog round), was 300, 600, 990, 1980, ...
#       round end           8 s after the last dog dies, as in BO1 (was 3 s, then 2 s)
#   levels/kino/kino_dynamics.group.bin  CAIControllerHellhound.m_damage: a bite takes 92 of the player's 500 health
#                                        (was 58): BO1's 40 of 100 against a zombie hit's 50, at the phone's 115
# usage as a library: patch_statics(bytes) -> bytes, patch_dynamics(bytes) -> bytes (tools/mulekick.py calls them)
import struct

from levelpatch import djb2

STATICS = 'levels/kino/kino_statics.group.bin'
DYNAMICS = 'levels/kino/kino_dynamics.group.bin'

DOGS_PER_PLAYER = {0: 6, 1: 6, 2: 8}  # by dog round (the 3rd wave of each set covers round 3 on)
TIME_OUT = {0: 2500, 1: 2000, 2: 1500}
HEALTH = {0: 400, 1: 900, 2: 1300}
MAX_ALIVE = {0: 2, 1: 2, 2: 2, 3: 4, 4: 6, 5: 8}  # by m_waveType: 0-2 single player (by difficulty), 3-5 2-4 players
WAVE_TIME_OUT = 8000
BITE = 92


def objects(d, cls, end=None):
    """(offset, {field name hash: (value offset, length)}) of each LFER object of class cls"""
    want = djb2(cls)
    o = 0
    end = len(d) if end is None else end
    while True:
        o = d.find(b'LFER', o, end)
        if o < 0:
            return
        c, _size, nf = struct.unpack_from('<3I', d, o + 4)
        if c == want and nf < 200:
            r, fields = o + 16, {}
            for _ in range(nf):
                _fc, _t, _a, fh, _b, ln = struct.unpack_from('<6I', d, r)
                fields[fh] = (r + 24, ln)
                r += 24 + ln
            yield o, fields
        o += 4


def put(d, fields, name, fmt, *values):
    off, ln = fields[djb2(name)]
    assert struct.calcsize(fmt) == ln, (name, ln)
    struct.pack_into(fmt, d, off, *values)


def get(d, fields, name, fmt):
    off, ln = fields[djb2(name)]
    return struct.unpack_from(fmt, d, off)


def patch_statics(src):
    d = bytearray(src)
    waves = list(objects(d, 'CWaveSpeciality'))
    assert len(waves) == 18, len(waves)
    for i, (_o, f) in enumerate(waves):
        k = i % 3
        wave_type, = get(d, f, 'm_waveType', '<I')
        n = DOGS_PER_PLAYER[k]
        assert get(d, f, 'm_players2ZombieNumber', '<I')[0] == 4
        put(d, f, 'm_players2ZombieNumber', '<5I', 4, n, 2 * n, 3 * n, 4 * n)
        assert get(d, f, 'm_healthBase', '<I')[0] == 1
        put(d, f, 'm_healthBase', '<2I', 1, HEALTH[k])
        if djb2('m_ZombieHealthMultiplier') in f:
            put(d, f, 'm_ZombieHealthMultiplier', '<f', 1.0)
        put(d, f, 'm_MaxActive', '<I', MAX_ALIVE[wave_type])
        put(d, f, 'm_StartingZombies', '<I', 1)
        put(d, f, 'm_TimeOut', '<I', TIME_OUT[k])
        put(d, f, 'm_WaveTimeout', '<I', WAVE_TIME_OUT)
    return bytes(d)


def patch_dynamics(src):
    d = bytearray(src)
    dogs = [(o, f) for o, f in objects(d, 'CAIControllerHellhound') if djb2('m_damage') in f]
    assert len(dogs) == 2, len(dogs)  # the dog and the flaming dog
    for _o, f in dogs:
        put(d, f, 'm_damage', '<I', BITE)
    return bytes(d)
