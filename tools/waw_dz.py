# Reads World at War Zombies (iOS) .dz packs: the same DTRZ layout as Black Ops Zombies' pack (tools/dzpatch.py), but
# each chunk is LZMA ("alone" format: 5-byte properties, 8-byte size) instead of stored.
# usage: wawdz.py PACK.dz OUTDIR     unpacks every file
import lzma
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dzpatch  # noqa: E402


def files(pack):
    """{path: bytes}"""
    with open(pack, 'rb') as f:
        entries, chunks, _ = dzpatch.read_header(f)
        size = os.fstat(f.fileno()).st_size
        starts = sorted(c[0] for c in chunks) + [size]
        out = {}
        for path, ch in entries:
            data = b''
            for c in ch:
                off, _stored, usize, flags, _arch = chunks[c]
                end = starts[starts.index(off) + 1]
                f.seek(off)
                raw = f.read(end - off)
                if flags & 0x100:  # stored
                    data += raw[:usize]
                else:  # a 13-byte LZMA header (properties, dictionary size, size), then a raw LZMA1 stream
                    p = raw[0]
                    filt = [{'id': lzma.FILTER_LZMA1, 'lc': p % 9, 'lp': (p // 9) % 5, 'pb': p // 45,
                             'dict_size': int.from_bytes(raw[1:5], 'little')}]
                    data += lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filt).decompress(raw[13:])[:usize]
            out[path] = data
        return out


if __name__ == '__main__':
    pack, outdir = sys.argv[1], sys.argv[2]
    for path, data in files(pack).items():
        dst = os.path.join(outdir, path)
        os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
        open(dst, 'wb').write(data)
        print('%9d  %s' % (len(data), path))
