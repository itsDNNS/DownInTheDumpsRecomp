# DownInTheDumpsRecomp

*Unofficial native PC port of Down in the Dumps (Philips Media / Haiku Studios, 1996) via static
recompilation – requires the original CDs.*

A native Windows version of **Down in the Dumps** (Philips Media / Haiku Studios, 1996), the
cartoon adventure about the Blub family who crash-land on a rubbish dump. The original DOS program
`DID.EXE` is **statically recompiled to C++**: every function of the game is translated
instruction by instruction into C++ that runs natively. The parts of the PC it relied on – DOS,
DOS/4GW, VESA graphics, mouse, CD-ROM drive, timer and the HMI SOS sound library – are replaced by
a small host layer on top of [SDL2](https://www.libsdl.org/). It is not an emulator and needs no
DOSBox. The program is called `blub` – after the Blub family and the name of the
original engine.

> **This repository contains no code and no data of the game.** You need your own original CDs.
> The C++ translation of the game program is generated on your computer, from your `DID.EXE`,
> when you build the project.

## Features

- plays directly from the **ISO images of the three original CDs** (no installation, no disc swapping)
- settings window: game data, fullscreen, window size (1x–4x), scaling (sharp pixels, smooth or
  the [xBRZ](https://sourceforge.net/projects/xbrz/) upscaler), VSync, volume, "Esc skips videos",
  German / English
- **xBRZ upscaling**: smooth, sharp edges instead of big pixels; only the parts of the picture that
  changed are scaled again, using all CPU cores
- **hotspot display** (F2): shows the clickable areas of the current scene, read from the game's
  own button list (yellow: something happens on a click, blue: reacts to the pointer only)
- **smooth mouse pointer**: like on the CRT monitors of 1996, the pointer moves at the refresh rate
  of your screen (up to 240 Hz) – the game itself keeps its original frame rate (mostly 8 frames
  per second)
- light on the CPU: pictures are only drawn when something changed, and the pause screen and the
  settings window wait for input instead of keeping a CPU core busy
- **game controllers** (Xbox, PlayStation, Switch, Steam Deck): the stick moves the pointer, A
  clicks, LB/RB jump to the previous/next hotspot
- **error reports**: if the game stops because of an error, the settings window shows a report
  without personal data and opens a prefilled GitHub bug report
- portable: settings and saved games stay in the program folder
- runs on Windows 10/11; on Linux and the Steam Deck with Proton or Wine
- timing issues of the original fixed properly (video/sound synchronisation on fast CPUs, CD speed test)

## Supported versions

| Release | `DID.EXE` SHA-1 | Status |
|---|---|---|
| German CDs (1996) | `9556506e3160fc05b4fb0dfa7c3e49e95727cbe2` | tested |
| English CDs (1996) | same file | intro, main menu, first chapter tested |

The recompilation is made for exactly this build of `DID.EXE` (575,949 bytes). Other builds are
recognised and rejected with their checksum – please report them.

**Status:** intro, main menu, dialogues, walking, scene changes, camera moves, videos and sound
work. A complete playthrough of all chapters has not been done yet – bug reports are welcome.

## Building

**The easy way (Windows):** download this repository (Code → Download ZIP, or `git clone`),
double-click `build.cmd` and choose the folder with your ISO images. The script downloads the build
tools once into `.tools` (LLVM-MinGW, CMake, Ninja, Python with capstone: pinned versions, checked
with SHA-256, about 900 MB; nothing is installed), generates the game code from your `DID.EXE`,
compiles it and puts the ready-to-play folder `Down in the Dumps` next to the source code. On a
current PC this takes a few minutes; afterwards build again after every update.

**By hand:** requirements (Windows):

- [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw) (clang), [CMake](https://cmake.org/) 3.20+,
  [Ninja](https://ninja-build.org/)
- Python 3.8+ with [capstone](https://www.capstone-engine.org/): `pip install capstone`
- your original CDs as ISO images (or the copied CD contents)

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBLUB_GAME_DATA="C:/path/to/the/ISOs"
cmake --build build
python tools/package.py --build build
```

`BLUB_GAME_DATA` may be a folder with the ISO images (also in subfolders), a single CD 1 ISO, a
folder with the CD contents, or `DID.EXE` itself. CMake then runs `generator/generate.py`, which
reads `DID.EXE`, checks it, and writes the generated C++ to `build/generated/`. SDL2 is downloaded
by CMake (pinned release) and linked statically. After an update of this repository just build
again: when the generator or its configuration changed, the game code is generated anew.

`tools/package.py` puts the ready-to-play folder into `build/Down in the Dumps/`. Copy your three
ISO images into its `ISOs` folder and start `blub.exe`.

A native Linux build works as well (GCC or Clang, system SDL2: `libsdl2-dev`), but the supported
way on Linux is the Windows build with Proton/Wine. MSVC is not supported: the generated code uses
GCC/Clang builtins.

## Playing

`blub.exe` opens the settings window. It shows whether the game data is complete (discs found,
`DID.EXE` recognised, language of the game data, chapters) – then click **Play**. When the game
ends you are back in the settings window.

| Key | Function |
|---|---|
| Mouse | everything: look, click, use objects |
| Space (optionally Esc) | skip video / cut scene |
| P | pause |
| F2 | show hotspots (clickable areas) on/off |
| F11, Alt+Enter | fullscreen on/off |
| Alt+F4 | quit |

| Controller | Function |
|---|---|
| left stick, d-pad | move the pointer |
| right stick | move the pointer slowly (aiming) |
| A | click |
| B | skip video / cut scene |
| X | show hotspots on/off |
| LB / RB | pointer to the previous / next hotspot |
| Start | pause |
| Back / Select | like Esc |

Command line: `blub --help` (starts the game directly, e.g. `blub --game <ISO folder> --fullscreen`).

**Windows Smart App Control** blocks unsigned programs like a self-built `blub.exe`. Switch it off
(Windows Security → App & browser control → Smart App Control); since the April 2026 update it can
be switched on again later.

**Proton (Steam, Steam Deck):** add `blub.exe` as a non-Steam game and force a Proton version in its
compatibility settings. The controller layout "Gamepad" works (stick and A); the trackpads of the
Steam Deck can stay a mouse. **Wine:** `wine blub.exe`.

## Reporting bugs

If the game stops because of an error, the settings window shows an error report: the version of
blub, the system, the error and the last files the game opened – no personal data. **Report on
GitHub...** copies it to the clipboard and opens the bug report form. Without an error message,
please attach `blub.log` (its location is shown under Info) and describe where in the game it
happened; a saved game from shortly before helps a lot.

## How it works

```
generator/   generate.py   finds DID.EXE on the CD (ISO 9660 or folder), checks the SHA-1
             le_loader.py  loads the LE executable (DOS/4GW), applies relocations
             recomp.py     translates the 572 game functions to C++ (x86 + x87 semantics)
             lift.py       translates the video/audio codecs as self-contained routines
             data/         function list (from a Ghidra analysis), host functions, hooks,
                           corrections of function boundaries
src/engine/  recomp/       runtime of the generated code: CPU state, flags, x87, memory arena
             host/         the machine: DOS files (read-only from the discs, writes to the save
                           folder), DPMI, VESA 640x480 with bank switching and page flipping, VGA
                           palette, mouse, keyboard, MSCDEX, timer, SOS sound mixer, C runtime,
                           hotspot display (reads the game's button tree)
             data/         game data access: ISO 9660 reader, GAP archives, LE executables
             codec/, gfx/  codecs used by the tests, SDL display with the xBRZ upscaler
src/         main.cpp, launcher.cpp (settings window, Dear ImGui)
tests/       codec and recompilation tests against reference data made with the original code,
             recordings/ playthrough tests
tools/       build.ps1 (build.cmd), package.py, replay_tests.py
```

Some behaviour of the 1996 program had to be handled explicitly, e.g. wait loops calibrated for
1996 CPUs (host hooks instead of busy loops), a patch the game applies to its own sound library at
start-up, and a memory allocation that only works if the first request for 640 KB of DOS memory
fails – as it always did on real PCs. The game also has a few bugs of its own, found by the
automatic exploration below, which blub fixes (`data/recomp_hooks.txt`):

- a scene change while the inventory bar holds an object left the bar with freed memory;
- a dialogue or gag starting while another one runs on the same character (a dialogue while an idle
  animation starts, a second click while the character speaks) lost the character's walking
  animation, and walking then read past the end of a frame table – on DOS both ended with a page
  fault;
- near the screen edges the click point followed the pointer sprite, which is kept on the screen,
  instead of the mouse: with the open hand as pointer, LOAD in the top bar could not be clicked;
- the end of a gag or of a video sequence freed the memory of a scene transition that had started
  meanwhile, and the next sound overwrote the transition's data while it played;
- in chapter 1, leaving the scene with the slow-motion button while its looping animation played
  made the button wait forever the next time it was pressed, with the pointer hidden;
- the inventory bar, sliding away, wrote 1280 bytes past the end of its buffer into the next memory
  block – in chapter 2 the header of a sound animation, which after saving (the game reloads itself
  then) ran past its data;
- a gag of the English release (chapters 1 and 6) has picture deltas with empty lines, on which the
  game's decoder ran through all memory.

Should the game use any other address outside its memory, blub stops with an error report instead
of letting it overwrite the game's own data.

Tests (need the German CDs): `cmake --build build --target blub_tests`, then
`build/blub_tests tests/fixtures <ISO folder>`.

**Playthrough tests.** With `--record FILE` (and `--replay FILE`) the game runs on a virtual clock
that only advances where the game waits or polls, so the same input gives exactly the same game.
`blub --record mysession.rec.txt` records the input while you play (in real time, starting with
empty saved games); `blub --replay mysession.rec.txt --checkpoints out.txt --headless` plays it back
without a window, about 40 times faster than real time, and writes a hash of the screen for every
second of game time. `python tools/replay_tests.py --blub <blub.exe> --game <ISO folder>` replays
all recordings in `tests/recordings` and compares them with their references (`.chk.txt`; write new
ones with `--update` after an intended change). The Windows and the Linux build give the same
checkpoints.

**Automatic exploration.** `blub --explore SEED --record run.rec.txt --headless --quit-after 1800
-- /DEMO TOON1\CARTOON1.EXP` lets the game play by itself for 30 minutes of game time (in well
under a minute): whenever the game waits for the player it clicks hotspots of the scene (the ones
not tried yet first), uses objects of the inventory on them (where the game shows a hint), walks
around, now and then skips a sequence, and saves the game every 15 minutes and loads it again 2
minutes later. The same seed gives the same run, and the recording reproduces it exactly – an error
it runs into can be replayed and debugged. `/DEMO` lets `DID.EXE` start a chapter directly
(`TOON1`…`TOON4`, `TOON6`). In test scripts, the event `hotspot_list` prints the clickable areas.

## Legal

This is an unofficial fan project, not affiliated with Philips, Haiku Studios or any rights
holder. *Down in the Dumps* is protected by copyright; you need your own copy. This repository
contains only original code of this project, the function list/configuration for the
recompilation and third-party libraries – no game code, graphics, sound or text.

A build of this project contains code generated from your `DID.EXE`. **Please do not distribute
builds** (e.g. `blub.exe`) – share the source code instead.

## Licenses

- This project: [GNU General Public License v3.0 or later](LICENSE) (GPL-3.0-or-later)
- [SDL2](https://www.libsdl.org/) – zlib license (downloaded at build time)
- [Dear ImGui](https://github.com/ocornut/imgui) – MIT license (`third_party/imgui`)
- [tinyfiledialogs](https://sourceforge.net/projects/tinyfiledialogs/) – zlib license (`third_party/tinyfiledialogs`)
- [xBRZ](https://sourceforge.net/projects/xbrz/) 1.9 – GNU GPL v3.0 (`third_party/xbrz`)
