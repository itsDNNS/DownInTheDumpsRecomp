"""Load a DOS4GW LE executable into flat memory images with fixups applied.

Output (in work/):
  obj1.bin, obj2.bin   - object images at their preferred base addresses
  layout.json          - object bases/sizes, entry point, fixup source addresses
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load(path_or_bytes):
    d = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else Path(path_or_bytes).read_bytes()
    le = struct.unpack_from('<I', d, 0x3C)[0]
    assert d[le:le + 2] == b'LE'
    u32 = lambda o: struct.unpack_from('<I', d, le + o)[0]
    page_size = u32(0x28)
    last_page = u32(0x2C)
    n_pages = u32(0x14)
    obj_tab, n_objs, page_map = u32(0x40), u32(0x44), u32(0x48)
    fix_page_tab, fix_rec_tab = u32(0x68), u32(0x6C)
    data_pages = u32(0x80)  # absolute file offset
    eip_obj, eip = u32(0x18), u32(0x1C)
    esp_obj, esp = u32(0x20), u32(0x24)

    objs = []
    for i in range(n_objs):
        vsize, base, flags, pidx, pcnt, _ = struct.unpack_from('<6I', d, le + obj_tab + 24 * i)
        objs.append(dict(vsize=vsize, base=base, flags=flags, pidx=pidx, pcnt=pcnt))

    # page map: 3-byte big-endian page number + flags byte
    def page_file_off(p):  # p is 1-based logical page
        e = d[le + page_map + 4 * (p - 1): le + page_map + 4 * p]
        num = (e[0] << 16) | (e[1] << 8) | e[2]
        return data_pages + (num - 1) * page_size, (page_size if num != n_pages else last_page)

    images = []
    for o in objs:
        img = bytearray(o['vsize'])
        for k in range(o['pcnt']):
            off, sz = page_file_off(o['pidx'] + k)
            chunk = d[off:off + sz]
            dst = k * page_size
            n = min(len(chunk), len(img) - dst)
            img[dst:dst + n] = chunk[:n]
        images.append(img)

    fixups = []  # (obj_index, offset_in_obj, kind, target_obj, target_off)
    for oi, o in enumerate(objs):
        for k in range(o['pcnt']):
            p = o['pidx'] + k
            a = struct.unpack_from('<I', d, le + fix_page_tab + 4 * (p - 1))[0]
            b = struct.unpack_from('<I', d, le + fix_page_tab + 4 * p)[0]
            pos = le + fix_rec_tab + a
            end = le + fix_rec_tab + b
            while pos < end:
                src, flg = d[pos], d[pos + 1]
                pos += 2
                if src & 0x20:
                    cnt = d[pos]; pos += 1
                    srcoffs = None
                else:
                    srcoffs = [struct.unpack_from('<h', d, pos)[0]]; pos += 2
                assert flg & 3 == 0, 'only internal references supported'
                if flg & 0x40:
                    tobj = struct.unpack_from('<H', d, pos)[0]; pos += 2
                else:
                    tobj = d[pos]; pos += 1
                stype = src & 0x0F
                toff = 0
                if stype != 2:
                    if flg & 0x10:
                        toff = struct.unpack_from('<I', d, pos)[0]; pos += 4
                    else:
                        toff = struct.unpack_from('<H', d, pos)[0]; pos += 2
                if srcoffs is None:
                    srcoffs = list(struct.unpack_from('<%dh' % cnt, d, pos)); pos += 2 * cnt
                for so in srcoffs:
                    fixups.append((oi, k * page_size + so, stype, tobj - 1, toff))

    for oi, off, stype, tobj, toff in fixups:
        img = images[oi]
        target = (objs[tobj]['base'] + toff) & 0xFFFFFFFF
        if stype == 7:  # 32-bit offset
            if 0 <= off <= len(img) - 4:
                struct.pack_into('<I', img, off, target)
        elif stype == 8:  # 32-bit self-relative
            struct.pack_into('<I', img, off, (target - (objs[oi]['base'] + off + 4)) & 0xFFFFFFFF)
        elif stype == 2:  # 16-bit selector: leave as-is (flat model)
            pass
        else:
            raise ValueError('fixup type %x' % stype)

    return dict(objs=objs, images=images, fixups=fixups,
                entry=objs[eip_obj - 1]['base'] + eip,
                stack=objs[esp_obj - 1]['base'] + esp)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else ROOT / 'orig' / 'DID.EXE'
    r = load(exe)
    out = ROOT / 'work'
    layout = dict(entry=r['entry'], stack=r['stack'], objects=[])
    for i, (o, img) in enumerate(zip(r['objs'], r['images'])):
        fn = 'obj%d.bin' % (i + 1)
        (out / fn).write_bytes(img)
        layout['objects'].append(dict(file=fn, base=o['base'], size=o['vsize'],
                                      exec=bool(o['flags'] & 4), write=bool(o['flags'] & 2)))
    layout['fixups'] = [r['objs'][oi]['base'] + off for oi, off, t, *_ in r['fixups'] if t == 7]
    (out / 'layout.json').write_text(json.dumps(layout))
    print('entry=%#x objects=%s fixups=%d' % (r['entry'],
          [(hex(o['base']), hex(o['vsize'])) for o in r['objs']], len(r['fixups'])))


if __name__ == '__main__':
    main()
