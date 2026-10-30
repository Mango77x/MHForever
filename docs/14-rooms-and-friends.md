# 14 — Private rooms and friends: research and plan (2026-10-29)

Online play works since 0.6.0: players pick a community ad hoc server and meet in the game's own Gathering Hall, with
PPSSPP and real PSP players. This document is the research for the last part of the online plan: **private rooms**
(play only with the people you choose, on any public server) and **friends** (see who is playing, invite them, join
them in one click). With both, and the open experiments of `13-online-plan.md` §7, online play for Freedom Unite is
finished.

It refines `08-multiplayer.md` §4 and `13-online-plan.md` §6.2 and §6.6, and replaces the Mainline DHT as the way
friends find each other (§4.3). Addresses in the game's code are in the local `research/rooms-2026-10-29.md`, as
`CLAUDE.md` asks.

Tags: **[verified]** checked against a primary source or measured on this date; **[to test]** expected from the
evidence but not yet seen working.

## 1. How the game picks a Gathering Hall [verified 2026-10-29, European executable]

- The game has **24 halls**. To show them it builds the 24 names it can use, a six-byte prefix followed by the hall
  number written with `%d` and padded to two digits: `MHP2Q000` to `MHP2Q023`. The prefix is `MHP2Q0`, or `MHP2A0` in a
  second mode chosen by a flag (which mode is which is not known yet; both are seen live).
- It then scans (`sceNetAdhocctlScan`, `GetScanInfo`) and **compares every group the server reports with its 24 names,
  byte for byte**. A match marks that hall as occupied. **Any other name is ignored.**
- It joins the hall the player picks through the network configuration dialog, with that hall's name. Seen at run
  time on socom.cc on 2026-10-29 (R1): the scan returned 13 and 14 Freedom Unite groups, and the game joined
  `MHP2Q001` and then `MHP2Q013` ("connect" action of the dialog), leaving the group, the library and the ad hoc
  stack each time it left a hall and starting them again before the next scan.
- Live public servers on 2026-10-29 match this exactly: Freedom Unite rooms `MHP2Q000` to `MHP2Q023` and `MHP2A000` /
  `MHP2A023` (14 rooms on socom.cc at once); the Japanese release uses `集会所NN` (seen: 12, 15, 23; the prefix is three
  Shift-JIS characters, also six bytes); Portable 3rd uses `MHP3Q000` to `MHP3Q023` the same way.

Consequence: **a group with any other name is invisible to every Freedom Unite player, on PPSSPP, a PSP or our game.**
Their game never lists it and never joins it. A private room needs nothing from the server.

## 2. What the servers do [verified 2026-10-29, aemu_postoffice source and live status pages]

- A scan returns every group of the same game, crosslinks included (FU Europe, FU North America and Portable 2nd G
  count as one game). So a private group's name does reach strangers' clients; it is the game that drops it (§1).
- Group names are not checked: any 8 bytes without a zero byte are a valid name.
- The relay addresses sockets by MAC address and port, not by group. Inside a private group, traffic goes only to the
  peers the game sees in that group.
- The server has an internal **channel** per group (fixed at 11 for today's protocol; a negative channel hides a group
  from scans). There is no client message to set it yet. A small protocol extension ("join on channel N") would give
  real server-side hidden rooms; worth proposing upstream later, not needed now.
- Status pages (`data.json`, `status.xml`) show every group's name and its players' nicknames. A private room's
  nicknames are therefore public there, as in any room; its name tells nothing.
