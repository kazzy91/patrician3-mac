"""Make an .icns from the icon inside a Windows exe (the game's own icon, read from the user's copy).
    python3 scripts/icon.py Patrician3.exe out.icns
Writes the largest icon of the first icon group as an .ico; macOS's sips reads that, and iconutil builds the .icns."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile


def resources(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec, optsize = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    rva = struct.unpack_from("<I", data, opt + 96 + 2 * 8)[0]          # data directory 2: resources
    secs = [struct.unpack_from("<8sIIII", data, opt + optsize + 40 * i) for i in range(nsec)]

    def off(r):
        for _, vsize, va, rsize, raw in secs:
            if va <= r < va + max(vsize, rsize):
                return r - va + raw
        raise ValueError("rva %x not in a section" % r)

    root = off(rva)

    def entries(d):                                                      # [(id, is_dir, offset)] of a directory
        named, ids = struct.unpack_from("<HH", data, d + 12)
        out = []
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", data, d + 16 + 8 * i)
            out.append((None if name & 0x80000000 else name, bool(target & 0x80000000), root + (target & 0x7FFFFFFF)))
        return out

    def leaf(d):                                                         # first language's data of a name directory
        while True:
            _, is_dir, o = entries(d)[0]
            if not is_dir:
                r, size = struct.unpack_from("<II", data, o)
                return data[off(r):off(r) + size]
            d = o

    table = {}
    for typ, _, d in entries(root):
        if typ in (3, 14):
            table[typ] = [(name, o) for name, _, o in entries(d)]
    return table, leaf


def main(exe, icns):
    data = open(exe, "rb").read()
    table, leaf = resources(data)
    icons = {name: o for name, o in table.get(3, [])}
    group = leaf(table[14][0][1])
    count = struct.unpack_from("<H", group, 4)[0]
    best = None
    for i in range(count):
        w, h, colors, _, planes, bits, size, ident = struct.unpack_from("<BBBBHHIH", group, 6 + 14 * i)
        key = (w or 256, bits)
        if best is None or key > best[0]:
            best = (key, group[6 + 14 * i:6 + 14 * i + 12], ident)
    image = leaf(icons[best[2]])
    ico = struct.pack("<HHH", 0, 1, 1) + best[1][:8] + struct.pack("<II", len(image), 22) + image
    tmp = tempfile.mkdtemp()
    try:
        open(os.path.join(tmp, "a.ico"), "wb").write(ico)
        png = os.path.join(tmp, "a.png")
        subprocess.run(["sips", "-s", "format", "png", os.path.join(tmp, "a.ico"), "--out", png], check=True,
                       stdout=subprocess.DEVNULL)
        iconset = os.path.join(tmp, "a.iconset")
        os.mkdir(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = "icon_%dx%d%s.png" % (size, size, "@2x" if scale == 2 else "")
                subprocess.run(["sips", "-z", str(size * scale), str(size * scale), png, "--out",
                                os.path.join(iconset, name)], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", icns], check=True)
    finally:
        shutil.rmtree(tmp)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
