# 08 — Multiplayer

## 1. Goal

2–4 friends play quests together as on the PSP, over the internet, without router configuration. P2P whenever possible, relay
otherwise.

### 1.1 Every platform, one network layer (decided 2026-10-22)

Freedom Unite is the first game, not the only one: later titles may come from the 3DS, Switch, PS2, Wii or Wii U, each
with its own kind of multiplayer. The friends system and the peer-to-peer layer are written once and shared; each
console only adds an adapter. Nothing below `core/net` may know which console or game is running (the same rule as
`core/` in `CLAUDE.md`).

```
launcher            identity, friends, presence ("playing <game>"), invites, rooms        — shared by every game
   │
core/net            discovery, peer links, relay through the group, encryption            — shared by every console
   │                offers one service: a ROOM of N members with virtual addresses, in one of two modes:
   │                  · packets:  members exchange datagrams/streams (the game's own netcode)
   │                  · inputs:   members exchange controller inputs per frame (netplay)
   ▼
platforms/<console>/net   adapter: the console's network API ↔ a room
                          psp: sceNetAdhoc/Adhocctl · 3ds: local wireless · switch: local wireless (LDN) · ps2/wii: LAN
games/<id>/               data only: max players, compatible builds, mode, expected bandwidth
```

The multiplayer kinds found on these consoles, and how each maps onto a room:

| Kind | Examples | How | Serverless? |
|---|---|---|---|
| Local wireless | PSP ad-hoc, DS/3DS, Switch local | the room is a virtual LAN; the game sees nearby consoles | yes |
| LAN / system link | PS2 network adapter, Wii U LAN | the same virtual LAN | yes |
| Official online services | DS/Wii Nintendo WFC, 3DS/Wii U online, PS2 online games | per game: many used the service only for matchmaking and then played peer to peer, which a room can replace; others need a replacement service | depends on the game |
| Several controllers on one console | Wii, PS2 split screen | no game netcode: inputs are synchronised between PCs (lockstep, later rollback); needs the game to run deterministically | yes, in inputs mode |

Consequences:
- Identity and friend codes belong to the launcher, not to a game: a friend is a friend in every game, and presence
  and invites carry the game id.
- Every room carries `game id + region + build + mode + max players`; mismatched clients are refused with a clear
  message.
- The group relay (§4.3) is sized per game from measured traffic; later consoles can send much more than FU.
- The room service is the only interface between `core/net` and an adapter. Anything PSP-specific that appears in
  `core/net` while writing the first adapter is in the wrong place.

### 1.2 First transport: the community's ad hoc servers (decided 2026-10-23)

Online play ships first on the servers the PSP community already runs for PPSSPP and for real PSPs (aemu), not on
servers of our own and not on direct P2P. Reasons:
- **It works on every network.** Each player only connects out to the server (TCP), so CG-NAT, mobile data and strict
  NATs are no obstacle; nobody opens ports. Direct P2P (§4) cannot promise that without a relay of some kind.
- **No cost and no money.** Rule 7 of `CLAUDE.md` stays as it is; the servers are run by volunteers and listed by
  PPSSPP.
- **Players from day one**, including PPSSPP and real PSP players, and the servers already group the European, North
  American and Japanese releases of the game together (see §3).
- **It is proven** with Monster Hunter: PPSSPP players use it, and a sibling recompilation project (Yakumo, MHP3rd, MIT)
  reports a complete quest between two machines through such a server.

What it means for the design:
- The PSP adapter (`platforms/psp/net`) implements `sceNetAdhoc*`, `sceNetAdhocctl*` and `sceNetAdhocDiscover*` on top of
  the aemu protocols. They are PSP-specific, so they never enter `core/net`.
