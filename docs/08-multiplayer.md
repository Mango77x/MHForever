# 08 — Online play

Freedom Unite plays online since 0.6.0 (experimental): players pick one of the PSP community's ad hoc servers in the
launcher and meet in the game's own Gathering Hall, with other copies of MHForever and with PPSSPP players, and go on
quests together. Private rooms (a code that gives friends halls of their own) and friends (added by code, who is
online, invites, joining) came in 0.8.0 (`14-rooms-and-friends.md`). Freedom (and Freedom DX) plays online too, the same
way (§1.4).

The runtime side, call by call, is in `05-runtime-hle.md` §5i; the launcher's screens are in `07-launcher-ux.md`.

## 1. How it works

### 1.1 Every platform, one network layer

Freedom Unite is the first game, not the only one: later titles may come from the 3DS, Switch, PS2, Wii or Wii U, each
with its own kind of multiplayer. What can be shared is written once; each console adds an adapter, and nothing below
`core/net` knows which console or game is running.

```
launcher            identity, friends, presence, invites, rooms                    — shared by every game
   │
core/net            rooms: N members with virtual addresses (room.hpp)            — shared by every console
   │                  · packets: members exchange datagrams/streams (the game's own netcode)
   │                  · inputs:  members exchange controller inputs per frame (netplay, later)
   ▼
platforms/<console>  adapter: the console's network API
                     psp: sceNetAdhoc/Adhocctl over the community servers (§1.2)
games/<id>/          data only: max players, hall names, compatible releases
```

Today the PSP adapter talks to the community servers directly (§1.2), so `core/net` holds the room interface and an
in-process network for tests (`local_network.*`). Other consoles' kinds of multiplayer map onto rooms as follows:

| Kind | Examples | How |
|---|---|---|
| Local wireless | PSP ad hoc, DS/3DS, Switch local | the room is a virtual LAN; the game sees nearby consoles |
| LAN / system link | PS2 network adapter, Wii U LAN | the same virtual LAN |
| Official online services | DS/Wii Nintendo WFC, 3DS/Wii U online, PS2 online games | per game: many used the service only for matchmaking and then played peer to peer, which a room can replace |
| Several controllers on one console | Wii, PS2 split screen | inputs synchronised between PCs (lockstep, later rollback); needs deterministic emulation |

### 1.2 The transport: the community's ad hoc servers

MHForever plays on the servers the PSP community already runs for PPSSPP and real PSPs (aemu), not on servers of its
own and not peer to peer:
- **It works on every network.** Each player only connects out to the server over TCP, so CG-NAT, mobile data and
  strict NATs are no obstacle and nobody opens ports.
- **No cost.** The servers are run by volunteers and listed by PPSSPP.
- **Players from day one**, PPSSPP and real PSP players included.

Two services on the same host:

