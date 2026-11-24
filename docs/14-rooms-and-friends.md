# 14 — Private rooms and friends

Since 0.8.0, on top of online play on the community servers (`08-multiplayer.md`): **private rooms**, halls only the
players who share a code can see, on any public server; and **friends**, added by code, who see who is playing,
invite each other and join in one click. Neither needs a server or an account of MHForever's own. Addresses in the
game's code are kept in the local `research/rooms-2026-10-29.md`, outside the repository.

## 1. How the game picks a Gathering Hall [verified 2026-10-29, European executable]

- The game has **24 halls**. To list them it builds the 24 names it can use, a six-byte prefix followed by the hall
  number padded to two digits: `MHP2Q000` to `MHP2Q023`. The prefix is `MHP2Q0`, or `MHP2A0` in a second mode chosen by
  a flag (which mode is which is not known; both are seen on live servers).
- It scans (`sceNetAdhocctlScan`, `GetScanInfo`) and **compares every group the server reports with its 24 names, byte
  for byte**. A match marks that hall as occupied; **any other name is ignored**.
- It joins the hall the player picks through the network configuration dialog, with that hall's name, and leaves the
  group, the library and the ad hoc stack each time it leaves a hall (seen on socom.cc, 2026-10-29).
- Live servers match this: Freedom Unite halls `MHP2Q000`–`MHP2Q023` and `MHP2A000`–`MHP2A023`; the Japanese release
  uses `集会所NN` (three Shift-JIS characters, also six bytes); Portable 3rd uses `MHP3Q000`–`MHP3Q023` the same way.

So **a group with any other name is invisible to every Freedom Unite player**, on PPSSPP, a PSP or MHForever: their game
never lists it and never joins it. That is what private rooms use.

## 2. What the servers do [verified 2026-10-29, aemu_postoffice source and live status pages]

- A scan returns every group of the same game, crosslinks included (Freedom Unite Europe, North America and Portable
  2nd G count as one game), so a private group's name does reach strangers' clients; it is their game that drops it.
- Group names are not checked: any 8 bytes without a zero byte are a valid name.
- The relay addresses sockets by MAC address and port, not by group: inside a private group, traffic goes only to the
  peers the game sees there.
- The server has an internal **channel** per group (fixed at 11; a negative channel hides a group from scans), with no
  client message to set it. A small protocol extension ("join on channel N") would give server-side hidden rooms; not
  needed, possibly worth proposing upstream.
- Status pages show every group's name and its players' nicknames, a private room's too; its name tells nothing.

## 3. Private rooms

### 3.1 How it works
- The player creates a **room code** in the launcher, e.g. `K7QF-29XM` (8 characters of Crockford base32, 40 bits; O
  reads as 0, I and L as 1), or types a friend's. The game starts with it (`--adhoc-room`) or gets it while running from
  the Escape menu (§3.4).
- The control library (`platforms/psp/runtime/src/adhocctl.cpp`) renames every hall the game asks for: game name
  `MHP2Q0NN` → server name `F` + 5 base32 characters of a hash (FNV-1a) of the room code and the game's six-byte prefix
  + the hall's own two characters `NN`. Each of the 24 halls of each prefix gets its own private name, so the game's
  hall list keeps working inside the room: a friend sees "hall 3 occupied" and walks in. The hall number stays readable
  on status pages; the code does not (25 bits of hash cannot give it back).
- In scan results the library translates private names back to the game's and **drops every public group**, so the
  game lists only the room's halls. To translate back it needs the game's prefixes: the game module gives them
  (`adhoc_hall_prefixes`: `MHP2Q0`, `MHP2A0` and the Japanese `集会所` for Freedom Unite, `MHPSP0` for Freedom), and any prefix the game joins
  with is learnt. A hall with an unknown prefix is only missing from the list; joining it still works.
- Connect, disconnect, peer lists and the network dialog use the game's name on the game's side and the private one on
  the server's. PDP, PTP and the relay are untouched.

### 3.2 What it gives and what it does not
- Works on every aemu server, with nothing for the operators to do.
- Strangers cannot see or join the room: their game ignores the name (§1), and the status page's name does not give the
  code.
- Only MHForever players can enter: a PPSSPP or PSP friend cannot type a room code. The public halls stay the default,
  and the launcher says so next to the option.
- **Not a lock.** A modified client could copy a private name from a status page and join that hall; the game itself
  cannot. A room code is privacy for everyday play, not security, and the launcher never calls it "secure".