- aemu_postoffice is active (commits on 2026-10-26 to 2026-10-28, several from PPSSPP's author), so a proposal would be
  read.

## 3. Private rooms: room codes that rename the halls

### 3.1 How it works
- The player creates a **room code** in the launcher, e.g. `K7QF-29XM` (8 characters of Crockford base32, 40 bits),
  or types a friend's. The game starts with it, or gets it while running from the in-game menu (§3.4).
- Our control library (`platforms/psp/runtime/src/adhocctl.cpp`) renames every hall the game asks for: game name
  `MHP2Q0NN` → server name `F` + 5 base32 characters of a hash (FNV-1a) of the room code and the game's six-byte
  prefix + the hall's own two characters `NN`. Each of the 24 halls (and each prefix) gets its own private name, so the
  game's hall list keeps working inside the private room: a friend sees "hall 3 occupied" and walks in. The hall number
  stays readable on status pages; the code does not (25 bits of hash cannot give it back).
- In scan results the library translates the private names back to the game's names and **drops every public group**,
  so the game shows only the private room's halls. To translate back it needs the game's prefixes: the game module
  gives them (`MHP2Q0`, `MHP2A0` and the Japanese `集会所`), and any prefix the game joins with is learnt. A hall with an
  unknown prefix is only missing from the list; joining it still works, because joining needs no translation back. Connect, disconnect, peer lists and the network dialog use the
  game's name on the game's side and the private one on the server's side.
- Nothing else changes: PDP, PTP and the relay are untouched.

### 3.2 What it gives and what it does not
- Works on every aemu server today, with no server of ours and nothing for the operators to do.
- Strangers cannot see or join the room: their game ignores the name (§1), and finding the name from the status page
  does not give the code (the name is a short hash of it).
- Only MHForever players can enter: a PPSSPP or PSP friend cannot type a room code. Public halls stay the default, and
  the launcher says so next to the option.
- **Not a lock.** Someone with a modified client could copy a private name from a status page and join that hall. The
  game itself could not; a room code is privacy for everyday play, not security. The launcher must not call it
  "secure".
- A room code gives its group its own 24 halls (for each of the two prefixes), four hunters each, as on the PSP: eight
  friends with one code fill two halls, and another group with another code on the same server has 24 different
  ones. Mapping all 24 halls to one server group was rejected: the game treats each hall number as a different room.

### 3.3 Sharing a room
- **Room code**: typed or pasted. Always works.
- **Invite link**: `mhforever://join?server=socom.cc&room=K7QF29XM&game=mhfu`. The launcher registers the
  `mhforever:` scheme (Windows: a key under `HKCU\Software\Classes`; Linux: a desktop entry with
  `x-scheme-handler/mhforever`). This is a change to the player's system, so it is offered once, not done silently, and
  the setting removes it. A link opens the launcher, which says who is in the room's server and asks before starting.
- A link carries the server too, so friends on different default servers still meet. An invite from a friend
  (§4.3) also carries the hall number, so the menu can say "Your friend is in hall 3".

### 3.4 Switching between public and private halls while playing (decided 2026-10-29)
Private rooms must not cost a player their PPSSPP and PSP friends, so the mode is a switch, not a setting fixed at
start:
- **In the launcher**, "Create a room" and "Join a room" (code, invite link, or a friend's invite) start the game in
  private mode with that code. Starting the game normally starts it in public mode.
- **In the in-game menu (Escape)**, the Online tab shows the mode and changes it in real time: "Halls: Public /
  Private (K7QF-29XM)", with fields to create a new code, type one, or copy the current one (or its invite link). The
  menu does not pause the game during online play, so this works with friends waiting.
- **When the change takes effect:** the game only asks for hall names when it scans and when it enters a hall, so the
  control library reads the mode at those moments. Switching in the village or the Gathering Hall entrance applies at
  once; switching while inside a hall (or a quest) never disconnects anyone: it applies the next time the player leaves
  the hall and enters again. The menu says which one happened: "Next time you enter the Gathering Hall you will see the
  halls of room K7QF-29XM" or "... the public halls, with PPSSPP and PSP players".
- **The server too** (added 2026-10-29, at the owner's request): the Online tab also picks the ad hoc server among the
  launcher's. The game keeps one session with the server and opens its sockets through the server's relay, so another
  server takes over only when that cuts nothing off: no group joined or being joined, no scan under way, no socket
  open (`Runtime::set_adhoc_server`; the control library swaps its link and the handlers stay). In the village that is
  at once; in a hall or a quest it waits until the game left it. Checked in the game the same day: in the village, inside
  a hall (nobody cut off, the move came after leaving), and the launcher kept the server chosen.
- **Where the state lives:** in the game process (the runtime's online state, shared by the control library and the
  overlay), so no link to the launcher is needed while playing. The game reports the mode it ends with, so the launcher
  offers the last code next time.
- Public stays the default, and the mode is shown wherever online status is shown (menu, launcher, presence).
- **Built 2026-10-29:** Home's "Play with friends" card (Create room, Join with code, Copy code, Back to public halls;
  the setting `adhocRoom`, passed to the game as `--adhoc-room`) and the Escape menu's Online tab (`07-launcher-ux.md`
  section 3c). The game learns a change through `Runtime::set_adhoc_room`; the launcher reads the room the game ended with
  from the `room=` line of the file the game writes for it. Codes are typed or pasted: no text field in the game.

### 3.5 The Japanese bridge (optional, later)
The same translation can map `集会所NN` and `MHP2Q0NN` to one private name, so our Japanese and international players
share halls. It only renames groups; whether the two releases understand each other's data is [to test] with two
copies.
It does not reach PPSSPP's Japanese players, whose halls stay `集会所NN`.

## 4. Friends

### 4.1 Requirements (unchanged since `08-multiplayer.md` §4)
No accounts, no servers of ours, no money; a friend is a friend in every game; presence ("playing Freedom Unite on
socom.cc, room open"), invites, join in one click; no IP address given to friends or strangers; works behind CG-NAT and
on mobile data.

### 4.2 Options compared

| | Mainline DHT (`08` §4.2) | **Nostr relays** | Discord Rich Presence | Our ad hoc server |
|---|---|---|---|---|
| Works behind CG-NAT, mobile data | Partly (UDP, hole punching) | **Yes** (WebSocket out) | Yes | Yes |
| Servers needed | None (DHT nodes) | Public relays, many, run by volunteers | Discord | The community's servers |
| Friend sees your IP | Yes, on direct links | **No** (only the relay sees it) | No | No |
| Account | No | **No** (a key pair) | Discord account | No |
| Presence when offline | No | No (ephemeral) | — | No |
| Code to write | Native DHT, KRPC parser, ICE, crypto | **A few hundred lines of TypeScript in the launcher UI** | IPC over a local pipe | Abuses servers meant for games |
| Libraries | jech/dht (MIT), libjuice, libsodium | nostr-tools (public domain), NIP-44 encryption | none needed | — |
| Verdict | Too much for presence; keep for direct links | **Recommended** | **Also, as an optional extra** | Rejected: not ours to use this way |

Why the DHT loses for friends: presence needs a live link between friends, which needs hole punching (about 70 %
success, `13-online-plan.md` §5) and shows each friend the other's IP address. Nostr relays carry small signed messages
over an outgoing WebSocket, the same model as the ad hoc servers, and keep IP addresses between the player and the relay.

### 4.3 Friends over Nostr: the design
Facts used [verified 2026-10-29, NIP-01, nostr-tools]: events are signed with secp256k1 Schnorr keys (BIP-340); kinds
20000–29999 are **ephemeral** (relays forward them to subscribers and do not store them); subscriptions filter by
author, kind and tag; NIP-44 is the versioned encryption for payloads; nostr-tools is public domain (Unlicense), runs in
the browser and supports NIP-44 and relay pools.

- **Identity.** The launcher already has a secret identity seed, a backup file and a friend code. A Nostr key pair is
  derived from the seed (`HKDF(seed, "mhf-nostr")`), so the existing backup covers it. The friend code becomes the first
  80 bits of `SHA-256(public key)`, as `08-multiplayer.md` §4.1 planned.
- **Adding a friend.** Player A opens "Add a friend" for 10 minutes; their launcher announces their public key every
  30 s in an ephemeral event tagged with a hash of their friend code. Player B types A's code, finds the key, checks that
  it hashes to the code, and sends a request encrypted to A. A accepts; both store each other's key and nickname. Both
  must be online at the same time, like a friend search on a console. A link (`mhforever://friend?key=…`) does the same
  without typing.
- **Who sees the friendship.** For each pair of friends both launchers derive a shared secret (the NIP-44 conversation
  key) and from it a **pair key** that rotates every day. Presence and invites for that friend are signed with the pair
  key and encrypted with the shared secret. A relay sees an address subscribing to a handful of random-looking keys,
  not who is friends with whom, and nobody else can read or forge them.
- **Presence.** While the launcher is open: one ephemeral event per friend every 60 s and on every change: in the
  launcher / playing Freedom Unite (region) / server / room open with free places. A friend is offline after 3 minutes
  without one. Nothing is stored, so nothing remains once the launcher closes.
- **Invites and joining.** "Invite" sends the server and room code to that friend; "Join" on a friend whose room is open
  does the same in reverse. Both open the confirmation of §3.3.
- **Relays.** Four or five public relays by default, editable in Settings → Online. Traffic is tiny (a friend list of
  20 sends about 20 events a minute). Read each relay's policy (NIP-11) before listing it, as with PPSSPP's servers.
- **Blocking and removing** are local, as in `08-multiplayer.md` §4.5.
- **Where the code lives.** The launcher UI (TypeScript, nostr-tools); the native side derives the key and passes only
  the Nostr key to the UI. The game is not involved: it only receives a server and a room code at start.

**Built 2026-10-30** (`launcher/ui/src/friendNetwork.ts`, the drawer in `Friends.tsx`), as above, with these details:
- Kinds: 21470 the announcement of "Let friends add me" (tag `t` = the first 128 bits of
  `SHA-256("mhf-friend-code:" + code)`, empty content), 21471 a request or its answer between the players' own keys
  (tag `p`, NIP-44), 21472 presence, invite and goodbye between friends (signed with the pair key, NIP-44).
- Pair key for messages from X to Y on UTC day d: `HMAC-SHA256(conversation key, "mhf-pair:d:X:Y")`. A launcher listens
  for days d-1, d and d+1 (clocks a little apart around midnight).
- The Nostr key is `HKDF-SHA256(identity seed, info "mhf-nostr")` (the launcher, `nostr_key`); the friend code is the
  first 80 bits of `SHA-256(public key)`. The friend code of 0.7 (a hash of the seed) is gone: nothing used it.
- Every event is checked before use: signature (nostr-tools), at most 4 KB, created within 5 minutes of now, at most 30
  a minute per sender, the same event once even when several relays carry it, and each field validated (names cut to
  24 characters, servers as the launcher checks them, room codes as 8 Crockford characters). A friend that is already
  in the list and sends a request again (a new computer) gets an answer at once; a blocked key is ignored. Requests
  from anyone else are shown only while "Let friends add me" is open (at most 20), so a key seen during those ten
  minutes cannot be used to pester the player later. Names lose control characters and bidirectional overrides.
- Closing the launcher says goodbye (friends see the player offline at once); a launcher that dies without it is
  offline for its friends after three minutes.
- Default relays (NIP-11 read 2026-10-30, all public, each forwarded the ephemeral kinds in a test): nos.lol,
  relay.primal.net, nostr.mom, offchain.pub, relay.nostr.net (`friendRelays` in the launcher's settings).
- While the game runs the launcher folds the game's room and server into its settings every 2 s, so friends see a
  room change made in the Escape menu.
- Join: the friend's server and room become the player's and the game starts. Invite: sends the player's server and
  room.
- **In the game too** (the Online tab of the Escape menu): while the game runs the launcher writes the friends and the
  invites to `game_friends.txt` (one line each: `friend|invite`, key, `offline|launcher|playing`, server, room, name,
  separated by tabs; the game skips any line that does not read so) and the game appends what the player does there
  (`invite <key>`, `dismiss <key>`) to `game_friend_actions.txt`, which the launcher takes every 2 s. Join in the game
  uses the live switch of §3.4: the friend's server and room apply as soon as that cuts nobody off. Offline friends are
  only counted.

### 4.4 Discord, as an extra
Most of the community is on Discord. Rich Presence over Discord's local IPC can show "Hunting in Freedom Unite · room
2/4" with a **Join** button whose secret is the invite link of §3.3. Recent open-source games still do this without
Discord's SDK (two open-source pull requests seen on 2026-10-29), but Discord now documents invites under its Social SDK, so whether
Join still works without it is [to test]. It needs an application id registered by the project (free). Optional and
off until the player turns it on.

## 5. Security and privacy
- Room codes and pair keys are secrets: never in logs, never in URLs other than an invite link the player chose to
  share.
- Everything that arrives from a relay is untrusted: strict parsing, size limits (Nostr events of a few hundred bytes),
  rate limits, signature check before decrypting, and the same fuzzing as the ad hoc parsers (`13-online-plan.md` E9).
- Relays see the player's IP address and when they are online, as the ad hoc servers do; the launcher says so.
- The game's own packets from strangers remain the larger risk (`08-multiplayer.md` §4.6); private rooms reduce it.

## 6. Experiments

| # | Experiment | Decides |
|---|---|---|
| R1 | Trace the group names the game scans and joins (both prefixes, a few halls) | §1 at run time; what `MHP2A0` is. **Done 2026-10-29 for `MHP2Q0`** on socom.cc; `MHP2A0` not reached yet |
| R2 | Two copies with the same room code on the local reference server: both enter one private hall; a third copy without the code does not see it | §3.1. **Done 2026-10-29 at library level** (`psp_adhocctl_server` against the reference server, 3 of 3 runs): the two with the code meet, the third sees only the renamed group and lands alone in its own hall of the same number. **With the game, 2026-10-29**: two copies with code `K7QF-29XM`; B entered hall 2 first, A's scan then showed exactly one hall, `MHP2Q002` (the private one, under the game's name), A joined it and the two exchanged the Gathering Hall's datagrams |
| R2b | Switch public → private → public from the Escape menu: in the village (applies at once) and inside a hall (applies after leaving), with a public player present | §3.4. **Inside a hall, 2026-10-29** on socom.cc, with a PPSSPP player in it: public → private kept the player there (the connection made stays, as designed); after leaving and entering again the hall was empty. A switch in the village applied at the next hall, and private → public showed the PPSSPP players again after entering anew. **Done** |
| R3 | R2 on socom.cc, with a PPSSPP player in public halls at the same time | §3.2 on a busy server |
| R4 | Our Japanese and European copies in one bridged hall, then a quest | §3.5 |
| R5 | Two launchers add each other by code and see presence through public relays, one of them on mobile data | §4.3. **2026-10-30**: `launcher/ui/tools/friends-live-check.ts` (two networks with fresh keys on the five default relays: add by code, accept, presence, invite) passed; the drawer in a browser against a third network too. Two real launchers on different networks still to try |
| R6 | Rich Presence Join between two Discord accounts | §4.4 |

## 7. Plan

**0.8 "Private rooms"**: §3.1–§3.4 (room codes, invite links, the switch in the launcher and in the Escape menu), R1–R3 and R2b; the remaining online
experiments E2, E4, E5 and E8 of `13-online-plan.md`.
Acceptance: two MHForever players on different networks complete a quest in a private room on a public server while
strangers play in the public halls.

**0.9 "Friends"**: §4.3 (friend codes over Nostr, presence, invites, join), R5; fuzzing (E9); Discord (§4.4, R6) if it
works without the SDK.
Acceptance: a friend added by code sees "room open" and joins it in one click; a stranger on the same server does not
see the room.

**After that, optional**: the Japanese bridge (§3.5, R4); a built-in server for LAN or VPN play (as Yakumo does);
proposing hidden channels to aemu_postoffice (§2). Direct P2P links (`08-multiplayer.md` §4.3) are no longer needed
for Freedom Unite.

## 8. Questions for the owner
1. Private rooms only for MHForever players, with a switch back to the public halls at any time (§3.4): fine?
2. Nostr for friends instead of the DHT: fine? It means relying on public relays run by others, as with the ad hoc
   servers.
3. Discord Rich Presence: wanted, if Join works without the SDK? It needs an application registered under the
   project's name.
4. Register the `mhforever:` link scheme, offered once on first use?

## 9. Sources (2026-10-29)
- The European executable of Freedom Unite: the hall names, the scan comparison (`research/rooms-2026-10-29.md`).
- Live status pages of the servers in PPSSPP's list (socom.cc, Games Nexus, Neko PSP and others), read on 2026-10-29.
- Kethen/aemu_postoffice: `adhocctl/server_cpp/server.cpp` and `client.cpp` (scan, chat, channels, login checks),
  commits to 2026-10-28.
- nostr-protocol/nips: NIP-01 (events, ephemeral kinds, filters), NIP-44 (encryption); nbd-wtf/nostr-tools README.
- Discord developer documentation, "Managing Game Invites" and "Setting Rich Presence" (Social SDK); pull requests
  ItsKodas/blastyard#31 and 47PADO47/nfs-mw#247, Rich Presence join without the SDK.
- Ryujinx's LDN guide (passphrases for private local-wireless rooms), the closest precedent for room codes.
- TeamGDB/Yakumo README (ad hoc servers, and hosting from the game for LAN or VPN).
