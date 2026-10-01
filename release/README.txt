Down in the Dumps (Philips Media / Haiku Studios, 1996) on modern systems
=========================================================================

This folder contains a new build of the game program for today's Windows PCs
(on Linux and the Steam Deck through Proton or Wine). The original program
DID.EXE ran under DOS; it has been translated instruction by instruction to C++
(static recompilation). DOS, the DOS memory manager, VESA graphics, mouse,
CD drive and the sound library are emulated by this build. No DOSBox needed.

The game data itself (graphics, sound, videos, texts, scripts and DID.EXE) is NOT
included. It is read directly from your original CDs while you play.

The game keeps its original speed (mostly 8 frames per second); the mouse
pointer moves smoothly at the refresh rate of your screen.

The game runs in the language of your CDs; this build does not translate the game.
It has been tested with the German release of 1996. Other releases (e.g. English)
work if they contain the same DID.EXE - the settings window checks this. The
settings window itself is available in English and German.


Getting started
---------------

1. Put the ISO images of your three original CDs into the folder "ISOs".
   (The file names do not matter.)

2. Start blub.exe (on Linux: see below).

3. The "Game" tab of the settings window should say:
     "DID.EXE recognized: original 1996 program"
     "All chapters present - ready to play"
   Then click "Play".

When you quit the game you return to the settings window.


Settings
--------

Game               folder with the ISOs (default: the "ISOs" folder here),
                   folder for saved games, "Show intro again"
Display            fullscreen, window size (1x to 4x), scaling, VSync,
                   flicker-free page flipping
                   Scaling: "Sharp pixels" like the original, "Smooth" (blurred)
                   or "xBRZ" (high quality: smooth, sharp edges)
Sound & controls   sound on/off, volume, "Esc skips videos"
Info               language of the settings window
"Original setup" starts the setup program of the CD (system overview).

The settings are stored in blub.ini, saved games in the folder "Saves" - both are
created here in this folder at the first start. The whole folder can be moved or
copied to another computer.


Controls in the game
--------------------

Mouse            everything: look, click, use objects
Space bar        skip video / cut scene (optionally also Esc)
P                pause
F2               show hotspots on/off: the clickable areas of the scene
                 (yellow: something happens on a click, blue: reacts to the
                 pointer only)
F11, Alt+Enter   fullscreen on/off
Alt+F4           quit the game

Game controllers (Xbox, PlayStation, Switch, Steam Deck):
Left stick, d-pad  move the pointer      Right stick    move it slowly (aiming)
A                  click                 B              skip video / cut scene
X                  show hotspots         LB / RB        pointer to the previous /
Start              pause                                next hotspot
(can be switched off under "Sound & controls")


Problems?
---------

If the game stops because of an error, the settings window shows an error
report (version, system, the error - no personal data). "Report on GitHub..."
copies it to the clipboard and opens the bug report form; please paste it there
and describe where in the game it happened. Without an error message, attach
blub.log (its location is shown under "Info").


Windows: Smart App Control
--------------------------

blub.exe is not digitally signed. If "Smart App Control" is switched on in
Windows 11, Windows may block the program ("An Application Control policy has
blocked this file"). Then switch Smart App Control off: Windows Security > App &
browser control > Smart App Control. Since the Windows update of April 2026 it can
be switched on again later without reinstalling Windows.

The SmartScreen warning "Windows protected your PC" can be passed with
"More info" > "Run anyway".


Linux and Steam Deck (Proton / Wine)
------------------------------------

There is only the Windows build; it runs under Proton and Wine.

Steam (including the Steam Deck):
  1. Steam > "Games" > "Add a Non-Steam Game to My Library..." > choose blub.exe
     from this folder.
  2. Right-click the entry > Properties > Compatibility > "Force the use of a
     specific Steam Play compatibility tool" > a current Proton version.
  3. Start. On the Steam Deck the controller layout "Gamepad" works: the left
     stick moves the pointer, A clicks (the trackpads can stay a mouse).

Wine:
  wine blub.exe      (run it in the folder of this game)

The whole folder (with the ISOs) must be reachable for Proton/Wine; a folder in
your home directory is easiest. Settings and saved games are kept here in the
folder, just like on Windows.


Contents of this folder
-----------------------

blub.exe          the game with its settings window (Windows 10/11, 64 bit;
                  Linux/Steam Deck through Proton or Wine)
ISOs\             put your CD images here
Licenses\         license of this port (GNU GPL v3.0 or later) and of the libraries used
                  (SDL2, Dear ImGui, tinyfiledialogs, xBRZ)
README.txt        this file
LIESMICH.txt      the same in German
