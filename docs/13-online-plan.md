# 13 — Online play: research and the plan to finish it (2026-10-27)

This document gathers what was researched on 2026-10-27 to take online play from "the Gathering Hall works" to a release,
and on to the long-term vision of `08-multiplayer.md` (friends, rooms, every console on one network layer). It does not
replace `08-multiplayer.md`: that stays the design reference; this is the evidence and the work plan. Details that come
from the game's code (addresses, call arguments) are in the local `research/online-2026-10-27.md`, as `CLAUDE.md` asks.

Tags: **[verified]** checked against a primary source or measured on this date; **[to test]** expected from the
evidence but not yet seen working.

## 1. Where online play stands

- Branch `net/aemu-client` (not merged; nine commits of its own, on a `main` from before 0.5.0): the vendored aemu_postoffice
  client, the ad hoc control library and the ad hoc sockets over a PSP ad hoc server, the launcher setting for the
  server, traces and live tests (`05-runtime-hle.md` §5i on that branch).
- Works [verified 2026-10-24]: two copies of our game, and our game with PPSSPP 1.20.4, see each other in the Gathering
  Hall through a local reference server.
- Does not work: **starting a quest together freezes the host** (§3).
- Not tried: a public server, more than two players, Linux, a real PSP, strict networks.
- The branch predates the per-copy settings and the North American release (0.5.0); rebasing it is the first task
  (§8).

## 2. What Freedom Unite needs from the network [verified 2026-10-27, from the European executable]

- The game has one small socket layer. Each operation (open, connect, listen, accept, send, receive, close) goes either
  to the PSP's ad hoc stack or to the infrastructure (BSD/TCP) stack, chosen by a mode flag; online play uses the ad hoc
  side. The infrastructure side (`sceNetInet*`) is not needed for multiplayer.
- **Gathering Hall:** PDP datagrams, all broadcast to the group, on port 10000 (`05-runtime-hle.md` §5i on the branch).
- **Quests: PTP streams.** The game opens, listens on, connects and accepts PTP sockets (8 KiB buffers, 250 ms
  retransmit interval, 100 retries, a backlog of 3). Live public servers show Freedom Unite players with PDP ports 20000
  and 30001 and PTP port 30001, i.e. the game's ports 10000 and 20001 plus PPSSPP's offset of 10000 [verified on public
  status pages].
- **How each call is made:** connect, accept, PTP receive and PDP receive are non-blocking (the game polls them every
  frame and treats "would block" as "not yet"). **PTP send is the only blocking call, with no timeout**, and PDP send
  is blocking but cannot normally block. So the one place where a network problem can stop the game outright is PTP
  send.
- The game joins its group through the network configuration dialog and does not use GameMode, Matching or Discover.
- It sends its product code only to the control library (`sceNetAdhocctlInit`), which passes it to the server.
- Group (room) names are built by the game: `MHP2Q0` or `MHP2A0` followed by two digits, e.g. `MHP2Q011`. Both strings
  are identical in the European and North American executables.

## 3. The quest-start freeze

The host's last log line (`[interp] interpreting …`) only says that new code started running when the quest began (a
per-frame network state machine called through a table of function pointers, which the static analysis cannot see). It
does not show where the host stopped. Ranked causes, each with the test that decides it:

1. **The host blocks in PTP send.** Our `sceNetAdhocPtpSend` waits with no timeout, as the game asks, but it also waits
   while our socket is still pairing with the relay or the relay reports "would block". If the pairing never finishes, the
   calling thread never returns; our trace prints after the call, so the log stays silent, which is what was seen.
   PPSSPP also waits in that case, so the real question is why the pairing does not finish for us.
2. **MAC mismatch on the first unicast.** Everything in the Gathering Hall is broadcast; the quest is the first time
   the game addresses a peer by MAC. PPSSPP compares MAC addresses ignoring the first byte and replaces the game's MAC
   with the peer's real one before it talks to the relay, because some games alter that byte (`isMacMatch` and
   `fixGameMac` in PPSSPP's `proAdhoc.cpp`; the aemu server says the same in its login check). We do neither. Whether
   Freedom Unite alters the byte is not known yet; the entry trace below shows it.
3. **Port or timing.** Both ends must add the same offset to PTP ports, and the listener must exist when the connect
   arrives; the server tolerates a listener that comes a little late since its 2026-03-19 release.
