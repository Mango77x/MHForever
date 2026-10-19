# MHForever

**An unofficial Monster Hunter recompilation project.**

Play **Monster Hunter Freedom Unite** natively on your PC: no emulator, any controller, a launcher that sets everything
up from your own disc image.

> Not affiliated with, endorsed by or sponsored by Capcom or Sony. Requires your own legitimate copy of the game.
> No game content is included or downloaded. "Monster Hunter" is a trademark of its owner and is used here only to
> describe which games the project works with.

## Where the project is today

| | |
|---|---|
| **Playable game** | **Monster Hunter Freedom Unite** (PSP), European release (`ULES01213`, v1.01). It is the only game for now. |
| **Frame rate** | **30 fps**, as on the original console. **60 fps is being worked on.** |
| **Online play** | Not available yet. **Multiplayer over the internet is being designed and built**, so you can hunt with friends without a PSP. |
| **Platforms** | Windows (64-bit). Linux and Steam Deck are planned. |
| **Controllers** | Gamepads work out of the box. **Tested with a PS5 DualSense**; other pads recognized by SDL should work too. Keyboard defaults included. |
| **Resolution** | Up to several times the original 480x272, with an optional smoother texture filter. |
| **Other games and regions** | The US and Japanese releases of Freedom Unite, and other games of the series, are planned. |
| **First public release** | In preparation. Watch the [Releases](../../releases) page. |

It is early software: the game boots and can be played, but expect rough edges and bugs. Reports are welcome.

## How it works for a player

1. Download MHForever from the Releases page (when the first release is out) and unzip it anywhere.
2. Open `launcher.exe` and add **your own** `.iso` of Freedom Unite (a plain, uncompressed image of your UMD).
3. The launcher checks your copy and prepares everything it needs from it on your PC. You can also let it keep a copy
   of your image in its own folder.
4. Press **Play**.

Nothing from the game is ever included in MHForever or sent anywhere: everything is read from the image you provide.
Your saves live next to the launcher. Any controller recognized by SDL should work (tested with a PS5 DualSense); there are keyboard defaults too.

**Keyboard:** arrows = D-pad, WASD = analog stick, K/L/J/I = cross/circle/square/triangle, Q/E = L/R, Enter = Start,
Backspace = Select.

## What this is

MHForever is not an emulator. The game's program is **statically recompiled** into native PC code, and the console's
system services are re-implemented. The result is a normal Windows program that runs the game directly, with a modern
launcher around it. That is what makes things like higher resolutions, any controller and, later, online play practical.

Developed with AI assistance.

## For developers and contributors

Build instructions are in [BUILDING.md](BUILDING.md), the rules for contributions in [CONTRIBUTING.md](CONTRIBUTING.md).

| | |
|---|---|
| [Overview](docs/00-overview.md) | [Architecture](docs/02-architecture.md) |
| [PSP platform](docs/03-psp-platform.md) | [Recompiler](docs/04-recompiler.md) |
| [Runtime / HLE](docs/05-runtime-hle.md) | [Freedom Unite module](docs/06-mhfu-module.md) |
| [Launcher, controllers, Steam Deck](docs/07-launcher-ux.md) | [Multiplayer](docs/08-multiplayer.md) |
| [Roadmap](docs/10-roadmap.md) | |

## License

GPL-3.0-or-later (see `LICENSE`). Third-party components keep their own licenses (see `THIRD_PARTY.md`).
