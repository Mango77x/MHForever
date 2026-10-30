// Friends over Nostr (docs/14-rooms-and-friends.md section 4.3): no accounts and no servers of ours. Each launcher has a
// key pair derived from its identity seed; the friend code is a hash of the public key. Public relays carry small signed
// events that they forward and never store (ephemeral kinds), so a friend's presence and invites live only while both
// launchers are open, and nobody sees the other's IP address.
//
// - Adding: the player who is added opens "Let friends add me"; for ten minutes their launcher announces its key under a
//   hash of its friend code. The other player's launcher finds it, checks that the key hashes to the code, and sends a
//   request encrypted to that key; accepting answers the same way.
// - Between friends: every message is signed with a pair key both friends can derive (from the NIP-44 conversation key,
//   per direction and per day) and encrypted with the conversation key. A relay sees random keys that change every day,
//   not who is friends with whom; nobody else can read or forge them.
//
// Self-contained (no imports of the launcher's own modules), so that it can be checked outside the UI.

import { finalizeEvent, getPublicKey, type Event } from 'nostr-tools/pure';
import { SimplePool } from 'nostr-tools/pool';
import { decrypt, encrypt, getConversationKey } from 'nostr-tools/nip44';
import { sha256 } from '@noble/hashes/sha2.js';
import { hmac } from '@noble/hashes/hmac.js';
import { bytesToHex, hexToBytes, utf8ToBytes } from '@noble/hashes/utils.js';

// Ephemeral kinds (20000-29999): relays forward them to subscribers and keep nothing.
export const kindAnnounce = 21470; // "Let friends add me": a key under the hash of its friend code
export const kindRequest = 21471;  // a friend request and its answer, between the players' own keys
export const kindPair = 21472;     // presence, invites, goodbye: between two friends, signed with their pair key

export const addWindowMs = 10 * 60 * 1000; // how long "Let friends add me" lasts, and how long a search for a code does
const announceEveryMs = 30 * 1000;
const presenceEveryMs = 60 * 1000;
export const offlineAfterMs = 3 * 60 * 1000;
const maxAgeSeconds = 5 * 60;   // events older (or further ahead) than this are replays or a broken clock
const maxContent = 4096;        // our events are a few hundred bytes
const maxPerMinute = 30;        // per sender

const alphabet = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'; // Crockford base32, as room codes

// ---------------------------------------------------------------- codes

// The friend code of a public key: the first 80 bits of SHA-256(key), 16 characters (docs/08-multiplayer.md §4.1).
export function friendCodeOf(publicKey: string): string {
  const digest = sha256(hexToBytes(publicKey));
  let bits = 0, acc = 0, code = '';
  for (let i = 0; code.length < 16; ) {
    if (bits < 5) {
      acc = ((acc << 8) | digest[i++]) & 0xffff;
      bits += 8;
    }
    code += alphabet[(acc >> (bits - 5)) & 31];
    bits -= 5;
  }
  return code;
}

// "k7q2-m9xa-4rtd-8ple" -> "K7Q2M9XA4RTD8PLE" (O reads as 0, I and L as 1); '' when the text is not a friend code.
export function friendCode(text: string): string {
  let code = '';
  for (const raw of text) {
    if (raw === '-' || raw === ' ') continue;
    let c = raw.toUpperCase();
    if (c === 'O') c = '0';
    if (c === 'I' || c === 'L') c = '1';
    if (c.length !== 1 || !alphabet.includes(c)) return '';
    code += c;
  }
  return code.length === 16 ? code : '';
}

export const formatFriendCode = (code: string) => (code.length === 16 ? code.match(/.{4}/g)!.join('-') : code);

const codeTag = (code: string) => bytesToHex(sha256(utf8ToBytes('mhf-friend-code:' + code))).slice(0, 32);

const isKey = (text: unknown): text is string => typeof text === 'string' && /^[0-9a-f]{64}$/.test(text);
const isRoom = (text: unknown): text is string => typeof text === 'string' && (text === '' || new RegExp(`^[${alphabet}]{8}$`).test(text));
const isServer = (text: unknown): text is string =>
  typeof text === 'string' && text.length <= 253 && /^([A-Za-z0-9._-]+(:[0-9]{1,5})?)?$/.test(text);
