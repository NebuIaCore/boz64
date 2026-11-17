# Adds strings to the game's localisation tables (localisation/ in the data pack).
#
# LocaliseHashes.dat: "LOC0" [count u16] [count u32 hashes, sorted] [count u16 string numbers]
# <language>.dat (english_us, english_uk, french, german, italian, spanish):
#   "LOC0" [u32 0x11161] [string bytes u32] [count u32] [count u32 offsets] "utf-8\0" [the strings, NUL-ended]
# A text's key is a 32-bit hash (game data stores the hash, e.g. a perk machine's name); the string numbers are
# the same in every language.
#
# usage as a library: add({path: bytes}, [(hash, {"english_us": "Mule Kick", ...}), ...]) -> {path: new bytes}
# A language missing from a text's dict gets its english_us text.
import struct

LANGS = ['english_us', 'english_uk', 'french', 'german', 'italian', 'spanish']
HASHES = 'localisation/LocaliseHashes.dat'


def lang_path(lang):
    return 'localisation/%s.dat' % lang


def add(files, texts):
    h = files[HASHES]
    assert h[:4] == b'LOC0'
    n, = struct.unpack_from('<H', h, 4)
    hashes = list(struct.unpack_from('<%dI' % n, h, 6))
    nums = list(struct.unpack_from('<%dH' % n, h, 6 + 4 * n))
    langs = {}
    for lang in LANGS:
        e = files[lang_path(lang)]
        assert e[:4] == b'LOC0'
        tag, size, count = struct.unpack_from('<3I', e, 4)
        assert count == n, (lang, count, n)
        offs = list(struct.unpack_from('<%dI' % count, e, 16))
        base = 16 + 4 * count
        assert e[base:base + 6] == b'utf-8\0'
        langs[lang] = [tag, offs, bytearray(e[base + 6:base + 6 + size])]
    for key, text in texts:
        if key in hashes:  # already there (e.g. patching twice): replace the text
            num = nums[hashes.index(key)]
            for lang, (tag, offs, blob) in langs.items():
                s = text.get(lang, text['english_us']).encode('utf-8') + b'\0'
                offs[num] = len(blob)
                blob += s
            continue
        num = len(hashes)
        assert num < 0xffff
        i = 0
        while i < len(hashes) and hashes[i] < key:
            i += 1
        hashes.insert(i, key)
        nums.insert(i, num)
        for lang, (tag, offs, blob) in langs.items():
            offs.append(len(blob))
            blob += text.get(lang, text['english_us']).encode('utf-8') + b'\0'
    out = {}
    n = len(hashes)
    out[HASHES] = b'LOC0' + struct.pack('<H', n) + struct.pack('<%dI' % n, *hashes) + struct.pack('<%dH' % n, *nums)
    for lang, (tag, offs, blob) in langs.items():
        out[lang_path(lang)] = (b'LOC0' + struct.pack('<3I', tag, len(blob), n) + struct.pack('<%dI' % n, *offs) +
                                b'utf-8\0' + bytes(blob))
    return out


def lookup(files, key, lang='english_us'):
    h = files[HASHES]
    n, = struct.unpack_from('<H', h, 4)
    hashes = struct.unpack_from('<%dI' % n, h, 6)
    nums = struct.unpack_from('<%dH' % n, h, 6 + 4 * n)
    e = files[lang_path(lang)]
    count, = struct.unpack_from('<I', e, 12)
    off, = struct.unpack_from('<I', e, 16 + 4 * nums[hashes.index(key)])
    o = 16 + 4 * count + 6 + off
    return e[o:e.index(b'\0', o)].decode('utf-8')
