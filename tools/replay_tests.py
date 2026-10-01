"""Playthrough tests: replays recorded sessions and compares the screen checkpoints with the reference.

Usage: python tools/replay_tests.py --blub <path to blub(.exe)> --game <folder with the ISO images>
                                     [--update] [recordings ...]

A recording (tests/recordings/<name>.rec.txt) is the input of a played session, made with
    blub --record <name>.rec.txt
Its reference (<name>.chk.txt) holds a hash of the screen for every second of game time. A replay
runs without a window and much faster than real time. The test fails when the game stops with an
error or when a checkpoint differs from the reference: then the game behaves differently than when
the reference was made - check whether that is intended, and if so write new references with --update.
"""
import argparse
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECORDINGS = HERE.parent / 'tests' / 'recordings'


def read_checkpoints(path):
    out = []
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if len(parts) == 2:
            out.append((int(parts[0]), parts[1]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--blub', required=True, help='the built program')
    ap.add_argument('--game', required=True, help='folder with the ISO images (or the CD contents)')
    ap.add_argument('--update', action='store_true', help='write the reference checkpoints instead of comparing')
    ap.add_argument('recordings', nargs='*', help='default: tests/recordings/*.rec.txt')
    a = ap.parse_args()

    recordings = [Path(r) for r in a.recordings] or sorted(RECORDINGS.glob('*.rec.txt'))
    if not recordings:
        sys.exit('no recordings found')
    failed = 0
    for rec in recordings:
        name = rec.name[:-len('.rec.txt')] if rec.name.endswith('.rec.txt') else rec.stem
        ref = rec.with_name(name + '.chk.txt')
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'checkpoints.txt'
            started = time.time()
            r = subprocess.run([str(Path(a.blub).resolve()), '--game', a.game, '--replay', str(rec.resolve()),
                                '--checkpoints', str(out), '--headless'])
            seconds = time.time() - started
            got = read_checkpoints(out) if out.exists() else []
        if r.returncode != 0:
            print('FAIL %s: the game stopped with an error after %d s of game time (see the error report)' % (name, len(got)))
            failed += 1
            continue
        if a.update:
            ref.write_text(''.join('%d %s\n' % c for c in got))
            print('updated %s: %d checkpoints (%.0f s)' % (ref.name, len(got), seconds))
            continue
        if not ref.exists():
            print('FAIL %s: no reference %s (make it with --update)' % (name, ref.name))
            failed += 1
            continue
        want = read_checkpoints(ref)
        diff = next((i for i, (w, g) in enumerate(zip(want, got)) if w != g), None)
        if diff is not None:
            print('FAIL %s: the screen differs from second %d of game time on' % (name, want[diff][0]))
            failed += 1
        elif len(got) != len(want):
            print('FAIL %s: %d checkpoints instead of %d' % (name, len(got), len(want)))
            failed += 1
        else:
            print('ok   %s: %d s of game time in %.0f s' % (name, len(got), seconds))
    if failed:
        sys.exit('%d of %d recordings failed' % (failed, len(recordings)))


if __name__ == '__main__':
    main()