const cleanText = (text: unknown, max: number) =>
  typeof text === 'string' ? Array.from(text.replace(/[\u0000-\u001f\u007f]/g, '')).slice(0, max).join('') : '';

// ---------------------------------------------------------------- what friends tell each other

export interface Friend {
  key: string; // their public key (hex)
  name: string;
  added: number; // unix seconds
}

// What the player is doing, as their friends see it.
export interface Status {
  name: string;
  playing: boolean;  // the game is running (else: in the launcher)
  game: string;      // e.g. "Freedom Unite"
  region: string;    // e.g. "Europe"
  server: string;    // the ad hoc server ("" offline)
  room: string;      // the private room's code ("" the public halls)
}

export interface Presence extends Status {
  seen: number; // Date.now() when it arrived
}

export interface Invite {
  from: string; // the friend's key
  server: string;
  room: string;
  seen: number;
}

export interface Request {
  key: string;
  name: string;
  code: string; // their friend code
}

export type SearchState = 'searching' | 'sent' | 'expired';

export interface FriendsEvents {
  presence(key: string, presence: Presence | null): void; // null: they said goodbye
  request(request: Request): void;                         // someone asks to be the player's friend
  accepted(friend: Friend): void;                          // a request the player sent was accepted
  invite(invite: Invite): void;
  search(code: string, state: SearchState): void;          // a friend code being looked for
  relays(connected: number): void;                         // how many relays answer
}

// ---------------------------------------------------------------- the network

export class FriendsNetwork {
  readonly publicKey: string;
  readonly code: string;

  private readonly secret: Uint8Array;
  private readonly pool: SimplePool;
  private relays: string[];
  private friends = new Map<string, Friend>();
  private blocked = new Set<string>();
  private conversations = new Map<string, Uint8Array>();
  private status: Status | null = null;
  private subs: { close(): void }[] = [];
  private pairAuthors = new Map<string, string>(); // a pair key a friend signs with -> that friend
  private day = -1;
  private seen = new Set<string>();
  private rate = new Map<string, number[]>();
  private announcing = 0; // until when (Date.now())
  private searches = new Map<string, { until: number; sub: { close(): void } }>();
  private outgoing = new Set<string>(); // keys the player asked to be friends with
  private timers: ReturnType<typeof setInterval>[] = [];
  private lastPresence = 0;

  constructor(secretHex: string, relays: string[], private readonly on: FriendsEvents) {
    this.secret = hexToBytes(secretHex);
    this.publicKey = getPublicKey(this.secret);
    this.code = friendCodeOf(this.publicKey);
    this.relays = [...relays];
    this.pool = new SimplePool({ enablePing: true, enableReconnect: true });
  }

  start() {
    this.subscribe();
    this.timers.push(setInterval(() => this.tick(), 5000));
  }

  stop() {
    for (const t of this.timers) clearInterval(t);
    this.timers = [];
    this.sayToAll({ t: 'bye' });
    for (const s of this.subs) s.close();
    for (const s of this.searches.values()) s.sub.close();
    this.subs = [];
    this.searches.clear();
    setTimeout(() => this.pool.destroy(), 1500); // the goodbyes go out first
  }

  setRelays(relays: string[]) {
    if (relays.join(' ') === this.relays.join(' ')) return;
    this.pool.close(this.relays.filter((r) => !relays.includes(r)));
    this.relays = [...relays];
    this.subscribe();
  }

  setFriends(friends: Friend[], blocked: string[]) {
    const before = [...this.friends.keys()].sort().join();
    this.friends = new Map(friends.filter((f) => isKey(f.key)).map((f) => [f.key, f]));
    this.blocked = new Set(blocked);
    const after = [...this.friends.keys()].sort().join();
    if (before !== after) {
      this.subscribe();
      this.sendPresence(true);
    }
  }

