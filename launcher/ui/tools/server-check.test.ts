import assert from 'node:assert/strict';
import test from 'node:test';
import { checkServer, suggestedServer } from '../src/serverCheck.ts';

const servers = [{ host: 'first' }, { host: 'second' }, { host: 'third' }];

test('a failed relay keeps the hall timing, including zero milliseconds', async () => {
  const calls: string[] = [];
  const result = await checkServer('lan:65534', async (address) => {
    calls.push(address);
    if (address.endsWith(':65535')) throw new Error('closed');
    return 0;
  });
  assert.deepEqual(calls, ['lan:65534', 'lan:65535']);
  assert.deepEqual(result, { hall: 0, relay: null });
});

test('malformed endpoints never start a probe', async () => {
  const calls: string[] = [];
  const result = await checkServer('lan:65535', async (address) => { calls.push(address); return 1; });
  assert.deepEqual(result, { hall: null, relay: null });
  assert.deepEqual(calls, []);
});

test('a suggestion requires both ports and considers the slower connection', () => {
  assert.equal(suggestedServer(servers, {
    first: { hall: 1, relay: null }, second: { hall: 2, relay: 180 }, third: { hall: 30, relay: 35 },
  }), servers[2]);
  assert.equal(suggestedServer(servers, { first: { hall: null, relay: 10 } }), undefined);
});

test('a tie follows catalog order even if the probes returned in reverse order', () => {
  assert.equal(suggestedServer(servers, {
    second: { hall: 15, relay: 12 }, first: { hall: 9, relay: 15 },
  }), servers[0]);
});