- A code gives its group its own 24 halls per prefix, four hunters each, as on the PSP: eight friends with one code
  fill two halls, and another group with another code on the same server has 24 different ones.

### 3.3 Sharing a room
The code is typed or pasted (Copy in the launcher and in the Escape menu). A friend's invite (§4.3) carries the server
and the code, so friends on different default servers still meet.

### 3.4 Switching while playing
Private rooms must not cost a player their PPSSPP and PSP friends, so the mode is a switch, not a setting fixed at
start:
- **In the launcher**, Home's "Play with friends" card creates a room, joins one by code, copies the code or goes back
  to the public halls (`adhocRoom` in the settings, passed as `--adhoc-room`). Starting the game normally keeps the last
  choice.
- **In the Escape menu**, the Online tab shows the mode and changes it in real time (`Runtime::set_adhoc_room`). The game
  only asks for hall names when it scans and when it enters a hall, so a change in the village applies at once, and a
  change inside a hall or a quest never disconnects anyone: it applies the next time the player enters a hall. The menu
  says which one happened.
- **The server too:** the Online tab also picks the server among the launcher's (`Runtime::set_adhoc_server`). Another
  server takes over only when that cuts nothing off: no group joined or being joined, no scan under way, no socket open.
  In the village that is at once; in a hall or a quest it waits until the game leaves it.
