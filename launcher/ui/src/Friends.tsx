// The Friends drawer and the friends network behind it (docs/14-rooms-and-friends.md section 4.3; the protocol is in
// friendNetwork.ts): your friend code, adding a friend by code, requests, who is online and where, invites and joining.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { AnimatePresence, motion } from 'motion/react';
import { api, selectedGame, type State } from './bridge';
import { FriendsNetwork, formatFriendCode, friendCode, friendCodeOf, offlineAfterMs, type Friend, type Invite, type Presence, type Request, type SearchState } from './friendNetwork';
import { formatRoomCode } from './rooms';
import { adhocServers } from './servers';
import { CloseIcon, CopyIcon, FriendsIcon } from './icons';

const serverName = (host: string) => adhocServers.find((s) => s.host === host)?.name ?? host;

// Where a friend is, in a few words.
function where(p: Presence | undefined, now: number): { online: boolean; text: string } {
  if (!p || now - p.seen > offlineAfterMs) return { online: false, text: 'Offline' };
  if (!p.playing) return { online: true, text: 'In the launcher' };
  if (p.server === '') return { online: true, text: `Playing ${p.game || 'Freedom Unite'} offline` };
  return { online: true, text: `Playing · ${p.room !== '' ? 'private room' : 'public halls'} on ${serverName(p.server)}` };
}

export interface FriendsModel {
  ready: boolean;
  code: string; // the player's friend code, XXXX-XXXX-XXXX-XXXX
  friends: Friend[];
  presence: Record<string, Presence>;
  requests: Request[];
  invites: Invite[];
  searches: Record<string, SearchState>;
  relays: number; // how many answer
  addUntil: number; // "Let friends add me" is open until then (Date.now()), 0 closed
  openAddWindow(): void;
  closeAddWindow(): void;
  add(code: string): void;
  cancelSearch(code: string): void;
  accept(r: Request): void;
  decline(r: Request, block: boolean): void;
  remove(key: string): void;
  invite(key: string): void;
  dismissInvite(i: Invite): void;
}

// Runs the friends network while the launcher is open, and keeps what the drawer shows.
export function useFriends(state: State | null, playing: boolean, say: (text: string) => void, onState: (state: State) => void): FriendsModel {
  const net = useRef<FriendsNetwork | null>(null);
  const [presence, setPresence] = useState<Record<string, Presence>>({});
  const [requests, setRequests] = useState<Request[]>([]);
  const [invites, setInvites] = useState<Invite[]>([]);
  const [searches, setSearches] = useState<Record<string, SearchState>>({});
  const [relays, setRelays] = useState(0);
  const [addUntil, setAddUntil] = useState(0);
  const [code, setCode] = useState('');
  const friendsRef = useRef<Friend[]>([]);
  const blockedRef = useRef<string[]>([]);
  friendsRef.current = state?.friends ?? [];
  blockedRef.current = state?.blocked ?? [];
  const sayRef = useRef(say);
  sayRef.current = say;

  const onStateRef = useRef(onState);
  onStateRef.current = onState;
  const save = useCallback((friends: Friend[], blocked: string[]) => {
    friendsRef.current = friends; // a second change before the answer builds on this one
    blockedRef.current = blocked;
    api.mhf_setFriends(friends, blocked).then((s) => onStateRef.current(s));
  }, []);

  const key = state?.nostrKey ?? '';
  useEffect(() => {
    if (!key) return;
    const n = new FriendsNetwork(key, [], {
      presence: (k, p) => setPresence((all) => {
        const next = { ...all };
        if (p) next[k] = p;
        else delete next[k];
        return next;
      }),
      request: (r) => setRequests((all) => (all.some((x) => x.key === r.key) ? all : [...all, r])),
      accepted: (f) => {
        setSearches((all) => { const next = { ...all }; delete next[friendCodeOf(f.key)]; return next; });
        if (!friendsRef.current.some((x) => x.key === f.key)) save([...friendsRef.current, f], blockedRef.current);
        sayRef.current(`${f.name || 'Your friend'} accepted your request`);
      },
      invite: (i) => {
        setInvites((all) => [...all.filter((x) => x.from !== i.from), i]);
        const name = friendsRef.current.find((f) => f.key === i.from)?.name ?? 'A friend';
        sayRef.current(`${name} invites you to play`);
      },
      search: (code, s) => setSearches((all) => ({ ...all, [code]: s })),
      relays: setRelays,
    });
    net.current = n;
    setCode(formatFriendCode(n.code));
    n.start();
    return () => {
      n.stop();
      net.current = null;
    };
  }, [key, save]);

  const relayList = state?.settings.friendRelays.join(' ') ?? '';
  useEffect(() => { net.current?.setRelays(relayList ? relayList.split(' ') : []); }, [relayList, key]);
  const friendList = JSON.stringify(state?.friends ?? []) + JSON.stringify(state?.blocked ?? []);
  useEffect(() => { net.current?.setFriends(friendsRef.current, blockedRef.current); }, [friendList, key]);

  const game = state ? selectedGame(state) : undefined;
  const status = state && {
    name: state.pseudonym,
    playing,
    game: game?.title ?? 'Freedom Unite',
    region: game?.region ?? '',
    server: state.settings.adhocServer,
    room: state.settings.adhocServer ? state.settings.adhocRoom : '',
  };
  const statusText = JSON.stringify(status);
  useEffect(() => { if (status) net.current?.setStatus(status); }, [statusText, key]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (!addUntil) return;
    const t = window.setTimeout(() => setAddUntil(0), Math.max(0, addUntil - Date.now()));
    return () => window.clearTimeout(t);
  }, [addUntil]);

  return {
    ready: code !== '',
    code,
    friends: state?.friends ?? [],
    presence,
    requests,
    invites,
    searches,
    relays,
    addUntil,
    openAddWindow: () => { if (net.current) setAddUntil(net.current.openAddWindow()); },
    closeAddWindow: () => { net.current?.closeAddWindow(); setAddUntil(0); },
    add: (text) => net.current?.add(text),
    cancelSearch: (code) => {
      net.current?.cancelSearch(code);
      setSearches((all) => { const next = { ...all }; delete next[code]; return next; });
    },
    accept: (r) => {
      net.current?.accept(r.key);
      setRequests((all) => all.filter((x) => x.key !== r.key));
      if (!friendsRef.current.some((x) => x.key === r.key)) {
        save([...friendsRef.current, { key: r.key, name: r.name, added: Math.floor(Date.now() / 1000) }], blockedRef.current);
      }
    },
    decline: (r, block) => {
      setRequests((all) => all.filter((x) => x.key !== r.key));
      if (block) save(friendsRef.current, [...blockedRef.current, r.key]);
    },
    remove: (k) => save(friendsRef.current.filter((f) => f.key !== k), blockedRef.current),
    invite: (k) => {
      if (!state?.settings.adhocServer) return;
      net.current?.invite(k, state.settings.adhocServer, state.settings.adhocRoom);
    },
    dismissInvite: (i) => setInvites((all) => all.filter((x) => x !== i)),
  };
}

