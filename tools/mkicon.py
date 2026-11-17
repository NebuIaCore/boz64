# Makes pc/boz64.ico (the Windows build's icon) from the game's own launcher icons in the original APK
# (res/drawable-*/icon.png). The .ico holds the PNGs as they are (Windows reads PNG icon images since Vista).
# usage: python tools/mkicon.py
import os
import struct
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APK = os.path.join(ROOT, '..', 'backup', 'apk', 'base.apk')
OUT = os.path.join(ROOT, 'pc', 'boz64.ico')
DENSITIES = ['ldpi', 'mdpi', 'hdpi', 'xhdpi', 'xxhdpi', 'xxxhdpi']


def main():
    z = zipfile.ZipFile(APK)
    names = set(z.namelist())
    images = []
    for d in DENSITIES:
        n = 'res/drawable-%s-v4/icon.png' % d
        if n not in names:
            continue
        png = z.read(n)
        if png[:8] != b'\x89PNG\r\n\x1a\n':
            sys.exit('%s is not a PNG' % n)
        w, h = struct.unpack('>II', png[16:24])
        if w <= 256 and h <= 256:
            images.append((w, h, png))
    if not images:
        sys.exit('no launcher icons in ' + APK)
    images.sort(key=lambda i: i[0])
    out = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for w, h, png in images:
        out += struct.pack('<BBBBHHII', w % 256, h % 256, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
    for _, _, png in images:
        out += png
    with open(OUT, 'wb') as f:
        f.write(out)
    print('%s: %s' % (os.path.normpath(OUT), ', '.join('%dx%d' % (w, h) for w, h, _ in images)))


if __name__ == '__main__':
    main()
