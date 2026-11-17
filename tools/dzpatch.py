# Writes a copy of a Marmalade DTRZ data pack (.dz) with some of its files replaced. Each replacement's
# data is appended at the end and its chunk entry is pointed there. The header keeps its size, so nothing
# else moves. Only existing files can be replaced (adding a file would grow the header).
#
# DTRZ layout: "DTRZ" [file count u16][dir count u16][version u8], file names (C strings), dir names
# (C strings, dir 0 is the root and has none), per file: [dir index u16][chunk indices u16...][0xffff],
# [archive count u16][chunk count u16], chunks: [offset u32][stored size u32][size u32][flags u16][archive u16].
# Flag 0x100 is stored (uncompressed); every chunk in the game's pack is stored.
#
# usage as a library: patch(src_dz, dst_dz, {"levels/kino/kino_statics.group.bin": bytes, ...})
import os
import shutil
import struct


def read_header(f):
    d = f.read(1 << 20)
    assert d[:4] == b'DTRZ', 'not a DTRZ pack'
    nfiles, ndirs, _ver = struct.unpack_from('<HHB', d, 4)
    o = 9

    def cstr():
        nonlocal o
        e = d.index(b'\0', o)
        s = d[o:e].decode('latin1')
        o = e + 1
        return s

    names = [cstr() for _ in range(nfiles)]
    dirs = [''] + [cstr() for _ in range(ndirs - 1)]
    files = []
    for i in range(nfiles):
        di, = struct.unpack_from('<H', d, o)
        o += 2
        ch = []
        while True:
            c, = struct.unpack_from('<H', d, o)
            o += 2
            if c == 0xffff:
                break
            ch.append(c)
        path = (dirs[di].replace('\\', '/') + '/' if dirs[di] else '') + names[i]
        files.append((path, ch))
    _narch, nchunks = struct.unpack_from('<HH', d, o)
    o += 4
    chunk_table = o
    chunks = [struct.unpack_from('<IIIHH', d, chunk_table + 16 * i) for i in range(nchunks)]
    return files, chunks, chunk_table


def read_file(dz, path):
    with open(dz, 'rb') as f:
        files, chunks, _ = read_header(f)
        for p, ch in files:
            if p == path:
                out = b''
                for c in ch:
                    off, clen, dlen, fl, _arc = chunks[c]
                    assert fl == 0x100 and clen == dlen, f'{path}: chunk flags {fl:#x}'
                    f.seek(off)
                    out += f.read(clen)
                return out
    raise KeyError(path)


def patch(src, dst, replacements):
    with open(src, 'rb') as f:
        files, chunks, chunk_table = read_header(f)
    index = {p: ch for p, ch in files}
    tmp = dst + '.tmp'
    shutil.copyfile(src, tmp)
    with open(tmp, 'r+b') as f:
        f.seek(0, os.SEEK_END)
        for path, data in replacements.items():
            ch = index.get(path)
            if ch is None:
                raise KeyError(f'{path} is not in {src}')
            if len(ch) != 1:
                raise ValueError(f'{path} has {len(ch)} chunks; only single-chunk files are supported')
            c = ch[0]
            _off, _clen, _dlen, fl, arc = chunks[c]
            pos = f.seek(0, os.SEEK_END)
            pos += -pos % 16
            f.seek(pos)
            f.write(data)
            f.seek(chunk_table + 16 * c)
            f.write(struct.pack('<IIIHH', pos, len(data), len(data), 0x100, arc))
            f.seek(0, os.SEEK_END)
    os.replace(tmp, dst)
