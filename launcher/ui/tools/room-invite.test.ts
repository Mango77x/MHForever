// Run with Node's built-in test runner: node --test tools/room-invite.test.ts
import assert from 'node:assert/strict';
import test from 'node:test';
import { copyEdition, formatInvite, readInvite, sharesHalls } from '../src/roomInvite.ts';
import { serverPorts } from '../src/servers.ts';

test('all supported copies can share their edition in a clipboard invite', () => {
  const releases = {
    Freedom: ['ULES00318', 'ULUS10084', 'ULJM05066'],
    'Freedom-DX': ['ULES00318-FDX', 'ULUS10084-FDX', 'ULJM05066-FDX', 'ULJM05066-FDX-EN'],
    'Freedom-Unite': ['ULES01213', 'ULUS10391'],
    'Portable-2nd-G': ['ULJM05500'],
    FUComplete: ['ULJM05500-FUC'],
  };
  for (const [expected, discs] of Object.entries(releases)) {
    for (const discId of discs) {
      const id = ['Freedom', 'Freedom-DX'].includes(expected) ? 'mhf1' : 'mhfu';
      const edition = copyEdition({ id, discId });
      assert.equal(edition, expected);
      const server = 'lan.local:65534', room = 'K7QF29XM';
      assert.deepEqual(readInvite(formatInvite(server, room, edition), ''), { server, room, edition });
    }
  }
});

test('halls depend on the game and region, while patched editions can share halls', () => {
  assert.ok(sharesHalls('Freedom', 'Freedom-DX'));
  assert.ok(sharesHalls('Portable-2nd-G', 'FUComplete'));
  assert.ok(!sharesHalls('Freedom-Unite', 'Freedom'));
  assert.ok(!sharesHalls('Freedom-Unite', 'Portable-2nd-G'));
  assert.ok(!sharesHalls('Freedom-Unite', undefined));
});

test('unversioned invites and room codes still work', () => {
  const expected = { server: 'host', room: 'K7QF29XM' };
  assert.deepEqual(readInvite('MHForever host K7QF-29XM', ''), expected);
  assert.deepEqual(readInvite(' k7qf-29xm ', 'host'), expected);
  assert.equal(readInvite('k7qf-29xm', ''), null);
});

test('invalid addresses or edition tags cannot select a destination', () => {
  for (const server of ['host:65535', 'host:0', 'https://host', 'host:123456']) {
    assert.equal(readInvite(`MHForever ${server} K7QF-29XM Freedom`, ''), null);
  }
  for (const tag of ['unknown', 'constructor', 'toString', 'Freedom extra']) {
    assert.equal(readInvite(`MHForever host K7QF-29XM ${tag}`, ''), null);
  }
  assert.deepEqual(serverPorts('host:65534'), { host: 'host', control: 65534, relay: 65535 });
});