  setStatus(status: Status) {
    const changed = JSON.stringify(status) !== JSON.stringify(this.status);
    this.status = status;
    if (changed) this.sendPresence(true);
  }

  // "Let friends add me": announces the player's key under their code for ten minutes.
  openAddWindow(): number {
    this.announcing = Date.now() + addWindowMs;
    this.announce();
    return this.announcing;
  }

  closeAddWindow() { this.announcing = 0; }

  // Looks for the player with this friend code (they have "Let friends add me" open) and sends them a request.
  add(text: string) {
    const code = friendCode(text);
    if (!code || code === this.code || this.searches.has(code)) return;
    const sub = this.pool.subscribeMany(this.relays, { kinds: [kindAnnounce], '#t': [codeTag(code)], since: now() - 60 }, {
      onevent: (e) => {
        if (!this.fresh(e) || friendCodeOf(e.pubkey) !== code || !this.searches.has(code)) return;
        this.searches.get(code)!.sub.close();
        this.searches.delete(code);
        this.outgoing.add(e.pubkey);
        this.request(e.pubkey, { t: 'request', name: this.status?.name ?? '' });
        this.on.search(code, 'sent');
      },
    });
    this.searches.set(code, { until: Date.now() + addWindowMs, sub });
    this.on.search(code, 'searching');
  }

  cancelSearch(code: string) {
    this.searches.get(code)?.sub.close();
    this.searches.delete(code);
  }

  // Answers a request: they become a friend (the caller adds them to the list).
  accept(key: string) {
    this.request(key, { t: 'accept', name: this.status?.name ?? '' });
  }

  invite(key: string, server: string, room: string) {
    this.say(key, { t: 'invite', server, room });
  }

  // ---------------------------------------------------------------- inside

  private tick() {
    const t = Date.now();
    if (this.announcing && t >= this.announcing) this.announcing = 0;
    if (this.announcing && t - this.lastAnnounce >= announceEveryMs) this.announce();
    for (const [code, s] of this.searches) {
      if (t < s.until) continue;
      s.sub.close();
      this.searches.delete(code);
      this.on.search(code, 'expired');
    }
    if (dayNow() !== this.day) this.subscribe(); // the pair keys of the new day
    if (t - this.lastPresence >= presenceEveryMs) this.sendPresence(false);
    let connected = 0;
    for (const [, up] of this.pool.listConnectionStatus()) if (up) ++connected;
    this.on.relays(connected);
  }

  private lastAnnounce = 0;
  private announce() {
    this.lastAnnounce = Date.now();
    this.publish(finalizeEvent({ kind: kindAnnounce, created_at: now(), tags: [['t', codeTag(this.code)]], content: '' }, this.secret));
  }

  private conversation(key: string): Uint8Array {
    let c = this.conversations.get(key);
    if (!c) {
      c = getConversationKey(this.secret, key);
      this.conversations.set(key, c);
    }
    return c;
  }

  // The secret key the messages from `from` to `to` are signed with on `day`; both friends can make it, nobody else.
  private pairSecret(friend: string, from: string, to: string, day: number): Uint8Array {
    let secret = hmac(sha256, this.conversation(friend), utf8ToBytes(`mhf-pair:${day}:${from}:${to}`));
    for (;;) {
      try {
        getPublicKey(secret);
        return secret;
      } catch { // not a valid key (about one in 2^128): hash again
        secret = sha256(secret);
      }
    }
  }

  private subscribe() {
    for (const s of this.subs) s.close();
    this.subs = [];
    this.day = dayNow();
    this.subs.push(this.pool.subscribeMany(this.relays, { kinds: [kindRequest], '#p': [this.publicKey], since: now() - 60 }, {
      onevent: (e) => this.onRequestEvent(e),
    }));
    this.pairAuthors.clear();
    for (const key of this.friends.keys()) {
      for (const d of [this.day - 1, this.day, this.day + 1]) { // clocks a little apart around midnight
        this.pairAuthors.set(getPublicKey(this.pairSecret(key, key, this.publicKey, d)), key);
      }
    }
    if (this.pairAuthors.size > 0) {
      this.subs.push(this.pool.subscribeMany(this.relays, { kinds: [kindPair], authors: [...this.pairAuthors.keys()], since: now() - 60 }, {
        onevent: (e) => this.onPairEvent(e),
      }));
    }
  }

