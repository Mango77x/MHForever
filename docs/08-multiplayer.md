# 08 — Multiplayer

## 1. Goal

2–4 friends play quests together as on the PSP, over the internet, without router configuration. P2P whenever possible, relay
otherwise.

## 2. How the game does it

The game uses the PSP's **ad-hoc** stack: `sceNetAdhocctl` for groups/rooms, `sceNetAdhoc` PDP (datagrams) and PTP (streams),
and `sceNetAdhocDiscover` (measured imports: 15 + 8 + 5 functions). Ports observed in FU: UDP 10000 (room) and 20001–20004 (quest).
See `06-mhfu-module.md` §7. Our runtime implements those APIs and sends packets over the internet instead of Wi-Fi.

## 3. How the community does it (reference)

- **PPSSPP**: an *adhocctl* server (TCP 27312) for group discovery and, since 1.20, the **"aemu postoffice" relay** (TCP 27313) that
  forwards all traffic with no port forwarding. Public relays exist (EU, Asia, etc.) with Monster Hunter communities.
- **aemu / PRO Online**: the same protocol on real PSPs/Vitas via a plugin. So **a client compatible with aemu could play with PPSSPP
  users and real PSPs** .
- `aemu_postoffice` publishes a C client (`postoffice_client.h`, `adhocctl.h`) meant to be integrated into PSP implementations.
  License: **GPL-3.0** (verified 2026-10-17), which is compatible with this GPL-3.0-or-later repository (relicensed 2026-10-19); the protocol can also be
  reimplemented from its `design.md`.

## 4. Design (decided 2026-10-17): friend codes and room codes, no accounts, no servers of our own

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
     on Windows `root / "C:"` replaces the root and escapes the memory stick folder. **Found 2026-10-17, to fix.**
   - `host0:` maps to `RuntimeOptions::host_root` (debug); disable it in player builds. **To fix.**
   - GE direct memory reads (`gpu/ge.cpp` host/rd*/wr*, `raw_pointer`): every list/vertex/texture address must stay
     inside RAM/VRAM bounds.
   - Every HLE function that takes guest pointers/sizes: validate ranges (savedata, font, atrac, sas, net).
   - Fuzz the ad-hoc HLE entry points and the HLE file/GE boundaries with malformed input before release.
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

### 4.7 Implementation order and lessons for when online work starts
1. Fix the HLE boundary issues above (independent of networking).
2. ad-hoc HLE (`sceNetAdhoc*`, `sceNetAdhocctl*`, `sceNetAdhocDiscover`) over a local loopback transport, tested with our
   own homebrew; then two instances on one PC.
3. core/net: identity, jech/dht, libjuice, UPnP/PCP, encryption; room codes first, then friends (pairwise rendezvous).
4. Measure FU's traffic (packet sizes/rates on ports 10000 and 20001–20004) to size the group relay.
5. Test matrix in §6 including Spanish ISPs with CG-NAT (Digi, Másmóvil group, Orange DS-Lite) and mobile hotspot.

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
| aemu_postoffice | GPL-3.0 | PPSSPP relay protocol | ❌ link; ✅ reimplement the protocol |
| HyperDHT (Holepunch) | MIT | DHT + hole punching by key | ⚠️ JavaScript runtime; reference only |

Since the move to GPL-3.0-or-later, iroh (Apache-2.0) and toxcore (GPL-3.0+) are also license-compatible; not needed for this design.

## 6. Tests

1. Two instances on the same machine (loopback) with our own ad-hoc homebrew.
2. Two machines on a LAN.
3. Internet with different NATs (including a phone hotspot, usually strict NAT → relay).
4. Mixed Windows ↔ Linux/Deck.
5. Against PPSSPP through an aemu relay (optional).
Measure latency and loss; the game is tolerant, but verify it in FU.

## 7. Sources (2026-10-17)

- Trautwein et al., "Challenging Tribal Knowledge – Large Scale Measurement Campaign on Decentralized NAT Traversal",
  arXiv:2510.27500 (≈70 % hole-punching success, 97.6 % first attempt).
- Jiménez, "Kademlia on the Open Internet" (KTH licentiate): Mainline DHT median lookup < 200 ms with improvements.
- Pubky/pkarr and pubky.org Mainline DHT notes: ~10 M nodes, BEP 44 records ≤ 1000 bytes, expire after hours.
- libjuice, jech/dht, Kethen/aemu_postoffice, GameNetworkingSockets READMEs; iroh docs (public relays terms, DHT lookup);
  Apache Software Foundation, "GPL compatibility"; Tor Project issue on `ring`'s license.
- itigic.com, "Which Internet operators in Spain use CG-NAT".
