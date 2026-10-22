MHForever @VERSION@
An unofficial Monster Hunter recompilation project
===================================================

MHForever lets you play Monster Hunter Freedom Unite (PSP) natively on your PC.
It is NOT affiliated with, endorsed by or sponsored by Capcom or Sony.
It contains none of the game's assets (no disc image, graphics, audio or text): you need your own copy
of the game.


What you need
-------------
* Windows 10 or 11, 64-bit.
* A plain (uncompressed) .iso image of your own copy of Monster Hunter Freedom Unite,
  European release (disc ID ULES01213, version 1.01). CSO/ZSO compressed images are not supported.
* A controller is optional. Gamepads work out of the box (tested with a PS5 DualSense).
  Keyboard: arrows = D-pad, WASD = analog stick, K/L/J/I = cross/circle/square/triangle,
  Q/E = L/R, Enter = Start, Backspace = Select.
  Escape (or the controller's guide button) pauses the game and opens a menu: volume, resolution, frame rate,
  fullscreen and quit. It works with the keyboard, a controller or the mouse.


How to play
-----------
1. Unzip this folder anywhere you can write to (not "Program Files").
2. Open MHForever.exe.
3. Add your .iso. The launcher checks your copy and prepares what it needs from it.
   In Settings you can choose to keep a copy of the image in MHForever's own folder.
4. Press Play.

Your saves are stored in Documents\MHForever\saves (one folder per game), so updating MHForever never loses them.
If you played an older version, the "memstick" folder next to its MHForever.exe is copied there
the first time you press Play (copy it next to the new MHForever.exe if you unzipped a new folder).
Prepared game data, your settings and your library are stored in %APPDATA%\MHForever. Removing
a game from the launcher deletes only its prepared data (never your original .iso or your saves).


Windows may warn you
--------------------
MHForever.exe and mhfu.exe are not digitally signed. Windows SmartScreen can show
"Windows protected your PC": choose "More info" and then "Run anyway". The launcher
also needs the Microsoft Edge WebView2 runtime, which Windows 10/11 normally includes.


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
