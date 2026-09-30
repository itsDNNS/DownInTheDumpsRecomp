"""Generate the recompiled game code from the user's own copy of Down in the Dumps.

The port does not contain any code of the original program. At build time this script reads DID.EXE
from the original CD (an ISO image, a folder of ISO images or a folder with the CD contents), checks
that it is the known build, and translates it to C++:

    <out>/recomp/   the whole game program (recomp.py)
    <out>/codec/    the video/audio codecs as self-contained routines (lift.py)

Usage: python generate.py --game <DID.EXE | .iso | folder> --out <dir>
Needs: Python 3.8+, capstone (pip install capstone)
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# builds of DID.EXE the recompiler configuration (data/*) was made for
KNOWN = {
    '9556506e3160fc05b4fb0dfa7c3e49e95727cbe2': 'original 1996 release (German and English CDs), 575,949 bytes',
}

# self-contained codec routines (lifted separately for the port's codec library and tests)
LIFTS = [
    ('codec/lifted_fil.cpp', 'lifted_fil_frame', '0x2cb15', ['0x2623e-0x2c8dd', '0x2cb15-0x2ce68', '0x2cfa6-0x2e1ed']),
    ('codec/lifted_decomp.cpp', 'lifted_decomp', '0x2623e', ['0x2623e-0x2c8dd']),
]


# ---------------------------------------------------------------- ISO 9660 (2048-byte sectors)

def iso_read_file(iso, path_parts):
    """contents of a file inside an ISO image (case-insensitive path), or None"""
    with open(iso, 'rb') as f:
        def read(pos, n):
            f.seek(pos)
            return f.read(n)
        pvd = read(16 * 2048, 2048)
        if len(pvd) < 2048 or pvd[1:6] != b'CD001':
            return None
        lba, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
        for k, want in enumerate(path_parts):
            data = read(lba * 2048, size)
            found = None
            p = 0
            while p < len(data):
                ln = data[p]
                if ln == 0:
                    p = (p // 2048 + 1) * 2048
                    continue
                nlen = data[p + 32]
                name = data[p + 33:p + 33 + nlen].decode('ascii', 'replace').split(';')[0].rstrip('.')
                if name.upper() == want.upper():
                    found = (struct.unpack_from('<I', data, p + 2)[0], struct.unpack_from('<I', data, p + 10)[0],
                             bool(data[p + 25] & 2))
                    break
                p += ln
            if not found:
                return None
            lba, size, is_dir = found
            if (k < len(path_parts) - 1) != is_dir:
                return None
        return read(lba * 2048, size)


def candidates(game):
    """(description, DID.EXE bytes) for every copy of DID.EXE found at `game`"""
    p = Path(game)
    def child(d, name):
        return next((c for c in d.iterdir() if c.name.upper() == name), None) if d.is_dir() else None
    if p.is_file():
        if p.suffix.upper() == '.ISO':
            data = iso_read_file(p, ['DID.EXE'])
            if data:
                yield str(p), data
        else:
            yield str(p), p.read_bytes()
        return
    if not p.is_dir():
        return
    dirs = [p] + sorted(c for c in p.iterdir() if c.is_dir())
    for d in dirs:
        exe = child(d, 'DID.EXE')
        if exe:
            yield str(exe), exe.read_bytes()
    for d in dirs:
        for iso in sorted(c for c in d.iterdir() if c.is_file() and c.suffix.upper() == '.ISO'):
            data = iso_read_file(iso, ['DID.EXE'])
            if data:
                yield '%s (in the ISO image)' % iso, data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', required=True, help='DID.EXE, a CD 1 ISO image, or a folder with ISOs/CD contents')
    ap.add_argument('--out', required=True, help='output directory for the generated sources')
    a = ap.parse_args()
    try:
        import capstone  # noqa: F401
    except ImportError:
        sys.exit('error: the Python package "capstone" is missing - install it with: pip install capstone')

    found, seen = None, []
    for desc, data in candidates(a.game):
        sha = hashlib.sha1(data).hexdigest()
        seen.append((desc, len(data), sha))
        if sha in KNOWN:
            found = (desc, data, sha)
            break
    if not found:
        if not seen:
            sys.exit('error: no DID.EXE found in %s (expected the ISO images or the contents of CD 1)' % a.game)
        msg = ['error: no supported DID.EXE found. Found:']
        msg += ['  %s: %d bytes, SHA-1 %s' % s for s in seen]
        msg.append('Supported: ' + '; '.join('%s (%s)' % (k, v) for k, v in KNOWN.items()))
        sys.exit('\n'.join(msg))
    desc, data, sha = found
    print('DID.EXE: %s - %s' % (desc, KNOWN[sha]))

    out = Path(a.out).resolve()
    obj = out / 'obj'
    for d in (obj, out / 'recomp', out / 'codec'):
        d.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(HERE))
    import le_loader
    r = le_loader.load(data)
    for i, img in enumerate(r['images']):
        (obj / ('obj%d.bin' % (i + 1))).write_bytes(img)

    env = dict(os.environ, BLUB_OBJ_DIR=str(obj), BLUB_OUT=str(out / 'recomp'), BLUB_CONFIG=str(HERE / 'data'))
    subprocess.run([sys.executable, str(HERE / 'recomp.py')], env=env, check=True)
    for fn, name, entry, ranges in LIFTS:
        subprocess.run([sys.executable, str(HERE / 'lift.py'), str(out / fn), name, entry] + ranges, env=env, check=True)
    (out / 'stamp.txt').write_text('DID.EXE %s\n' % sha)
    print('generated sources in %s' % out)


if __name__ == '__main__':
    main()