function Countdown({ until }: { until: number }) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(t);
  }, []);
  const left = Math.max(0, Math.round((until - now) / 1000));
  return <>{Math.floor(left / 60)}:{String(left % 60).padStart(2, '0')}</>;
}

export function FriendsDrawer({ model, state, playing, onClose, onCopy, onJoin }: {
  model: FriendsModel;
  state: State;
  playing: boolean;
  onClose: () => void;
  onCopy: (text: string, what: string) => void;
  onJoin: (server: string, room: string) => void;
}) {
  const [typed, setTyped] = useState('');
  const [menu, setMenu] = useState<string | null>(null); // the friend whose "Remove" is asked
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = window.setInterval(() => setNow(Date.now()), 15_000);
    return () => window.clearInterval(t);
  }, []);
  const validCode = friendCode(typed);
  const ownCode = validCode !== '' && formatFriendCode(validCode) === model.code;
  const sorted = useMemo(() => [...model.friends].sort((a, b) => {
    const pa = where(model.presence[a.key], now).online ? 0 : 1, pb = where(model.presence[b.key], now).online ? 0 : 1;
    return pa - pb || a.name.localeCompare(b.name);
  }), [model.friends, model.presence, now]);
  const nameOf = (key: string) => model.friends.find((f) => f.key === key)?.name || 'A friend';
  const server = state.settings.adhocServer;
  const relayCount = state.settings.friendRelays.length;

  const searching = Object.entries(model.searches);
  return (
    <motion.aside className="drawer friends" aria-label="Friends" data-nav-scope initial={{ x: '105%' }} animate={{ x: 0 }} exit={{ x: '105%' }}
      transition={{ type: 'spring', stiffness: 300, damping: 34 }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
        <h2>Friends</h2>
        <button className="rail-btn" onClick={onClose} aria-label="Close" style={{ width: 44, height: 44 }}><CloseIcon /></button>
      </div>

      <AnimatePresence initial={false}>
        {model.invites.map((i) => (
          <motion.div key={i.from + i.seen} className="f-card invite" layout initial={{ opacity: 0, y: -8 }} animate={{ opacity: 1, y: 0 }} exit={{ opacity: 0 }}>
            <div className="f-text">
              <b>{nameOf(i.from)} invites you</b>
              <span>{i.room ? `Private room ${formatRoomCode(i.room)}` : 'Public halls'} on {serverName(i.server)}</span>
            </div>
            <div className="f-actions">
              <button className="play small" disabled={playing} title={playing ? 'Close the game first' : undefined}
                onClick={() => { model.dismissInvite(i); onJoin(i.server, i.room); }}>Join</button>
              <button className="ghost f-btn" onClick={() => model.dismissInvite(i)}>Not now</button>
            </div>
          </motion.div>
        ))}
        {model.requests.map((r) => (
          <motion.div key={r.key} className="f-card" layout initial={{ opacity: 0, y: -8 }} animate={{ opacity: 1, y: 0 }} exit={{ opacity: 0 }}>
            <div className="f-text">
              <b>{r.name || 'Someone'} wants to be your friend</b>
              <span className="code">{formatFriendCode(r.code)}</span>
            </div>
            <div className="f-actions">
              <button className="play small" onClick={() => model.accept(r)}>Accept</button>
              <button className="ghost f-btn" onClick={() => model.decline(r, false)}>Decline</button>
              <button className="ghost f-btn danger" onClick={() => model.decline(r, true)}>Block</button>
            </div>
          </motion.div>
        ))}
      </AnimatePresence>

      <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
        <span className="eyebrow">Your friend code</span>
        <div className="code-box">
          <span className="code">{model.code || '…'}</span>
          <button className="copy" onClick={() => onCopy(model.code, 'Friend code')} aria-label="Copy friend code" data-nav-default><CopyIcon /> Copy</button>
        </div>
        {model.addUntil ? (
          <div className="f-note on">
            <span>Friends can add you with your code for <b><Countdown until={model.addUntil} /></b>.</span>
            <button className="ghost f-btn" onClick={model.closeAddWindow}>Stop</button>
          </div>
        ) : (
          <button className="ghost mid" onClick={model.openAddWindow} disabled={!model.ready}>Let friends add me</button>
        )}
      </div>

      <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
        <label htmlFor="add-code" className="eyebrow">Add by code</label>
        <form style={{ display: 'flex', gap: 8 }} onSubmit={(e) => { e.preventDefault(); if (validCode && !ownCode) { model.add(validCode); setTyped(''); } }}>
          <input id="add-code" className="text-input code" placeholder="XXXX-XXXX-XXXX-XXXX" value={typed} maxLength={24} spellCheck={false}
            onChange={(e) => setTyped(e.target.value)} style={{ flex: 1, minWidth: 0 }} />
          <button className="play small" type="submit" disabled={!validCode || ownCode}>Add</button>
        </form>
        <span className="muted f-hint">{ownCode ? 'That is your own code.' : 'Your friend presses "Let friends add me" first, then you add their code.'}</span>
        {searching.map(([code, s]) => (
          <div key={code} className="f-note">
            <span>
              <span className="code">{formatFriendCode(code)}</span>{' · '}
              {s === 'searching' ? 'Looking for them…' : s === 'sent' ? 'Request sent, waiting for them to accept' : 'Not found: ask them to press "Let friends add me"'}
            </span>
            <button className="ghost f-btn" onClick={() => model.cancelSearch(code)}>{s === 'searching' ? 'Cancel' : 'OK'}</button>
          </div>
        ))}
      </div>

      <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
        <span className="eyebrow">{model.friends.length ? `Friends · ${model.friends.length}` : 'Friends'}</span>
        {sorted.length === 0 && (
          <div className="empty">
            <FriendsIcon size={34} color="var(--accent)" />
            <b>No friends yet</b>
            Add a friend by their code. You see when they play and where, and you can join them or invite them to your room.
          </div>
        )}
        {sorted.map((f) => {
          const p = model.presence[f.key];
          const w = where(p, now);
          const canJoin = w.online && !!p && p.playing && p.server !== '';
          return (
            <div key={f.key} className={'f-row' + (w.online ? ' online' : '')}>
              <span className="avatar">{(f.name || '?').slice(0, 1).toUpperCase()}{w.online && <span className="online" />}</span>
              <div className="f-text">
                <b>{f.name || 'Friend'}</b>
                <span>{w.text}</span>
              </div>
              <div className="f-actions">
                {menu === f.key ? (
                  <>
                    <button className="ghost f-btn danger" onClick={() => { model.remove(f.key); setMenu(null); }}>Remove</button>
                    <button className="ghost f-btn" onClick={() => setMenu(null)}>Keep</button>
                  </>
                ) : (
                  <>
                    {canJoin && <button className="play small" disabled={playing} title={playing ? 'Close the game first' : `Join them on ${serverName(p!.server)}`}
                      onClick={() => onJoin(p!.server, p!.room)}>Join</button>}
                    {w.online && server !== '' && (
                      <button className="ghost f-btn" title={`Send them your ${state.settings.adhocRoom ? 'room' : 'server'}`} onClick={() => model.invite(f.key)}>Invite</button>
                    )}
                    <button className="ghost f-btn more" aria-label={`More for ${f.name}`} onClick={() => setMenu(f.key)}>···</button>
                  </>
                )}
              </div>
            </div>
          );
        })}
      </div>

      <p className="muted f-hint" style={{ marginTop: 'auto' }}>
        {model.relays > 0 ? `Connected to ${model.relays} of ${relayCount} relays.` : 'Connecting to the relays…'} Friends meet through public
        relays: no account, and your friends never see your IP address. The relays do, and when you are online, as game servers do.
      </p>
    </motion.aside>
  );
}