- The state lives in the game process (the runtime's online state, shared by the control library and the menu), so no
  link to the launcher is needed while playing. The game writes the room and server it ends with (`room=` in the
  controls file) and the launcher keeps them; while the game runs the launcher reads them every 2 s, so friends see a
  change made in the menu.

## 4. Friends

### 4.1 Requirements
No accounts, no servers of MHForever's own, no money; a friend is a friend in every game; presence, invites and joining
in one click; no IP address given to friends or strangers; works behind CG-NAT and on mobile data.

### 4.2 Why Nostr
Nostr relays carry small signed messages over an outgoing WebSocket, the same model as the ad hoc servers: they work
behind any NAT, keep IP addresses between each player and the relay, and need no account (a key pair). The Mainline DHT
of the first design (`08-multiplayer.md` §8) needed live links between friends, so hole punching (about 70 % success)
and each friend seeing the other's IP address. Discord Rich Presence needs a Discord account and is left as an optional
extra (§7).

### 4.3 How it works (`launcher/ui/src/friendNetwork.ts`, the drawer in `Friends.tsx`)
Facts used [verified 2026-10-29, NIP-01, NIP-44, nostr-tools]: events are signed with secp256k1 Schnorr keys (BIP-340);
kinds 20000–29999 are **ephemeral** (relays forward them to subscribers and store nothing); NIP-44 is the versioned
encryption; nostr-tools is public domain and runs in the browser.

- **Identity.** The Nostr key is `HKDF-SHA256(identity seed, info "mhf-nostr")`, derived by the native launcher
  (`nostr_key`), which passes only that key to the UI; the backup file of the seed covers it. The friend code is the
  first 80 bits of `SHA-256(public key)` (`08-multiplayer.md` §4.1).
- **Adding a friend.** Player A turns on "Let friends add me" for 10 minutes; their launcher announces their public key
  in kind 21470 (tag `t` = the first 128 bits of `SHA-256("mhf-friend-code:" + code)`, empty content). Player B types A's
  code, finds the key, checks that it hashes to the code, and sends a request in kind 21471 (tag `p`, NIP-44); A
  accepts the same way. Both store each other's key and name. Both must be online at the same time, like a friend
  search on a console.
- **Who sees the friendship.** For each pair, both launchers derive the NIP-44 conversation key and from it a **pair
  key** for messages from X to Y on UTC day d: `HMAC-SHA256(conversation key, "mhf-pair:d:X:Y")`, listening for days d-1,
  d and d+1. Presence, invites and goodbyes (kind 21472) are signed with the pair key and encrypted with the conversation
  key, so a relay sees random-looking keys, not who is friends with whom, and nobody else can read or forge them.
- **Presence.** While the launcher is open, one event per friend every 60 s and on every change: in the launcher, or
  playing (game, region), the server, the room and its free places. Closing the launcher says goodbye; a launcher that
  dies without it is offline for its friends after three minutes. Nothing is stored.
- **Invites and joining.** Invite sends the player's server and room; Join makes a friend's server and room the
  player's and starts the game. A friend in the player's room reads "In your room" (Join and Invite are then hidden).
- **Checks.** Every event is checked before use: signature, at most 4 KB, created within 5 minutes of now, at most 30 a
  minute per sender, the same event once even when several relays carry it, and each field validated (names cut to 24
  characters without control characters or bidirectional overrides, servers as the launcher checks them, room codes as
  8 Crockford characters). A friend already in the list who asks again (a new computer) gets an answer at once; a
  blocked key is ignored; requests from anyone else are shown only while "Let friends add me" is open (at most 20).
  Removing and blocking are local.
- **Relays.** nos.lol, relay.primal.net, nostr.mom, offchain.pub and relay.nostr.net by default (NIP-11 read
  2026-10-30, all public, each forwarded the ephemeral kinds in a test), editable as `friendRelays` in the settings.
  A list of 20 friends sends about 20 events a minute.
- **In the game.** While the game runs the launcher writes the friends and the invites to `game_friends.txt` (one line
  each: `friend|invite`, key, `offline|launcher|playing`, server, room, name, separated by tabs; lines that do not read
  so are skipped), and the game appends what the player does in the Online tab (`invite <key>`, `dismiss <key>`) to
  `game_friend_actions.txt`, which the launcher takes every 2 s. Join in the game uses the live switch of §3.4. With no
  menu up, a notice in the top right corner says for five seconds when friends come into the player's private room or
  leave it.
- **The Online tab**, top to bottom: Public halls / Private room; Friends (invites, then who is online, five unless
  "Show all"); in a private room only, the current room (code, Copy, New code, Leave), the players in it and where the
  player stands, and Join another room; the server last.

## 5. Security and privacy
- Room codes and pair keys are secrets: never in logs or URLs.
- Everything from a relay is untrusted: strict parsing, size and rate limits, the signature checked before decrypting
  (§4.3). Fuzzing these parsers with the ad hoc ones is still to do (`08-multiplayer.md` §7).
- Relays see the player's IP address and when they are online, as the ad hoc servers do; the launcher says so.
- The game's own packets from strangers remain the larger risk (`08-multiplayer.md` §4.6); private rooms reduce it.

## 6. Verified

| # | What | Result |
|---|---|---|
| R1 | The group names the game scans and joins | Seen on socom.cc, 2026-10-29, for `MHP2Q0`; `MHP2A0` not reached |
| R2 | Two copies with one code meet; a third without it does not see them | Library level against the reference server (3 of 3 runs), then with the game, 2026-10-29: B entered hall 2, A's scan showed exactly that hall under the game's name, A joined and the two exchanged the hall's datagrams |
| R2b | Public → private → public from the Escape menu, in the village and in a hall with a PPSSPP player | Works, 2026-10-29: the connection made stays; after entering again the hall was empty, and back in public the PPSSPP players were there again |
| R5 | Two identities add each other and exchange presence and an invite over the default relays | `launcher/ui/tools/friends-live-check.ts` passes on two networks (2026-10-30); the drawer in a browser against a third |

Not yet: R3 (a private room on a busy public server while PPSSPP players use the public halls), two real launchers on
different networks with mobile data.

## 7. Open work
- **The Japanese bridge:** the same translation can map `集会所NN` and `MHP2Q0NN` to one private name, so Japanese and
  international copies of MHForever share halls; whether the two releases understand each other's data needs two
  copies to test. It would not reach PPSSPP's Japanese players.
- **Invite links** (`mhforever://join?server=…&room=…&game=…`), which need the launcher to register the scheme, offered
  once and removable.
- **Discord Rich Presence** with a Join button, off by default: it needs an application registered by the project, and
  whether Join works without Discord's Social SDK is not known.
- **Hidden groups upstream** (§2).

## 8. Sources (2026-10-29)
- The European executable of Freedom Unite: the hall names and the scan comparison (local research notes).
- Live status pages of the servers in PPSSPP's list (socom.cc, Games Nexus, Neko PSP and others).
- Kethen/aemu_postoffice: `adhocctl/server_cpp/server.cpp` and `client.cpp` (scan, channels, login checks).
- nostr-protocol/nips: NIP-01, NIP-11, NIP-44; nbd-wtf/nostr-tools.
- Discord developer documentation, "Managing Game Invites" and "Setting Rich Presence".
- Ryujinx's LDN guide (passphrases for private local-wireless rooms), the closest precedent for room codes.