- The launcher picks the server (PPSSPP's published list, or any address) and gives the game a stable virtual MAC and
  the player's pseudonym.
- The final design of §4 (friends, presence, invites, room codes) is still the goal, built later on top of this: a room
  code can stand for "group G on server S", and friends exchange invites over the DHT or a direct link between
  launchers. A server of our own is only needed for direct P2P links (§4.3), which become an optional improvement.
- Before release: ask PPSSPP before the launcher reads their server list, write to the main server operators, and credit
  PPSSPP, aemu/aemu_postoffice and the server operators in the README and on the website.
- Privacy: the servers' public status pages show each player's nickname and ports; the launcher says so. Servers in
  their default `relay_only` mode do not give players' IP addresses to other players.

## 2. How the game does it

The game uses the PSP's **ad-hoc** stack: `sceNetAdhocctl` for groups/rooms, `sceNetAdhoc` PDP (datagrams) and PTP (streams),
and `sceNetAdhocDiscover` (measured imports: 15 + 8 + 5 functions). Ports observed in FU: UDP 10000 (room) and 20001–20004 (quest).
See `06-mhfu-module.md` §7. Our runtime implements those APIs and sends packets over the internet instead of Wi-Fi.

## 3. How the community does it (reference)

- **PPSSPP**: an *adhocctl* server (TCP 27312) for group discovery and, since 1.20, the **"aemu postoffice" relay** (TCP 27313) that
  forwards all traffic with no port forwarding. Public relays exist (EU, Asia, etc.) with Monster Hunter communities.
  - PPSSPP downloads its server list from `https://metadata.ppsspp.org/adhoc-servers.json` (21 servers on 2026-10-23,
    all `"data_mode": "AemuPostoffice"`); its default server is `socom.cc`. Entries have a host, a location, a Discord
    link and often a live status URL (`data.json`: games, groups, nicknames, ports). One of them (`psp.mgn.pub`) says it
    has "a good amount of Monster Hunter players".
  - adhocctl (verified in PPSSPP's `proAdhoc.h`): little-endian, packed; opcodes ping 0, login 1 (MAC[6], nickname[128],
    product code[9]), connect 2 (group[8]), disconnect 3, scan 4, scan complete 5, connect BSSID 6, chat 7 (64 bytes).
  - Relay (aemu_postoffice `design.md`, `aemu_postoffice_packets.h`): one TCP connection per ad hoc socket, opened with
    an init record (type: PDP, PTP listen, PTP connect, PTP accept, unreliable PDP; source and destination MAC and port);
    PDP blocks up to 10 KiB, PTP up to 50 KiB. There is also a UDP path through ENet.
  - The reference server's `game_db.json` "crosslinks" product codes into one game: `ULES01213` (FU Europe),
    `ULUS10391` (FU North America) and `ULJM05500` (MHP2G Japan) all play in the same groups.
- **aemu / PRO Online**: the same protocol on real PSPs/Vitas via a plugin. So **a client compatible with aemu could play with PPSSPP
  users and real PSPs** .
- `aemu_postoffice` publishes a C client (`postoffice_client.h`, `adhocctl.h`) meant to be integrated into PSP implementations.
  License: **GPL-3.0** (verified 2026-10-17), which is compatible with this GPL-3.0-or-later repository (relicensed 2026-10-19); the protocol can also be
  reimplemented from its `design.md`. Its client (`client/postoffice.c` with Windows/Linux socket layers and ENet, plus
  `adhocctl/client/adhocctl.c`) is the one PPSSPP uses; the plan is to vendor it in `third_party/aemu_postoffice/`.
  Its reference server (`server_cpp`) can run locally, so tests can talk to a real server without internet or game data.

## 4. Target design (decided 2026-10-17): friend codes and room codes, no accounts, no servers of our own

Since 2026-10-23 this is the design built after the first transport of §1.2, on top of it.

Requirements: no hosted servers, no accounts; a local pseudonym, friend codes and room codes; the launcher is the
hub (friends list, presence, invites, rooms). Researched 2026-10-17 (sources in §7).

```
recompiled game ── sceNetAdhoc*/sceNetAdhocctl HLE (platforms/psp/runtime)
                         │ virtual ad-hoc LAN: virtual MACs, PDP/PTP ports, one group per room
                         ▼
core/net ── one UDP socket per player, shared by every peer link
   ├─ identity:   Ed25519 key pair generated locally + pseudonym (exportable backup file)
   ├─ discovery:  BitTorrent Mainline DHT (public, ~10M nodes, nobody owns it)
   ├─ links:      UDP hole punching (+ IPv6 direct, + UPnP/NAT-PMP/PCP port mapping)
   └─ fallback:   relay through another player of the group; optional community relays
```

### 4.1 Identity and friend codes
- First run: the player picks a pseudonym; the launcher creates an Ed25519 key pair (never leaves the PC) and offers a
  backup file (losing the PC without it = new identity; friends re-add the new code).
- **Friend code** = the first 80 bits of SHA-256(public key), base32 → `XXXX-XXXX-XXXX-XXXX` (16 characters, typeable).
  Collisions or impersonation would need ~2^80 work. On first contact each side proves it owns the key behind the code
  (challenge signed with Ed25519), then both store the full public keys; after that every link is authenticated.
- Adding a friend is mutual (both enter each other's code, or one accepts the other's request once connected).

### 4.2 Discovery and presence (Mainline DHT)
- (Superseded in part by §4.6.2: the public code is only announced while accepting friend requests; established friends
  use a pairwise secret rendezvous.)
- While the launcher runs it **announces** itself (BEP 5 `announce_peer` with `implied_port`) under
  `info_hash = SHA-1("mhf-friend" ‖ friend code)`. `implied_port` makes the DHT record the NAT-mapped port of the very UDP
  socket used for play, which is what hole punching needs. Re-announce every ~15 min (DHT peer entries expire in ~30 min).
- To find a friend: `get_peers` on that info hash → candidate addresses → hole punch (§4.3) → authenticate (§4.1).
  Median Mainline lookups are typically one second or less; good enough for presence and invites.
- Once linked, friends keep a direct, encrypted keep-alive link while both launchers are open: **presence**
  ("in the village", "in a quest", "room open"), **invites** and chat travel directly, with no third party.
- Optional, later: BEP 44 signed records (≤1000 bytes) to publish a richer status without a live link.
- Library: `jech/dht` (MIT, single C file, used by Transmission; BEP 5, IPv4 + IPv6) or our own small KRPC client.
- Caveat: some corporate/university networks block the DHT; home networks normally don't.

### 4.3 Peer links (NAT traversal)
1. **IPv6 first**: when both players have global IPv6 (Movistar, Orange and Digi in Spain deploy it) the link is direct
   with no NAT at all. This also sidesteps CG-NAT, which several Spanish ISPs use on IPv4 (Másmóvil/Yoigo/Pepephone, Digi,
   Orange/Jazztel via DS-Lite, Fi).
2. **Automatic port mapping**: try UPnP-IGD, NAT-PMP and PCP (`miniupnpc`, `libnatpmp`, BSD-3). PCP works through
   Orange's DS-Lite.
3. **UDP hole punching**: both sides learn each other's public endpoint (DHT `implied_port`, public STUN servers) and send
   simultaneously, ICE-style. `libjuice` (MPL-2.0, C, no dependencies, ICE/STUN/TURN, IPv4/IPv6) implements the
   connectivity checks; signaling (exchanging candidates) goes over the DHT/first-contact path. Large measurements put
   decentralised hole punching at ~70 % success, 97 % of it on the first attempt; failures are mostly
   endpoint-dependent ("symmetric") NATs, common on mobile data and some CG-NAT.
4. **Relay through the group**: game traffic is small, so any member who can reach two others forwards packets between
   them (the room host by default). Only a group where *every* member is behind a symmetric NAT cannot meet.
5. **Optional community relays**: as a last resort, the launcher can use public relays run by others (PPSSPP's aemu
   postoffice servers, reimplemented protocol), off by default and clearly labelled.

### 4.4 Rooms
- **Room code** (6 characters) created by the host. The host announces under `SHA-1("mhf-room" ‖ code ‖ game id)`;
  joiners look it up, link to the host and then to every member (full mesh of up to 4, relayed where needed).
- The room carries `game id + region + build version`; mismatched clients are refused with a clear message.
- Friends see "room open" in presence and join with one click (no code needed).
- Inside the game nothing changes: the room is one virtual ad-hoc group, and the game's Gathering Hall works as on the
  PSP.

### 4.5 Privacy and safety (shown in the launcher)
- Linked players see each other's IP address, as in any P2P game; the DHT also sees that an address runs the launcher
  (as with torrents). An option "only connect through the group relay" hides your IP from non-friends in rooms.
- No central moderation: blocking is local (block list by key).
- All links are encrypted and authenticated (keys from §4.1, e.g. Noise or libsodium `crypto_box`/`crypto_kx`).

### 4.6 Security model (threat review 2026-10-17) — must be done before multiplayer ships
1. **The game's own network code is the largest risk.** FU parses peers' packets with 2008 code never meant for hostile
   input. Containment: recompiled code only reaches guest memory through bounds-checked accessors (`GuestMemory`
   aot_load*/aot_store*, slow path for anything outside RAM) and indirect jumps only reach recompiled functions or the
   interpreter, so a game exploit stays inside the virtual PSP. The walls to harden are the HLE boundaries:
   - `hle_io.cpp` `host_path()`: reject path components with a drive or root name (`C:`, `\\?\`, ADS `file:stream`) —
     on Windows `root / "C:"` replaces the root and escapes the memory stick folder. **Fixed** (`resolve_under_root`,
     tested in `test_hle_io`).
   - `host0:` maps to `RuntimeOptions::host_root` (debug). **Fixed 2026-10-23:** empty by default, and an empty root never
     resolves, so the device is closed unless a developer tool sets a folder.
   - GE direct memory reads. **Checked 2026-10-23:** the GE reaches memory only through `host(address, length)`, which
     returns nothing outside RAM and the EDRAM mirrors (reads give 0, writes are dropped); the OpenGL backend and the
     frontend use `GuestMemory::raw_pointer(address, length)`, also bounds-checked, and skip a null result.
   - HLE functions that take guest pointers/sizes. **Checked 2026-10-23:** they go through `GuestMemory`
     (`load*`/`store*`/`copy_in`/`copy_out`/`read_string`), which throws outside guest memory; `Kernel::thread_main`
     catches it and stops the program with a message. A bad pointer from the network therefore ends the game cleanly
     instead of touching host memory. Host buffers sized by the guest (`hle_misc`, savedata, thread arguments) can at
     worst fail to allocate, which ends the same way. The new net HLE must follow the same rule and cap every size by
     the protocol limits (PDP 10 KiB, PTP 50 KiB blocks).
   - Fuzz the ad-hoc HLE entry points and the HLE file/GE boundaries with malformed input before release. **Pending**,
     with the net HLE.
2. **IP exposure.** Linked players see each other's IP (city-level geolocation, possible DDoS while playing).
   **Design correction:** the public friend code must not be a permanent rendezvous, or anyone holding a leaked code
   could find a player's IP and online times. So:
   - The public code is only announced while the player has "accept friend requests" open (time-limited).
   - Established friends meet at a **pairwise secret rendezvous**: `info_hash = H(X25519 shared secret ‖ epoch)`, rotated
     periodically; only that friend can compute it.
   - Public rooms: option to route through the room host so non-friends never see your IP.
3. **Launcher network code** (DHT/KRPC bencode parsing, ICE, our framing): strict validation, size limits, rate limits,
   fuzzing (libFuzzer) in CI.
4. **Covered by design:** all links encrypted and mutually authenticated by Ed25519 keys (no impersonation, no MITM on
   the same LAN); cheating among friends is out of scope (same as on PSP) and only affects the game; blocking is local.
5. **Distribution:** if the launcher ever self-updates, updates must be signed (Ed25519/minisign) and verified before
   running; an unsigned updater would be the most dangerous entry point.
Target: equal or better than ad-hoc in PPSSPP (where peer packets also reach the emulated game, without encryption or
authentication).

### 4.7 Implementation order (revised 2026-10-23)
1. Fix the HLE boundary issues above (independent of networking).
2. Vendor the aemu_postoffice client; the PSP adapter (`sceNetAdhoc*`, `sceNetAdhocctl*`, `sceNetAdhocDiscover*`) on top
   of it, with call and packet tracing. **Done 2026-10-23** (`05-runtime-hle.md` §5i), tested against the reference server
   running locally; a homebrew test against it and the first run with the game are still to do.
3. Two instances on one PC, through a local server, complete a quest together.
4. Launcher: server choice (PPSSPP's list or an address), live status, stable virtual MAC, pseudonym; a real game on a
   community server. Credits and the messages to PPSSPP and the operators before release (§1.2). Target: 0.6
   (0.5 was the North American release, so multiplayer moved back one version).
5. Measure FU's traffic (packet sizes/rates on ports 10000 and 20001–20004).
6. The design of §4 on top: identity, friends, presence, invites and room codes (room = group on a server); then,
   optionally, direct P2P links (libjuice, UPnP/PCP, IPv6) for lower latency. Target: 0.7.
7. Test matrix in §6 including Spanish ISPs with CG-NAT (Digi, Másmóvil group, Orange DS-Lite) and mobile hotspot.

### 4.8 Compatibility with PPSSPP and real PSPs 
Optional mode, later: speak PPSSPP's adhoc-server protocol plus aemu postoffice to join existing community servers
(not ours). Separate from the friends system.

## 5. Libraries (licenses checked 2026-10-17 against GPL-2.0-only; the repository is GPL-3.0-or-later since 2026-10-19, which only widens what is allowed)

| Library | License | Use | Verdict |
|---|---|---|---|
| **libjuice** | MPL-2.0 | ICE/STUN/TURN, hole punching | ✅ MPL-2.0 lists GPL-2.0 as a "Secondary License" |
| **jech/dht** | MIT | Mainline DHT (BEP 5, IPv6) | ✅ |
| **miniupnpc / libnatpmp** | BSD-3 | UPnP, NAT-PMP, PCP | ✅ |
| **libsodium** or **Monocypher** | ISC / BSD-2 or CC0 | Ed25519, key exchange, encryption | ✅ |
| GameNetworkingSockets (Valve) | BSD-3 | Messages + P2P (WebRTC ICE, own signaling) | ✅ license, but heavy (protobuf, WebRTC); not needed |
| iroh (n0) | MIT/Apache-2.0, but depends on `ring` (Apache-2.0 AND ISC) | Dial by key, DHT discovery, relays | ❌ Apache-2.0 is incompatible with GPL-2.0; also Rust toolchain; n0's public relays are "development and hobby use only" |
| c-toxcore (Tox) | GPL-3.0+ | Same model (key IDs, DHT, relays) | ❌ GPL-3 only — design reference |
| aemu_postoffice | GPL-3.0 | PPSSPP relay protocol and client | ✅ since the GPL-3.0-or-later relicensing: vendored client (§1.2) |
| HyperDHT (Holepunch) | MIT | DHT + hole punching by key | ⚠️ JavaScript runtime; reference only |

Since the move to GPL-3.0-or-later, iroh (Apache-2.0) and toxcore (GPL-3.0+) are also license-compatible; not needed for this design.

## 6. Tests

1. Two instances on the same machine (loopback) with our own ad-hoc homebrew. *Homebrew still to write*; the live test
   `psp_adhocctl_server` (two players against a real server) covers the library and the sockets.
2. Two machines on a LAN.
3. Internet with different NATs (including a phone hotspot, usually strict NAT → relay).
4. Mixed Windows ↔ Linux/Deck.
5. Against PPSSPP through an aemu relay.
Measure latency and loss; the game is tolerant, but verify it in FU.

How to run these locally is in `BUILDING.md` ("Trying online play"). Results so far (reference server running in WSL2 on the same PC):

| Date | What | Result |
|---|---|---|
| 2026-10-23 | `psp_adhocctl_server`: two players join a group, see each other, exchange datagrams (also one that does not fit) and a PTP stream, close | passes (3 of 3 runs) |
| 2026-10-24 | keepalive: 45 s idle | passes (needs a server on a steady clock) |
| 2026-10-24 | jitter: 300 datagrams at 30 per second | 300 of 300 arrive, median delay 16 ms, max 23 ms |
| 2026-10-24 | two copies of the game in the Gathering Hall | they see each other |
| 2026-10-24 | our game with PPSSPP 1.20.4 in the Gathering Hall | they see each other (PPSSPP needs the relay forced on for this server) |
| 2026-10-24 | movement of a remote hunter | steps every 0.67 s in ours and in PPSSPP: the game's own traffic |
| 2026-10-24 | starting a quest together | **fails**: the host freezes (see `10-roadmap.md`, Phase 5) |

Not yet tried: a public server, mobile data or strict NATs, more than two players, Linux, a real PSP.

## 7. Sources (2026-10-17)

- Trautwein et al., "Challenging Tribal Knowledge – Large Scale Measurement Campaign on Decentralized NAT Traversal",
  arXiv:2510.27500 (≈70 % hole-punching success, 97.6 % first attempt).
- Jiménez, "Kademlia on the Open Internet" (KTH licentiate): Mainline DHT median lookup < 200 ms with improvements.
- Pubky/pkarr and pubky.org Mainline DHT notes: ~10 M nodes, BEP 44 records ≤ 1000 bytes, expire after hours.
- libjuice, jech/dht, Kethen/aemu_postoffice, GameNetworkingSockets READMEs; iroh docs (public relays terms, DHT lookup);
  Apache Software Foundation, "GPL compatibility"; Tor Project issue on `ring`'s license.
- itigic.com, "Which Internet operators in Spain use CG-NAT".