  private fresh(e: Event): boolean {
    if (this.seen.has(e.id)) return false; // the same event from another relay
    this.seen.add(e.id);
    if (this.seen.size > 5000) this.seen = new Set([...this.seen].slice(-2500));
    return Math.abs(e.created_at - now()) <= maxAgeSeconds && e.content.length <= maxContent;
  }

  private allowed(sender: string): boolean {
    const t = Date.now();
    const times = (this.rate.get(sender) ?? []).filter((x) => t - x < 60_000);
    times.push(t);
    this.rate.set(sender, times);
    return times.length <= maxPerMinute;
  }

  private open(e: Event, key: string): Record<string, unknown> | null {
    try {
      const value = JSON.parse(decrypt(e.content, this.conversation(key)));
      return value && typeof value === 'object' && !Array.isArray(value) ? value : null;
    } catch {
      return null;
    }
  }

  private onRequestEvent(e: Event) {
    if (!this.fresh(e) || !isKey(e.pubkey) || this.blocked.has(e.pubkey) || !this.allowed(e.pubkey)) return;
    const m = this.open(e, e.pubkey);
    if (!m) return;
    const name = cleanText(m.name, 24);
    if (m.t === 'request') {
      if (this.friends.has(e.pubkey)) this.accept(e.pubkey); // they lost us (a new computer): just answer
      else this.on.request({ key: e.pubkey, name, code: friendCodeOf(e.pubkey) });
    } else if (m.t === 'accept' && this.outgoing.has(e.pubkey)) {
      this.outgoing.delete(e.pubkey);
      this.on.accepted({ key: e.pubkey, name, added: now() });
    }
  }

  private onPairEvent(e: Event) {
    const key = this.pairAuthors.get(e.pubkey);
    if (!key || !this.fresh(e) || !this.allowed(key)) return;
    const m = this.open(e, key);
    if (!m) return;
    if (m.t === 'presence') {
      if (!isServer(m.server) || !isRoom(m.room)) return;
      this.on.presence(key, {
        name: cleanText(m.name, 24), playing: m.playing === true, game: cleanText(m.game, 40), region: cleanText(m.region, 24),
        server: m.server, room: m.room, seen: Date.now(),
      });
    } else if (m.t === 'invite') {
      if (!isServer(m.server) || m.server === '' || !isRoom(m.room)) return;
      this.on.invite({ from: key, server: m.server, room: m.room, seen: Date.now() });
    } else if (m.t === 'bye') {
      this.on.presence(key, null);
    }
  }

  private request(key: string, message: Record<string, unknown>) {
    const content = encrypt(JSON.stringify(message), this.conversation(key));
    this.publish(finalizeEvent({ kind: kindRequest, created_at: now(), tags: [['p', key]], content }, this.secret));
  }

  private say(key: string, message: Record<string, unknown>) {
    const secret = this.pairSecret(key, this.publicKey, key, dayNow());
    const content = encrypt(JSON.stringify(message), this.conversation(key));
    this.publish(finalizeEvent({ kind: kindPair, created_at: now(), tags: [], content }, secret));
  }

  private sayToAll(message: Record<string, unknown>) {
    for (const key of this.friends.keys()) this.say(key, message);
  }

  private sendPresence(force: boolean) {
    if (!this.status || (!force && Date.now() - this.lastPresence < presenceEveryMs)) return;
    this.lastPresence = Date.now();
    this.sayToAll({ t: 'presence', ...this.status });
  }

  private publish(e: Event) {
    for (const p of this.pool.publish(this.relays, e)) p.catch(() => {}); // a relay that refuses or is down: the others carry it
  }
}

const now = () => Math.floor(Date.now() / 1000);
const dayNow = () => Math.floor(Date.now() / 86_400_000);
