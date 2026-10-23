# aemu_postoffice (client side)

The clients of the two services a PSP ad hoc server offers, by Kethen: `adhocctl` (matchmaking, TCP 27312) and the
postoffice relay (TCP 27313, or UDP through ENet), as used by PPSSPP and by the aemu plugin for real PSPs.

- Origin: https://github.com/Kethen/aemu_postoffice, commit `51e8409e5eb7608681bcdb2aadcb79bd30eeec78` (2026-10-22).
- License: GNU GPL version 3 (`LICENSE`). MHForever is GPL-3.0-or-later, so binaries that include this code are
  distributed under the GPL version 3.
- Unmodified. Only what a desktop client needs was copied, in the original layout: `client/` (without the PSP-only
  files, the tests and the build scripts), `adhocctl/client/`, `adhocctl/packets_v1.h`, `adhocctl/pspstructs.h`,
  `aemu_postoffice_packets.h` and `design.md` (the protocol description).
- `ext/enet/enet.h` is ENet as a single header (zpl-c/enet v2.7.0, MIT license, notice inside the file), which the
  client includes for its UDP mode.

How MHForever uses it: `docs/08-multiplayer.md` section 1.2.
