MHForever @VERSION@
An unofficial Monster Hunter recompilation project
===================================================

MHForever lets you play Monster Hunter Freedom Unite (PSP) natively on your PC.
It is NOT affiliated with, endorsed by or sponsored by Capcom or Sony.
It contains none of the game's assets (no disc image, graphics, audio or text): you need your own copy
of the game.


What you need
-------------
* A 64-bit Linux distribution (glibc) with a graphics driver that supports OpenGL 3.3, on X11 or Wayland.
* WebKitGTK 4.1 and GTK 3, which the launcher uses to show its window.
  Debian/Ubuntu: libwebkit2gtk-4.1-0 libgtk-3-0
  Fedora:        webkit2gtk4.1 gtk3
  Arch:          webkit2gtk-4.1 gtk3
  SDL3 is linked into the programs, so it does not need to be installed.
* FFmpeg's libavcodec and libavutil (the movies and the music use them), version 7.1: libavcodec.so.61 and
  libavutil.so.59. Install your distribution's FFmpeg libraries; a different major version will not load.
* A plain (uncompressed) .iso image of your own copy of Monster Hunter Freedom Unite,
  European release (disc ID ULES01213, version 1.01). CSO/ZSO compressed images are not supported.
* A controller is optional. Gamepads work out of the box (tested with a PS5 DualSense).
  Keyboard: arrows = D-pad, WASD = analog stick, K/L/J/I = cross/circle/square/triangle,
  Q/E = L/R, Enter = Start, Backspace = Select. Change them, and the controller's buttons, in Settings -> Controls
  or from the in-game menu (Escape, Controls tab).
  Escape (or the controller's guide button) pauses the game and opens a menu: volume, resolution, frame rate,
  fullscreen and quit. It works with the keyboard, a controller or the mouse.


How to play
-----------
1. Extract this folder anywhere you can write to, for example your home folder.
   The two programs must keep their execute permission. If your file manager removed it:
       chmod +x MHForever mhfu
2. Open MHForever.
3. Add your .iso. The launcher checks your copy and prepares what it needs from it.
   In Settings you can choose to keep a copy of the image in MHForever's own folder.
4. Press Play.

Your settings and library live in $XDG_DATA_HOME/MHForever (usually ~/.local/share/MHForever).
Your saves live in ~/.local/share/MHForever/saves (one folder per game), so updating MHForever never loses them.
They use the same format as the PSP and PPSSPP: you can bring your own saves from a PSP memory stick or from
PPSSPP with Settings > Saves > Choose folder (saves of your own version of the game only; the one you have now is
kept in a backup folder), and you can copy MHForever's saves into PPSSPP's memory stick folder.
Prepared game data and your library are stored in the same MHForever folder. Removing a game from the launcher
deletes only its prepared data (never your original .iso or your saves).


Steam Deck
----------
This release has not been tested on a Steam Deck. It is a normal desktop build: extract the folder and add MHForever as
a non-Steam game (Game Mode > Add a Non-Steam Game). Controller-only use, 1280x800 and Game Mode are untested; reports
are welcome.


If something does not start
---------------------------
* "The launcher needs WebKitGTK (libwebkit2gtk-4.1)": install the WebKitGTK 4.1 package listed above.
* "Cannot create the window" or a black window: update your graphics driver, or try the X11 session instead of Wayland.
* The game closes right away: its log is in game.log next to MHForever; the launcher shows its last lines.
* Missing system libraries: run the program in a terminal to see which one the dynamic loader reports.


Status
------
Early release. Freedom Unite runs at 30 fps, as on the original console. Settings also
offers 60 to 180 fps (any multiple of 30): the game still runs at 30 and the extra pictures
are drawn in between, so it is experimental and heavy scenes may not hold the highest rates.
Online multiplayer is being worked on. Expect rough edges.


Source code and licenses
------------------------
MHForever is free software, GPL-3.0-or-later: see LICENSE.
Source code of MHForever and bug reports: https://github.com/Mango77x/MHForever
Third-party components and their licenses: THIRD_PARTY.md and the licenses folder.
