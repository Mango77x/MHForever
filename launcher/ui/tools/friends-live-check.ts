// A check of the friends network on the real public relays (docs/14-rooms-and-friends.md R5), outside the UI: two
// launchers with fresh keys add each other by code, see each other's presence and send an invite. It uses the network,
// so it is run by hand, never in CI:
//
//   node --experimental-transform-types tools/friends-live-check.ts [wss://relay ...]

import { generateSecretKey } from 'nostr-tools/pure';
import { bytesToHex } from '@noble/hashes/utils.js';
import { FriendsNetwork, type Friend, type FriendsEvents } from '../src/friendNetwork.ts';

const relays = process.argv.slice(2).length > 0
  ? process.argv.slice(2)
  : ['wss://nos.lol', 'wss://relay.primal.net', 'wss://nostr.mom', 'wss://offchain.pub', 'wss://relay.nostr.net'];

type Waiter = { what: string; ok: (value: unknown) => boolean; resolve: (value: unknown) => void };
function listener(name: string) {
  const waiting: Waiter[] = [];
  const fire = (what: string, value: unknown) => {
    console.log(`  ${name}: ${what}`);
    for (const w of waiting.filter((x) => x.what === what && x.ok(value))) w.resolve(value);
  };
  const events: FriendsEvents = {
    presence: (key, p) => fire(p ? 'presence' : 'bye', { key, p }),
    request: (r) => fire('request', r),
    accepted: (f) => fire('accepted', f),
    invite: (i) => fire('invite', i),
    search: (code, state) => fire('search ' + state, code),
    relays: () => {},
  };
  const wait = (what: string, ok: (value: unknown) => boolean = () => true, ms = 30_000) =>
    new Promise((resolve, reject) => {
      waiting.push({ what, ok, resolve });
      setTimeout(() => reject(new Error(`${name}: no "${what}" within ${ms / 1000} s`)), ms);
    });
  return { events, wait };
}

const a = listener('Alice'), b = listener('Bob');
const alice = new FriendsNetwork(bytesToHex(generateSecretKey()), relays, a.events);
const bob = new FriendsNetwork(bytesToHex(generateSecretKey()), relays, b.events);
const status = (name: string) => ({ name, playing: false, game: 'Freedom Unite', region: 'Europe', server: 'socom.cc', room: '' });

let failed = false;
try {
  alice.start();
  bob.start();
  alice.setStatus(status('Alice'));
  bob.setStatus(status('Bob'));
  await new Promise((r) => setTimeout(r, 2000)); // the relays connect

  console.log('1. Alice lets friends add her; Bob adds her by code');
  alice.openAddWindow();
  const requested = a.wait('request');
  bob.add(alice.code);
  await b.wait('search sent');
  const request = (await requested) as { key: string; name: string };
  if (request.key !== bob.publicKey || request.name !== 'Bob') throw new Error('the request is not from Bob');

  console.log('2. Alice accepts; both keep each other');
  const accepted = b.wait('accepted');
  alice.accept(request.key);
  const friend = (await accepted) as Friend;
  if (friend.key !== alice.publicKey || friend.name !== 'Alice') throw new Error('the answer is not from Alice');
  alice.setFriends([{ key: bob.publicKey, name: 'Bob', added: 0 }], []);
  bob.setFriends([friend], []);

  console.log('3. Presence: Alice starts playing in a private room');
  const seen = b.wait('presence', (v) => {
    const { p } = v as { p: { playing: boolean; room: string } };
    return p.playing && p.room === 'K7QF29XM';
  });
  alice.setStatus({ ...status('Alice'), playing: true, room: 'K7QF29XM' });
  await seen;

  console.log('4. Bob invites Alice');
  const invited = a.wait('invite');
  bob.invite(alice.publicKey, 'socom.cc', 'AB12CD34');
  const invite = (await invited) as { from: string; room: string };
  if (invite.from !== bob.publicKey || invite.room !== 'AB12CD34') throw new Error('the invite is wrong');

  console.log('PASSED');
} catch (e) {
  failed = true;
  console.log('FAILED:', (e as Error).message);
} finally {
  alice.stop();
  bob.stop();
  setTimeout(() => process.exit(failed ? 1 : 0), 2000);
}