| Service | Port | What goes through it |
|---|---|---|
| adhocctl (control) | TCP 27312 (`host:port` overrides it) | login (virtual MAC, nickname, product code), groups (the game's halls), scans, peer lists |
| aemu postoffice (relay) | TCP 27313, the next port up | every datagram (PDP, up to 10 KiB) and stream (PTP, up to 50 KiB) between players |

The client is aemu_postoffice's, vendored unmodified (`third_party/aemu_postoffice`, GPL-3.0, the one PPSSPP uses). On
top of it the runtime implements `sceNetAdhocctl` (`adhocctl.*`, `adhoc_link_aemu.*`), `sceNetAdhoc` PDP and PTP
(`adhoc_sockets.*`, `hle_net_sockets.cpp`) and a `sceNetAdhocDiscover` stub. Ports go on the wire plus 10000, as in
PPSSPP, so both meet.

What the launcher gives the game when a server is chosen (Settings → Online): `--adhoc-server`, a virtual MAC that is the
same on every launch (`02:` and five bytes of `SHA-256("mhf-adhoc-mac" + identity seed)`, a locally administered address
that tells nothing about the computer), the player's name as the nickname, the private room's code if any
(`--adhoc-room`), and the two friends files of `14-rooms-and-friends.md` §4.3. Nothing is contacted before the game first
uses its ad hoc libraries.

### 1.3 What Freedom Unite does on the network [verified 2026-10-27, European executable]

- **Gathering Hall:** PDP datagrams, all broadcast to the group, on port 10000. A hunter's position and name go out every
  40 vblanks (667 ms), two other kinds every 16 vblanks, so a remote hunter moves in steps, in PPSSPP too
  (`05-runtime-hle.md` §5i).
- **Quests:** PTP streams on ports 20001–20004 (8 KiB buffers, 250 ms retransmit interval, 100 retries, a backlog of 3).
  The game opens, listens, connects and accepts them; which side connects is the game's choice, not the quest leader's.
- Connect, accept and both receives are polled every frame. **PTP send is the only blocking call, with no timeout**, so
  it is the one place where a network problem can stop the game; the runtime waits the way the PSP does, in 1 ms steps,
  with the other threads running.
- The game joins its hall through the network configuration dialog (`sceUtilityNetconf`), which the runtime fails after
  15 s if the server never confirms the group. It does not use GameMode, Matching or Discover.
- Hall names are the game's: `MHP2Q0` or `MHP2A0` and two digits (`MHP2Q000`–`MHP2Q023`), the same in the European and
  North American executables; the Japanese release uses `集会所` and two digits (`14-rooms-and-friends.md` §1).
- The game must never pause while online: a paused game stops answering and the others drop it from the quest. The
  Escape menu does not pause the game (since 0.6.0), as a PSP had no pause either.

### 1.4 What Freedom does on the network [measured 2026-11-24, all three releases]

The same calls as Freedom Unite, from the same engine, so the same runtime serves it and nothing of it is mapped per
release. What differs:
- **Halls:** `MHPSP0` and two digits (`MHPSP000`), the same in the European, North American and Japanese executables, so
  all three releases meet in the same halls (the servers crosslink `ULUS10084`, `ULES00318` and `ULJM05066`).
- **Gathering Hall:** PDP datagrams on port 10000, of 90, 110, 74 and 24 bytes (and one of 338 when a player comes in).
  The names of everyone in the hall show in the list, but **a hunter's body shows only to those in the same quest party**:
  players see each other once they have joined the same quest. The game works so; it is not a fault of the network.
- **Quests:** PTP streams on port 20001 and up, as Freedom Unite's, plus PDP datagrams on the quest's own port.
- The village's Online Gathering Hall opens after the first talk with the village elder.

## 2. Servers

- **The list.** Settings → Online offers Offline, five servers from PPSSPP's published list
  (`metadata.ppsspp.org/adhoc-servers.json`) with their ping, or any address: Socom (socom.cc, France, the busiest),
  Madness Gaming Network (psp.mgn.pub, United States, a Monster Hunter community), Games Nexus (adhoc.gamesnexus.ovh,
  Italy), Psi-Hate (psi-hate.com, United States) and Neko PSP (cat.wjg.jp, Japan). The list is built into the launcher
  (`launcher/ui/src/servers.ts`); it does not download PPSSPP's.
- **Crosslinks.** A server groups players by product code, and "crosslinks" the releases of one game: `ULES01213`,
  `ULUS10391` and `ULJM05500` count as one game (PPSSPP's server and aemu's `game_db.json` list them), so a scan returns
  the halls of all three. The game itself keeps only the names it knows (§3).
- **What the server checks at login:** the MAC must not be `xx:00:00:00:00:00` (it compares only the last five bytes,
  "because some games corrupt the first byte"), the product code must be nine capital letters or digits, the nickname
  shorter than 128 bytes. Group names are not checked.
- **Defaults that matter:** `relay_only` (players' IP addresses are not given to other players), at most 20 relay
  sessions per MAC and 16 control sessions per IP, a client dropped after 30 s of silence.
- **Port offset:** fixed at PPSSPP's default of 10000. Players on another offset (some communities use 8800) do not meet
  MHForever players; there is no setting for it.
- **Status pages** (`data.json`, `status.xml`) show every game's halls with each player's nickname and ports, not their
  MAC or IP address. The launcher says so next to the server choice.

**Test a connection.** In Settings → Online, “Test this server” opens TCP connections to the selected server's control
and relay ports. The results show which ports answered. A successful connection does not verify the relay protocol or
quest traffic. “Compare community servers” contacts each listed server's control port. Both checks are manual.
For an existing aemu server on a LAN or VPN, enter its reachable hostname or IPv4 address. “Test before saving” checks
that address while keeping the current server or Offline active. Choose “Use this server” to apply the address.

**Share a private room.** On Home, create a room and choose “Copy invite”. Send the plain text privately. The other
player can paste it on Home and review the server and host edition, even from Offline. If the selected copy uses
different halls, the launcher offers a compatible installed copy or the option to add one. Editions that share halls
but may have different quests can still join. Choose “Join room” to confirm. This selects the server and
room together. Launch compatible copies of the same game and enter the same Gathering Hall. A room code alone still
works when both players have already selected the same server. Older three-field clipboard invites also remain readable.
To share with a launcher that only understands the old format, send the server and use “Copy code”.

**Temporary send congestion.** The PSP control worker retries an unsent command on the same TCP connection before
sending newer commands. A lost control session still reports a disconnect to the game, which can request a new hall.

## 3. Who plays with whom

| Players | Status | Notes |
|---|---|---|
| MHForever ↔ MHForever, same release | Works: quests (two instances, 2026-10-27; players since 0.6.0) | |
| MHForever EU ↔ MHForever NA | Expected to work, not measured | Same code and hall names; the server crosslinks the two product codes |
| MHForever ↔ PPSSPP (FU, EU or NA) | Works: quests with either side leading (socom.cc, 2026-10-27) | PPSSPP 1.20 or later, with the server's relay: PPSSPP uses it by itself for servers in its list, and needs "Try to use the packet relay of the server" set to always on for any other address |
| MHForever ↔ real PSP or Vita (aemu plugin) | Expected to work, not tested | Same protocols; the plugin uses the same port offset |
| FU (EU/NA) ↔ Portable 2nd G (JP) | Do not meet | The server crosslinks them, but the Japanese game's halls are `集会所NN` and FU's `MHP2Q0NN`: neither game lists the other's |
| FUComplete ↔ the plain Japanese release | Meet in the same halls | Both are `ULJM05500`; FUComplete adds quests and events, so play with the same edition |
| Two versions of MHForever | Compatible | Nothing of MHForever's own is on the wire: the traffic is the game's and the protocol aemu's |
| Freedom, EU ↔ NA ↔ JP, Freedom DX included | Works: hall and quests (one of each in a quest of three, 2026-11-24) | Same hall names, crosslinked product codes. Freedom DX and the plain release meet; play with the same edition, since its options change the game |
| Freedom ↔ Freedom Unite | Do not meet | Two games: the servers do not crosslink them, and their halls differ |
| Freedom ↔ PPSSPP | Expected to work, not tested | Same protocol and port offset as Freedom Unite's |

Why real PSPs of different regions never met in 2009–2011 (players' reports): a PSP's ad hoc network name contains the
product code, so EU and US consoles did not see each other; tunnels that ignored it (Ad-Hoc Party, XLink Kai) let them
play. The servers' crosslinks do the same.

## 4. Identity, rooms, friends and safety

### 4.1 Identity and friend codes
- The launcher makes a secret **identity seed** on first run and offers a backup file; nothing else identifies the
  player. The virtual MAC (§1.2) and the Nostr key of the friends network (`HKDF-SHA256(seed, "mhf-nostr")`) both come
  from it, so the backup restores both.
- The **friend code** is the first 80 bits of `SHA-256(Nostr public key)`, 16 base32 characters (`XXXX-XXXX-XXXX-XXXX`).
  Finding another key with the same code would take about 2^80 tries; adding a friend checks that the key behind the
  code hashes to it (`14-rooms-and-friends.md` §4.3).
- The **name** is the player's pseudonym, sent to the server as the nickname and to friends with presence.

### 4.2 Private rooms
A room code (`K7QF-29XM`, 8 Crockford base32 characters) renames every hall the game asks for, so only players with the
same code see and join those halls, on any public server, with nothing for the operators to do. Only MHForever players
can enter one; the public halls stay the default and a switch goes back to them at any time. Details:
`14-rooms-and-friends.md` §3.

### 4.3 Friends
Friends find each other over public Nostr relays (WebSocket, outgoing only): presence ("playing Freedom Unite on
socom.cc, room open"), invites and joining in one click, without accounts or servers of MHForever's own, and without
showing anyone's IP address to friends. Details: `14-rooms-and-friends.md` §4.

### 4.4 In the launcher and in the game
- **Launcher:** Settings → Online (server, name, privacy note); Home's "Play with friends" card (create or join a room,
  copy its code, back to the public halls); the Friends drawer (`07-launcher-ux.md`).
- **Game:** the Escape menu's Online tab, shown when a server is set: public halls or private room, friends and their
  invites, the current room and who is in it, and the server. Room and server change while playing, taking effect only
  when that cuts nobody off (`14-rooms-and-friends.md` §3.4). A notice says when friends come into the player's room.

### 4.5 Privacy
- Settings → Online → “Share my server and room with friends” controls automatic location sharing for both games.
  Turning it off sends empty server and room fields in new presence updates. Friends still see the game being played.
  Explicit invitations still carry the destination. Previously shared codes remain usable; create a new room to change codes.
- The ad hoc server sees the player's IP address, nickname, product code and when they play; its status page shows the
  nickname to anyone. In `relay_only` mode other players never see the IP address.
- The Nostr relays see the player's IP address and when the launcher is open, and random-looking keys; not who is friends
  with whom, and nothing they can read (`14-rooms-and-friends.md` §5).
- Room codes are secrets: never logged, never put in a URL.

### 4.6 Security model
1. **The game's own network code is the largest risk.** Freedom Unite parses peers' packets with 2008 code never meant
   for hostile input. Containment: recompiled code reaches guest memory only through bounds-checked accessors, and
   indirect jumps only reach recompiled functions or the interpreter, so a game exploit stays inside the virtual PSP.
   The walls are the HLE boundaries:
   - File paths: a component with a drive or root name (`C:`, `\\?\`, `file:stream`) is refused, so nothing escapes the
     memory stick folder (`resolve_under_root`, tested in `test_hle_io`). `host0:` is closed unless a developer tool sets
     a folder.
   - The GE reaches memory only through bounds-checked lookups (outside RAM reads give 0, writes are dropped).
   - HLE functions that take guest pointers go through `GuestMemory`, which throws outside guest memory; the kernel stops
     the program with a message, so a bad pointer from the network ends the game cleanly. The net HLE caps every size by
     the protocol's limits (PDP 10 KiB, PTP 50 KiB).
   - Fuzzing the ad hoc entry points and the control library's parser with malformed input is still to do (§7).
2. **IP addresses.** Nobody sees another player's address: the ad hoc servers relay everything and the friends network
   goes through Nostr relays.
3. **Launcher network code** (Nostr events): every event is checked before use (signature, size, age, rate, fields), as
   `14-rooms-and-friends.md` §4.3 lists.
4. **Updates** are checked against the SHA-256 GitHub publishes for each archive; signed updates are still to do
   (`07-launcher-ux.md`).
5. Cheating among players is out of scope, as on the PSP.

Target: at least as safe as ad hoc play in PPSSPP, where peers' packets also reach the emulated game.

## 5. When it does not work

| Symptom | Likely causes |
|---|---|
| "Communication error" before the Gathering Hall | No server set for that game program, the server is down or unreachable (TCP 27312 blocked) |
| No halls, or nobody in them | Players on different servers, one of them in a private room (or with another code), or a different release that does not share halls (§3) |
| Players see each other in the hall, but a quest stays on the loading screen | The quest's streams go through the relay (TCP 27313): blocked by a firewall, antivirus or VPN, a server without the relay, PPSSPP without the relay, or a very distant server; or two editions with different quests (FUComplete and the plain release); or both players with the same virtual MAC because one copied the other's `launcher.json` |
| A remote hunter moves in steps | The game's own rate (§1.3), not a network problem |

For a report: `game.log` from every player, the server, each player's release, and whether anyone is on PPSSPP.
`MHF_TRACE_ADHOC=1` logs the ad hoc calls (those that fail, and those that keep waiting) and, once a second, how many
datagrams were sent and read; `=2` also dumps the start of every datagram. `BUILDING.md` ("Trying online play") runs a local server.

## 6. Tests and results

- `psp_adhocctl` (no network): the control library against a fake server.
- `psp_adhocctl_server`: two players against a real server (`MHF_ADHOC_TEST_SERVER`), with groups, datagrams, a stream,
  a private room, and optionally a 45 s keepalive and a jitter run; skipped without a server.
- `launcher/ui/tools/friends-live-check.ts`: two identities on the default Nostr relays add each other and exchange
  presence and an invite.

| Date | What | Result |
|---|---|---|
| 2026-10-23 | `psp_adhocctl_server` against the reference server | passes (3 of 3 runs) |
| 2026-10-24 | 300 datagrams at 30 per second through the relay | 300 of 300, median delay 16 ms, max 23 ms |
| 2026-10-24 | two copies, and a copy with PPSSPP 1.20.4, in the Gathering Hall | they see each other |
| 2026-10-27 | two instances on one PC start and complete a quest | works, after two fixes: the interpreter dropped the caller's frame on a jump into a jump-table case, and `sceNetAdhocPtpOpen` refused the flag the game passes |
| 2026-10-27 | quests with a PPSSPP player on socom.cc, either side leading | works; both times MHForever listened and PPSSPP connected |
| 2026-10-29 | private room: two copies with one code meet, a third does not see them; switching public ↔ private in a hall with a PPSSPP player | works (`14-rooms-and-friends.md` §6) |
| 2026-10-30 | friends over the default relays, two networks | works |
| 2026-11-24 | Freedom: two copies (EU) on a local server and on socom.cc (through the internet, in a private room): hall, quest posted, joined from the board, completed with its rewards, back in the hall | works at 30 fps |
| 2026-11-24 | Freedom: three players, one of each release (EU, NA, JP), and one on Freedom DX; one player's program closed in the middle of a quest | they meet and play; the others go on and the game drops the one who left |
| 2026-11-24 | Freedom: a private room (two with a code, one without) | the two meet in their own hall, the third is alone in the public one |

## 7. Open work

- **Freedom online, still to check:** four players, a large monster hunted to the end, PPSSPP and real PSPs, Freedom DX's
  North American and Japanese copies, and players on different networks.
- **Measurements still to make:** EU with NA; three and four players (the PTP backlog of 3, the relay's limits); players
  on different networks, mobile data included; Linux; a real PSP.
- **Fuzzing** the ad hoc entry points and the control library's parser (§4.6).
- **PPSSPP and the server operators:** write to them about MHForever on their servers (drafts kept outside the
  repository); required before the launcher reads PPSSPP's list itself.
- **Optional:** the servers' UDP mode (the vendored client has the ENet path); a port offset setting; PPSSPP's MAC
  normalisation if a game turns out to alter the first byte; invite links (`mhforever://`); Discord Rich Presence; the
  Japanese bridge; a built-in server for LAN or VPN play (aemu's server is GPL-3.0, so it can be bundled); proposing
  hidden groups to aemu_postoffice (`14-rooms-and-friends.md` §2).

## 8. Not pursued: direct peer-to-peer links

The first design (2026-10-17) had players link to each other directly: Ed25519 identities, discovery through the
BitTorrent Mainline DHT, UDP hole punching with IPv6 and UPnP/NAT-PMP/PCP port mapping, and relaying through another
player when that failed. The community servers made it unnecessary for Freedom Unite: they work behind any NAT, hide IP
addresses and bring PPSSPP players. Hole punching succeeds for about 70 % of pairs in large measurements, and only about
11 % of Spanish users have IPv6, so a relay would be needed anyway. It could come back for lower latency or LAN play;
the libraries checked then (libjuice MPL-2.0, jech/dht MIT, miniupnpc/libnatpmp BSD-3, libsodium ISC, Monocypher
BSD-2/CC0) are all compatible with the repository's GPL-3.0-or-later.

## 9. Sources

- PPSSPP: `Core/HLE/proAdhoc.cpp`, `proAdhocServer.cpp` (crosslinks), `sceNetAdhoc.cpp`; the published server list.
- Kethen/aemu_postoffice: `design.md`, `aemu_postoffice_packets.h`, the adhocctl server and client (to 2026-10-28).
- TeamGDB/Yakumo (MIT, static recompilation of Portable 3rd): quests through a PSP ad hoc server, the no-pause rule.
- The servers' live status pages (socom.cc, Games Nexus, Neko PSP and others), October 2026.
- Trautwein et al., "Challenging Tribal Knowledge – Large Scale Measurement Campaign on Decentralized NAT Traversal",
  arXiv:2510.27500; APNIC IPv6 measurements for Spain (to 2026-10-24).
