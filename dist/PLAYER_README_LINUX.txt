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
  FFmpeg (the movies and the music) is linked into the programs too, so it does not need to be installed either.
* A plain (uncompressed) .iso image of your own copy of Monster Hunter Freedom Unite,
  European (disc ID ULES01213) or North American (disc ID ULUS10391) release, or of Monster Hunter Portable 2nd G,
  the Japanese release (disc ID ULJM05500), version 1.01.
  CSO/ZSO compressed images are not supported. You can add several: choose which one to play in the Library.
* A controller is optional. Gamepads work out of the box (tested with a PS5 DualSense).
  Keyboard: arrows = D-pad, WASD = analog stick, K/L/J/I = cross/circle/square/triangle,
  Q/E = L/R, Enter = Start, Backspace = Select. Change them, and the controller's buttons, in Game settings -> Controls
  or from the in-game menu (Escape, Controls tab).
  Escape (or the controller's guide button) opens a menu over the game, which keeps running: volume, resolution,
  frame rate, fullscreen and quit. It works with the keyboard, a controller or the mouse.


How to play
-----------
1. Extract this folder anywhere you can write to, for example your home folder.
   The programs must keep their execute permission. If your file manager removed it:
       chmod +x MHForever mhfu mhfu-us mhfu-jp
2. Open MHForever.
3. Add your .iso. The launcher checks your copy and prepares what it needs from it.
   In Settings you can choose to keep a copy of the image in MHForever's own folder.
4. Press Play.

When a new version is out, the launcher says so: "Update now" downloads it, checks it and restarts.
Settings > Check for updates turns this off.

Your settings and library live in $XDG_DATA_HOME/MHForever (usually ~/.local/share/MHForever).
Your saves live in ~/.local/share/MHForever/saves (one folder per game), so updating MHForever never loses them.
Settings > Storage > Saves folder keeps them in another folder of your choice (your saves are copied there).
They use the same format as the PSP and PPSSPP: you can bring your own saves from a PSP memory stick or from
PPSSPP with Game settings > Saves > Choose folder (saves of your own version of the game only; the one you have now is
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
Early release. Freedom Unite runs at 30 fps, as on the original console. Game settings also
offers 60 to 180 fps (any multiple of 30): the game still runs at 30 and the extra pictures
are drawn in between, so it is experimental and heavy scenes may not hold the highest rates.
Online play (experimental): in the launcher, Settings > Online, pick a server. They are the PSP ad hoc
servers PPSSPP uses, so you meet other MHForever players and PPSSPP players in the game's own
Gathering Hall and hunt together. Offline is the default: then nothing is contacted. The server
shows your name and your game on its public status page; in their usual mode,
servers do not show your address to other players.
Private rooms: on the Home screen, "Create room" gives you a code (like K7QF-29XM) to send to your
friends, and "Join with code" takes theirs. Only players with the same code and the same server meet
in the Gathering Hall; "Back to public halls" brings you back to everyone. You can also switch, copy, type or
paste a code, leave a room and change the server in the game (Escape > Online).
Friends: in the launcher, Friends shows your friend code. Your friend presses "Let friends add me",
you type their code, they accept. You then see who is online and where, and can invite them or join
their room in one click, from the launcher or from the game's Online tab. Friends meet through public
Nostr relays: no account, and your friends never see your address (the relays do, as game servers do).
Escape opens the menu without pausing the game, so you do not drop out of a quest. It works with the
mouse, the keyboard or a controller.

Rooms and friends are new: tell us how they work for you on Discord, https://discord.gg/6xrxY2buDf


Source code and licenses
------------------------
MHForever is free software, GPL-3.0-or-later: see LICENSE.
Source code of MHForever and bug reports: https://github.com/Mango77x/MHForever
Third-party components and their licenses: THIRD_PARTY.md and the licenses folder.
