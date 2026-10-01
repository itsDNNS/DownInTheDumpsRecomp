"""Assemble the ready-to-play folder from a build:

    <out>/blub.exe (or blub)        the game with its settings window
    <out>/README.txt, LIESMICH.txt  instructions (English, German)
    <out>/ISOs/                     where the player puts the CD images
    <out>/Licenses/                 license of this port (GPL-3.0) and of SDL2, Dear ImGui, tinyfiledialogs, xBRZ

Usage: python tools/package.py --build <build dir> [--out <folder>]
The folder contains only this port - no code or data of the game.
"""
import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def crlf(src, dst):
    text = Path(src).read_text(encoding='utf-8').replace('\r\n', '\n')
    Path(dst).write_bytes(text.replace('\n', '\r\n').encode('utf-8'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', required=True, help='CMake build directory with the built blub program')
    ap.add_argument('--out', default=None, help='output folder (default: <build>/Down in the Dumps)')
    a = ap.parse_args()
    build = Path(a.build).resolve()
    exe = next((build / n for n in ('blub.exe', 'blub') if (build / n).is_file()), None)
    if not exe:
        sys.exit('error: no blub program in %s - build it first (cmake --build %s)' % (build, a.build))
    sdl_license = next((p for p in (ROOT / 'third_party' / 'SDL2' / 'LICENSE.txt',
                                    build / '_deps' / 'sdl2-src' / 'LICENSE.txt') if p.is_file()), None)
    out = Path(a.out).resolve() if a.out else build / 'Down in the Dumps'
    if out.exists():
        shutil.rmtree(out)
    (out / 'ISOs').mkdir(parents=True)
    (out / 'Licenses').mkdir()
    shutil.copy2(exe, out / exe.name)
    rel = ROOT / 'release'
    crlf(rel / 'README.txt', out / 'README.txt')
    crlf(rel / 'LIESMICH.txt', out / 'LIESMICH.txt')
    crlf(rel / 'ISOs.txt', out / 'ISOs' / 'PUT YOUR ISOs HERE.txt')
    crlf(ROOT / 'LICENSE', out / 'Licenses' / 'blub (GPL-3.0).txt')
    crlf(ROOT / 'third_party' / 'imgui' / 'LICENSE.txt', out / 'Licenses' / 'Dear ImGui.txt')
    crlf(rel / 'tinyfiledialogs.txt', out / 'Licenses' / 'tinyfiledialogs.txt')
    crlf(ROOT / 'third_party' / 'xbrz' / 'License.txt', out / 'Licenses' / 'xBRZ (GPL-3.0).txt')
    if sdl_license:
        crlf(sdl_license, out / 'Licenses' / 'SDL2.txt')
    elif exe.suffix == '.exe':
        print('warning: SDL2 license text not found')
    for f in sorted(out.rglob('*')):
        if f.is_file():
            print('%9d  %s' % (f.stat().st_size, f.relative_to(out)))
    print('-> %s' % out)


if __name__ == '__main__':
    main()