4. **Another thread waits on the network thread** (a semaphore or event flag).

What to build first (all in the runtime, no guesswork): a trace on the *entry* of every ad hoc call with its arguments;
a watchdog that dumps the guest threads (`Runtime::dump_threads`) when no vblank has passed for 3 s; and the server's
live session list (`/data.json`) captured at the same moment. One reproduction with these answers 1–4. Then:
- normalise MACs the way PPSSPP does (first byte ignored, peer's real MAC used on the wire);
- keep a blocking send waiting, as the PSP does, but log it after a second and fail it as disconnected if the socket
  closes or the pairing fails;
- add the state-machine function to the recompiled set if the interpreter turns out to be too slow there (it is called
  every frame during a quest).

## 4. Compatibility between versions

| Who plays with whom | Status | Notes |
|---|---|---|
| Our EU ↔ our EU, our NA ↔ our NA | Hall [verified 2026-10-24 for EU]; quest blocked by §3 | |
| **Our EU ↔ our NA** | [to test] expected to work | Same code (the US build is the European one shifted), same group names; servers merge the two product codes into one game (PPSSPP's server lists the pair, and so does aemu's `game_db.json`) |
| Ours ↔ PPSSPP (FU, any region) | Hall [verified 2026-10-24]; quests [to test] | PPSSPP must use the relay; for a server outside its list, "relay" must be forced on |
| Ours ↔ real PSP or Vita (aemu plugin) | [to test] | Same protocols; the plugin mirrors PPSSPP's port offset (`port_offset.txt`) |
| **FU (EU/NA) ↔ Portable 2nd G (JP)** | Do not meet | The server merges the product codes, but the Japanese game names its rooms in Japanese (`集会所01`, "Gathering Hall 01", seen live on a public server on 2026-10-27) while FU uses `MHP2Q0…`. Different group names, so different rooms. See §6.3 |
| Two versions of MHForever | Compatible by construction today | The game's traffic is the game's own and the server protocol is aemu's; nothing of ours is on the wire yet. Our own protocol (§6) must carry a version from its first release |

Notes:
- **Why real PSPs of different regions never met** (players' reports from 2009–2011): a PSP's ad hoc network name
  contains the product code, so EU and US consoles could not see each other. Tunnels that ignore it (Ad-Hoc Party,
  XLink Kai) let them play. It was the network name, not the game, that kept them apart [explanation consistent with
  the servers' crosslinks; not verified on hardware].
- **Port offset:** keep PPSSPP's default of 10000. Players on another offset (one public room used 8800 on
  2026-10-27) cannot meet players on 10000, so the launcher should offer the setting for communities that use another
  value.
- Characters, items and quests are the same data in every region (the Japanese release shows the same items under other
  names).

## 5. What the community and a sibling project teach [verified 2026-10-27]

**aemu_postoffice** (Kethen; the client we vendored):
- Since 2026-10-14 the server and client also offer UDP (ENet) for both services, to avoid TCP connection problems;
  PPSSPP merged UDP support on 2026-10-21 (PR 22549). Our vendored client already contains the ENet path.
- Since 2026-10-26 every file carries `SPDX-License-Identifier: GPL-3.0-only`, plus contributor rules: changes must keep
  the license, a link to the repository and a copy of the GPLv3, and **pull requests must not be co-authored by an AI
  service**. Our vendored copy already keeps the license and the link; the next update brings the SPDX lines. Any change
  we send upstream must follow those rules (as this project already does).
- Server defaults that matter to us: `relay_only` on (players' IP addresses are not given to other players), at most 20
  relay sessions per MAC and 16 control sessions per IP, a client dropped after 30 s of silence.
- What the server checks at login: the MAC must not be `xx:00:00:00:00:00` (it compares only the last five bytes,
  "because some games corrupt the first byte"), the product code must be nine capital letters or digits, the nickname
  shorter than 128 bytes. Group names are not checked: any name up to 8 bytes without a zero byte is taken as it is.
- The status page (`/data.json`) shows, for every game, the rooms, each player's nickname and their PDP/PTP ports. Not
  the MAC, not the IP.

**PPSSPP's public server list** (`metadata.ppsspp.org/adhoc-servers.json`): 22 servers on 2026-10-27, all relay
servers; servers are added by pull request to PPSSPP. Live Freedom Unite rooms that day: EA Nation Hub (France), Games
Nexus (Italy) and Neko PSP (Japan, Japanese release). Asking PPSSPP before reading this list stays a release condition
(`08-multiplayer.md` §1.2).

**Yakumo** (TeamGDB, MIT, static recompilation of Portable 3rd; quests between two instances work through a PSP ad hoc
server):
- **The game must never pause during ad hoc play.** A paused game stops answering and the others drop it from the
  quest. Their menu keeps the game running in multiplayer (an option can restore pausing). Our Escape menu pauses the
  game today; this must change before 0.6 (`07-launcher-ux.md` §3c already lists it).
- They wrote their own client of the two protocols (MIT) instead of vendoring aemu's: TCP_NODELAY, keepalive, a
  reconnect that rejoins the group within 10 s, stall and request timeouts, and a network thread that keeps the server
  connection alive while the menu is open.
- They also host from the game (a built-in server for LAN or VPN play).
- PSP timeouts last real time ("emulated time is held to real time"); fast-forward is single player only.

**Other facts:**
- PPSSPP has no network workaround specific to Freedom Unite.
- IPv6 is not a way around carrier-grade NAT in Spain for most players: about 11 % of Spanish users are IPv6-capable
  (APNIC, 30-day average to 2026-10-24), flat for two years. `08-multiplayer.md` §4.3 overstated it; the relay stays
  the main path and direct links are an optimisation.
- Library versions to use when the time comes: libjuice 1.7.4 (2026-09-26), libsodium 1.0.22 or Monocypher 4.0.3,
  miniupnpc 2.3.x, ENet (zpl-c) 2.7.0; `jech/dht` has no releases (last change 2026-09-04).

## 6. Design decisions this research proposes

### 6.1 The relay stays the transport; UDP as an option
Players connect out to a community server; nothing to open on the router. Offer the server's UDP mode as an option
("try UDP") once the TCP path is solid, since the client already has it.

### 6.2 Private rooms on public servers, by translating the group name
The game always picks names like `MHP2Q011`, so two strangers who pick the same room number land together. Our control
library can translate the name the game asks for into another 8-byte name on the server (derived from a room code), and
report the game's own name back to the game. Effects:
- A room code gives a private room on any aemu server, with no server of our own and nothing for the game to know.
- Friends can join with the code; strangers, PPSSPP and PSP players cannot, so public rooms stay the default and the
  private ones are opt-in.
- The server takes any group name up to 8 bytes without a zero byte [verified in its source]; a readable code (letters
  and digits) keeps the status page tidy.
This is the "room = group on server" of `08-multiplayer.md` §1.2, made concrete. [to test with a real server]

### 6.3 A bridge to the Japanese release (later)
The same translation can map `集会所NN` and `MHP2Q0NN` to one server group, so the Japanese and the international
releases meet. Open questions before promising it: the Japanese name that corresponds to `MHP2A0`; whether the game data
the two exchange is the same (the reports say yes); and names and chat, which the Japanese release writes in Shift-JIS.
Needs a Japanese copy to test, so it waits for the Japanese region.

### 6.4 Our own protocol carries a version from the first byte
Anything MHForever adds to the wire (friend links, presence, invites, room negotiation) starts with a magic, a protocol
version and a capability list, and refuses what it does not understand with a clear message. Rooms carry game id,
region, build and mode (`08-multiplayer.md` §1.1).

### 6.5 Identity and the MAC
The launcher already derives a stable, locally administered MAC from the player's identity; it stays the same for
every region's program, so a player is the same person whichever disc they play.

### 6.6 What stays from `08-multiplayer.md` §4, corrected
Friends, presence and invites over the Mainline DHT, links authenticated by Ed25519 keys, and a pairwise secret
rendezvous stay the target for 0.7. Direct links (IPv6, UPnP/PCP, hole punching) become 0.8 or later, an optimisation for
latency and not a requirement, given §5's IPv6 figures and the 2025 measurements of about 70 % hole-punching success.

## 7. Decisive experiments

| # | Experiment | Decides | Status |
|---|---|---|---|
| E1 | Two local instances start a quest, with entry traces, the watchdog dump and `/data.json` | the cause of §3 | Done 2026-10-27: two fixes (interpreter jump into recompiled code, PtpOpen arguments) |
| E2 | Our EU and our NA in the same Gathering Hall, then a quest | EU ↔ NA | To do |
| E3 | Ours and PPSSPP (US ISO) complete a quest; then PPSSPP hosts | interop in both roles | Half done 2026-10-27 on socom.cc: PPSSPP created the room, ours led the quest (listened and accepted), the quest played. Ours joining a quest led by PPSSPP (ours connects) still to test |
| E4 | Three and four players (two PCs, or three instances) | the PTP backlog of 3, relay limits | To do |
| E5 | A public server in Europe (Games Nexus or EA Nation Hub), from two networks including mobile data | real latency, strict NATs | To do |
| E6 | Escape menu open for 30 s during a quest | the no-pause rule | To do |
| E7 | Private room: a translated group name on the reference server, then on a public one | §6.2 | To do |
| E8 | Linux (WSL first, then a real machine) | parity | To do |
| E9 | Fuzz the ad hoc HLE entry points and the control-library parser | `08-multiplayer.md` §4.6 | To do |

## 8. Work plan

### 0.6 "Hunt together" (online on community servers)
1. Rebase `net/aemu-client` onto `main`. The server setting belongs to the launcher's own Settings (it is the same for
   every game), not to a copy's Game settings.
2. Diagnostics: entry traces, the vblank watchdog with thread dump, a script that captures `/data.json`.
3. E1, then fix the freeze (§3), including PPSSPP's MAC normalisation.
4. Never pause during ad hoc (E6).
5. E2, E3, E4: a full quest in every pairing above.
6. Update the vendored aemu client (SPDX headers, latest fixes).
7. Launcher: server list (after PPSSPP agrees), ping to each, the number of Freedom Unite rooms from each status page,
   the port offset setting, and a privacy note (the server shows your nickname and the game).
8. E5 and E8; then E9.
9. Messages to PPSSPP and the main server operators; credits in the README and on the website.
Acceptance: two players on different networks, one on our Windows build and one on PPSSPP or our Linux build, complete
a quest on a public server, and a third and a fourth join.

### 0.7 "Friends and private rooms"
Identity backup and friend codes (the launcher already has the identity), presence and invites (DHT, authenticated
links), room codes as private rooms on public servers (§6.2), our protocol with its version (§6.4).
Acceptance: a friend added by code sees "in a room" and joins it in one click; a stranger on the same server cannot.

### 0.8 and later
Direct links for lower latency (IPv6, port mapping, hole punching, relay through the group); a built-in server for LAN
and VPN play (the aemu server is GPL-3.0, so it can be bundled); the Japanese bridge (§6.3) once the Japanese region
exists; the same room service for the next console (`08-multiplayer.md` §1.1).

## 9. Questions for the owner

1. Private rooms are invisible to PPSSPP and PSP players by design (§6.2). Fine as an option next to public rooms?
2. Who writes to PPSSPP and the server operators, and when (before 0.6 ships)?
3. Keep the vendored GPL-3.0-only aemu client, or write our own client as Yakumo did? Keeping it is less work and is
   what PPSSPP uses; our own would give us full control of reconnects and timeouts.
4. Bundle a LAN server in a later version?

## 10. Sources (2026-10-27)

- The European executable of Freedom Unite (calls and their arguments; see `research/online-2026-10-27.md`).
- PPSSPP source, `Core/HLE/proAdhoc.cpp`, `proAdhocServer.cpp` (crosslinks), `sceNetAdhoc.cpp` (PTP over the relay,
  blocking waits), `assets/compat.ini`; PR 22549 (UDP mode); wiki "How to play multiplayer games with PPSSPP".
- Kethen/aemu_postoffice: releases 2026-03-19 to 2026-10-14, commit of 2026-10-26 (licence headers, contribution rules),
  `server_cpp/config.h`, `adhocctl/server_cpp/server.cpp`, `design.md`, `hosting.md`; Kethen/aemu `installation.md`.
- `metadata.ppsspp.org/adhoc-servers.json` and the status pages of the listed servers (live rooms, 2026-10-27).
- TeamGDB/Yakumo (MIT): README, `profiles/mhp3rd/README.md`, `host/hle/hle_adhoc.cpp`, `host/adhoc/client.cpp`.
- tclamb/mhp2g-decomp (CC0): network headers and symbol lists.
- APNIC Labs IPv6 measurements for Spain (`data1.labs.apnic.net/v6stats/v6economy/ES.json`).
- GameFAQs answers on cross-region ad hoc play (2009–2011), for players' reports only.
