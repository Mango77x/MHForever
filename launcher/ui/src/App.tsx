import type React from 'react';
import { useCallback, useEffect, useRef, useState } from 'react';
import { AnimatePresence, MotionConfig, motion } from 'motion/react';
import { api, onEvent, selectedGame, type Game, type LauncherEvent, type Settings, type State, type UpdateInfo } from './bridge';
import { AlertIcon, CheckIcon, ChevronIcon, CopyIcon, FriendsIcon, HomeIcon, LibraryIcon, SlidersIcon } from './icons';
import logoUrl from '../../../assets/branding/icon-128.png';
import { FriendsDrawer, useFriends } from './Friends';
import { Home } from './Home';
import { Library } from './Library';
import { GameSettingsPage, SettingsPage } from './SettingsPage';
import { clock, hours } from './format';
import { startNavigation } from './gamepad';

type Screen = 'home' | 'library' | 'settings' | 'game';
type Verify = { phase: 'reading' | 'hashing' | 'preparing' | 'copying' | 'done' | 'failed'; progress: number; file: string; game?: Game; message?: string };

function verifyLabel(phase: Verify['phase'], progress: number): string {
  const percent = `${Math.round(progress * 100)}%`;
  switch (phase) {
    case 'reading': return 'Reading the disc';
    case 'preparing': return 'Preparing the game from your disc';
    case 'copying': return `Copying the disc image · ${percent}`;
    default: return `Comparing with known good dumps · ${percent}`;
  }
}
type Launch = { phase: 'checking' | 'starting' | 'running' | 'exited' | 'failed'; startedAt: number; seconds?: number; message?: string };

const spring = { type: 'spring' as const, stiffness: 420, damping: 32 };
const titles: Record<Screen, string> = { home: 'Home', library: 'Library', settings: 'Settings', game: 'Game settings' };

function Toast({ text }: { text: string | null }) {
  return (
    <AnimatePresence>
      {text && (
        <motion.div
          key={text}
          className="toast"
          role="status"
          initial={{ opacity: 0, y: 30, x: '-50%' }}
          animate={{ opacity: 1, y: 0, x: '-50%' }}
          exit={{ opacity: 0, y: 20, x: '-50%' }}
          transition={spring}
        >
          <CheckIcon size={16} color="#1a1208" /> {text}
        </motion.div>
      )}
    </AnimatePresence>
  );
}

function Steps({ items, current }: { items: string[]; current: number }) {
  return (
    <div className="steps">
      {items.map((label, i) => (
        <div key={label} className={'step' + (i < current ? ' done' : i === current ? ' now' : '')}>
          <span className="b">{i < current && <CheckIcon size={12} color="#1a1208" width={4} />}</span>
          {label}
        </div>
      ))}
    </div>
  );
}

export default function App() {
  const [state, setState] = useState<State | null>(null);
  const [screen, setScreen] = useState<Screen>('home');
  const [friendsOpen, setFriendsOpen] = useState(false);
  const [profileOpen, setProfileOpen] = useState(false);
  const [toast, setToast] = useState<string | null>(null);
  const [verify, setVerify] = useState<Verify | null>(null);
  const [launch, setLaunch] = useState<Launch | null>(null);
  const [now, setNow] = useState(Date.now());
  const [padMode, setPadMode] = useState(false);
  const [update, setUpdate] = useState<UpdateInfo | null>(null);
  const [updating, setUpdating] = useState<{ phase: string; progress: number; message?: string } | null>(null);
  const toastTimer = useRef<number>(0);

  const say = useCallback((text: string) => {
    setToast(null);
    window.clearTimeout(toastTimer.current);
    requestAnimationFrame(() => setToast(text));
    toastTimer.current = window.setTimeout(() => setToast(null), 2400);
  }, []);

  useEffect(() => { api.mhf_getState().then(setState); }, []);
  const friends = useFriends(state, !!launch && (launch.phase === 'starting' || launch.phase === 'running'), say, setState);
  useEffect(() => { api.mhf_updateInfo().then((u) => { if (u) setUpdate(u); }); }, []);
  useEffect(() => startNavigation(setPadMode), []);

  useEffect(() => onEvent((e: LauncherEvent) => {
    switch (e.type) {
      case 'state': setState(e.state); break;
      case 'verify': setVerify({ phase: e.phase, progress: e.progress, file: e.file }); break;
      case 'verified': setVerify((v) => ({ phase: 'done', progress: 1, file: v?.file ?? '', game: e.game })); break;
      case 'verifyFailed': setVerify((v) => ({ phase: 'failed', progress: 0, file: v?.file ?? '', message: e.message })); break;
      case 'launch': setLaunch((l) => ({ phase: e.phase, startedAt: e.phase === 'running' ? Date.now() : l?.startedAt ?? Date.now() })); break;
      case 'exited':
        setLaunch((l) => (l ? { ...l, phase: 'exited', seconds: e.seconds } : l));
        window.setTimeout(() => setLaunch(null), 2600);
        break;
      case 'update': setUpdate({ version: e.version, page: e.page, size: e.size }); break;
      case 'updateProgress': setUpdating({ phase: e.phase, progress: e.progress }); break;
      case 'updateReady': setUpdating({ phase: 'ready', progress: 1 }); break;
      case 'updateFailed': setUpdating({ phase: 'failed', progress: 0, message: e.message }); break;
      case 'launchFailed': setLaunch((l) => ({ phase: 'failed', startedAt: l?.startedAt ?? Date.now(), message: e.message })); break;
    }
  }), []);

  useEffect(() => {
    if (launch?.phase !== 'running') return;
    const t = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(t);
  }, [launch?.phase]);

  // B / Escape closes the topmost layer.
  useEffect(() => {
    const back = () => {
      if (verify && (verify.phase === 'done' || verify.phase === 'failed')) setVerify(null);
      else if (launch && (launch.phase === 'failed' || launch.phase === 'exited')) setLaunch(null);
      else if (profileOpen) setProfileOpen(false);
      else if (friendsOpen) setFriendsOpen(false);
      else if (screen !== 'home') setScreen('home');
    };
    window.addEventListener('mhf-back', back);
    return () => window.removeEventListener('mhf-back', back);
  }, [verify, launch, profileOpen, friendsOpen, screen]);

  if (!state) return <div className="app"><div className="aurora" /></div>;

  const go = (s: Screen) => { setScreen(s); setProfileOpen(false); };
  const setSettings = (s: Partial<Settings>) => {
    setState({ ...state, settings: { ...state.settings, ...s } });
    api.mhf_setSettings(s).then(setState);
  };
  const addCopy = () => { setProfileOpen(false); api.mhf_addGame(); };
  const locateExe = () => api.mhf_pickExecutable().then((s) => { setState(s); if (s.executable.found) say('Executable found'); });
  const copyCode = () => api.mhf_copyText(friends.code).then(() => say('Friend code copied'));
  const play = () => {
    const game = selectedGame(state);
    if (!game) return;
    setLaunch({ phase: 'checking', startedAt: Date.now() });
    api.mhf_play(game.id);
  };
  // A friend's room (or their server's public halls): the game starts there.
  const join = (adhocServer: string, adhocRoom: string) => {
    if (!selectedGame(state)) {
      say('Add your copy of the game first');
      return;
    }
    setFriendsOpen(false);
    api.mhf_setSettings({ adhocServer, adhocRoom }).then((s) => {
      setState(s);
      play();
    });
  };
  const waiting = friends.requests.length + friends.invites.length;
  const online = friends.friends.filter((f) => {
    const p = friends.presence[f.key];
    return p && Date.now() - p.seen < 3 * 60 * 1000;
  }).length;

  const nav: { id: Screen | 'friends'; label: string; icon: React.ReactNode }[] = [
    { id: 'home', label: 'Home', icon: <HomeIcon /> },
    { id: 'library', label: 'Library', icon: <LibraryIcon /> },
    { id: 'friends', label: 'Friends', icon: <FriendsIcon /> },
    { id: 'settings', label: 'Settings', icon: <SlidersIcon /> },
  ];
  const activeNav = friendsOpen ? 'friends' : screen;
  const launchStep = launch ? { checking: 0, starting: 1, running: 3, exited: 3, failed: 0 }[launch.phase] : 0;

  return (
    <MotionConfig reducedMotion="user">
      <div className={'app' + (padMode ? ' pad-mode' : '')}>
        <div className="aurora" aria-hidden="true" />
        <div className="grain" aria-hidden="true" />

        <nav className="rail" aria-label="Main">
          <img className="logo" src={logoUrl} alt="MHForever" width={46} height={46} draggable={false} />
          {nav.map((n) => (
            <button
              key={n.id}
              className={'rail-btn' + (activeNav === n.id ? ' on-text' : '')}
              style={activeNav === n.id ? { color: 'var(--on-accent)' } : undefined}
              onClick={() => (n.id === 'friends' ? (setFriendsOpen(true), setProfileOpen(false)) : go(n.id))}
              aria-label={n.label}
              aria-current={activeNav === n.id ? 'page' : undefined}
            >
              {activeNav === n.id && (
                <motion.span
                  layoutId="rail-active"
                  transition={spring}
                  style={{ position: 'absolute', inset: 0, borderRadius: 16, background: 'var(--accent)', boxShadow: '0 10px 30px -8px var(--accent)' }}
                />
              )}
              <span style={{ position: 'relative', display: 'flex' }}>{n.icon}</span>
              <span className="tip">{n.label}</span>
            </button>
          ))}
        </nav>

        <main className="main">
          <header className="topbar">
            <div className="brand">
              <b>MHForever</b>
              <AnimatePresence mode="wait">
                <motion.span key={screen} initial={{ opacity: 0, y: 6 }} animate={{ opacity: 1, y: 0 }} exit={{ opacity: 0, y: -6 }} transition={{ duration: 0.2 }}>
                  {titles[screen]}
                </motion.span>
              </AnimatePresence>
            </div>
            <div className="top-actions">
              <button className="ghost top friends-btn" onClick={() => { setFriendsOpen(true); setProfileOpen(false); }}>
                <FriendsIcon size={18} /> Friends
                {waiting > 0 ? <span className="badge hot">{waiting}</span> : online > 0 ? <span className="badge live">{online} online</span> : null}
              </button>
              <div style={{ position: 'relative' }}>
                <button className={'profile' + (profileOpen ? ' open' : '')} onClick={() => setProfileOpen(!profileOpen)} aria-label="Your profile" aria-expanded={profileOpen}>
                  <span className="avatar">{state.pseudonym.slice(0, 1).toUpperCase()}<span className="online" /></span>
                  <span className="profile-name"><b>{state.pseudonym}</b><span>Online</span></span>
                  <span className="chev"><ChevronIcon /></span>
                </button>
                <AnimatePresence>
                  {profileOpen && (
                    <motion.div
                      className="pmenu"
                      role="menu"
                      data-nav-scope
                      initial={{ opacity: 0, y: -8, scale: 0.96 }}
                      animate={{ opacity: 1, y: 0, scale: 1 }}
                      exit={{ opacity: 0, y: -6, scale: 0.97 }}
                      transition={spring}
                    >
                      <div style={{ display: 'flex', alignItems: 'center', gap: 14 }}>
                        <span className="avatar" style={{ width: 52, height: 52, borderRadius: 16, fontSize: 20 }}>{state.pseudonym.slice(0, 1).toUpperCase()}</span>
                        <div style={{ display: 'flex', flexDirection: 'column', gap: 3 }}>
                          <span style={{ fontFamily: 'var(--display)', fontWeight: 700, fontSize: 19 }}>{state.pseudonym}</span>
                          <span className="muted" style={{ fontSize: 13 }}>Local identity · no account</span>
                        </div>
                      </div>
                      <span className="eyebrow" style={{ fontSize: 12 }}>Your friend code</span>
                      <div className="code-box">
                        <span className="code">{friends.code}</span>
                        <button className="copy" onClick={copyCode} aria-label="Copy friend code" data-nav-default><CopyIcon /> Copy</button>
                      </div>
                      <div style={{ display: 'flex', flexDirection: 'column', gap: 2, borderTop: '1px solid var(--line)', paddingTop: 8 }}>
                        <button className="menu-item" onClick={() => go('settings')}>Settings</button>
                        <button className="menu-item" onClick={() => { setProfileOpen(false); api.mhf_exportIdentity().then((r) => r.saved && say('Identity backup saved')); }}>Back up my identity</button>
                      </div>
                    </motion.div>
                  )}
                </AnimatePresence>
              </div>
            </div>
          </header>

          {update && !updating && (
            <motion.div className="update-bar" initial={{ opacity: 0, y: -8 }} animate={{ opacity: 1, y: 0 }}>
              <span><b>MHForever {update.version}</b> is available{update.size > 0 ? ` (${Math.round(update.size / 1048576)} MB)` : ''}.</span>
              <button className="ghost mid" onClick={() => {
                setUpdating({ phase: 'downloading', progress: 0 });
                api.mhf_installUpdate().then((r) => {
                  if (!r.started) setUpdating({ phase: 'failed', progress: 0, message: 'The update cannot start while a game is running or a copy is being added.' });
                });
              }}>Update now</button>
            </motion.div>
          )}

          <AnimatePresence mode="wait">
            <motion.div
              key={screen}
              style={{ display: 'flex', flexDirection: 'column', gap: 26 }}
              initial={{ opacity: 0 }}
              animate={{ opacity: 1 }}
              exit={{ opacity: 0, y: -10, filter: 'blur(4px)', transition: { duration: 0.18 } }}
            >
              {screen === 'home' && (
                <Home state={state} onPlay={play} onAddCopy={addCopy} onLocateExecutable={locateExe} onSettings={() => go('game')}
                  onOnlineSettings={() => go('settings')} onRoom={(adhocRoom) => setSettings({ adhocRoom })}
                  onCopy={(text) => api.mhf_copyText(text).then(() => say('Room code copied'))} />
              )}
              {screen === 'library' && <Library games={state.games} onAddCopy={addCopy} selectedDisc={selectedGame(state)?.discId ?? ''} onOpen={(g) => api.mhf_selectGame(g.discId).then((s) => { setState(s); go('home'); })} />}
              {screen === 'settings' && (
                <SettingsPage
                  state={state}
                  onChange={setSettings}
                  onPseudonym={(n) => api.mhf_setPseudonym(n).then((s) => { setState(s); say('Name saved'); })}
                  onCopyCode={copyCode}
                  friendCode={friends.code}
                  onSavesFolder={(backToDefault) => api.mhf_savesFolder(backToDefault ? 'default' : '').then((r) => {
                    setState(r.state);
                    if (r.error) say(r.error);
                    else if (r.picked) say(r.copied ? `Saves folder changed, saves of ${r.copied} game${r.copied === 1 ? '' : 's'} copied` : 'Saves folder changed');
                  })}
                />
              )}
              {screen === 'game' && (
                <GameSettingsPage
                  state={state}
                  onChange={setSettings}
                  onChangeCopy={addCopy}
                  onRemoveCopy={(id) => api.mhf_removeGame(id).then((s) => { setState(s); say('Copy removed from the library'); })}
                  onLocateExecutable={locateExe}
                />
              )}
            </motion.div>
          </AnimatePresence>
        </main>

        {/* Friends drawer */}
        <AnimatePresence>
          {friendsOpen && (
            <>
              <motion.div className="scrim" onClick={() => setFriendsOpen(false)} initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }} aria-hidden="true" />
              <FriendsDrawer model={friends} state={state} playing={!!launch && launch.phase !== 'exited' && launch.phase !== 'failed'}
                onClose={() => setFriendsOpen(false)} onCopy={(text, what) => api.mhf_copyText(text).then(() => say(`${what} copied`))}
                onJoin={join} />
            </>
          )}
        </AnimatePresence>

        {/* Verify a copy */}
        <AnimatePresence>
          {verify && (
            <motion.div className="overlay" role="dialog" aria-label="Adding your copy" data-nav-scope initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
              {verify.phase === 'done' && verify.game ? (
                <>
                  <motion.div className={'badge-big' + (verify.game.verification === 'verified' && verify.game.supported ? '' : ' warn')} initial={{ scale: 0.4, opacity: 0 }} animate={{ scale: 1, opacity: 1 }} transition={{ type: 'spring', stiffness: 260, damping: 16 }}>
                    {verify.game.verification === 'verified' && verify.game.supported ? <CheckIcon size={52} color="#1a1208" width={2.6} /> : <AlertIcon size={50} color="#1a1208" />}
                  </motion.div>
                  <h2>{verify.game.verification === 'verified' ? (verify.game.supported ? 'Copy verified' : 'Recognised, not supported yet') : 'Unverified dump'}</h2>
                  <p className="muted" style={{ margin: 0, maxWidth: 460, lineHeight: 1.55 }}>
                    {verify.game.title} · {verify.game.region} · {verify.game.discId} v{verify.game.version}
                    {verify.game.verification !== 'verified' && verify.game.supported && <><br />This image does not match a known good dump. It may still work; if something looks wrong, make a fresh dump of your disc.</>}
                    {!verify.game.supported && <><br />This region is planned but not playable yet.</>}
                  </p>
                  <div className="row-wrap" style={{ justifyContent: 'center' }}>
                    {verify.game.supported && <button className="play" onClick={() => { setVerify(null); go('home'); }} data-nav-default>Done</button>}
                    {!verify.game.supported && <button className="ghost" onClick={() => setVerify(null)} data-nav-default>Close</button>}
                  </div>
                </>
              ) : verify.phase === 'failed' ? (
                <>
                  <div className="badge-big warn"><AlertIcon size={50} color="#1a1208" /></div>
                  <h2>That file can't be used</h2>
                  <p className="muted" style={{ margin: 0, maxWidth: 460, lineHeight: 1.55 }}>{verify.message}</p>
                  <div className="row-wrap" style={{ justifyContent: 'center' }}>
                    <button className="play" onClick={() => { setVerify(null); addCopy(); }} data-nav-default>Choose another file</button>
                    <button className="ghost" onClick={() => setVerify(null)}>Cancel</button>
                  </div>
                </>
              ) : (
                <>
                  <div className="ring" />
                  <h2>Checking your copy</h2>
                  <div className="progress" aria-hidden="true"><div style={{ width: `${Math.round(verify.progress * 100)}%` }} /></div>
                  <span className="muted" style={{ fontSize: 14 }}>
                    {verify.file} · {verifyLabel(verify.phase, verify.progress)}
                  </span>
                </>
              )}
            </motion.div>
          )}
        </AnimatePresence>

        {/* Launch */}
        <AnimatePresence>
          {launch && (
            <motion.div className="overlay" role="dialog" aria-label="Starting the game" data-nav-scope initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
              {launch.phase === 'failed' ? (
                <>
                  <div className="badge-big warn"><AlertIcon size={50} color="#1a1208" /></div>
                  <h2>The game couldn't start</h2>
                  <p className="muted" style={{ margin: 0, maxWidth: 560, lineHeight: 1.55, whiteSpace: 'pre-line', userSelect: 'text' }}>{launch.message}</p>
                  <button className="ghost" onClick={() => setLaunch(null)} data-nav-default>Close</button>
                </>
              ) : launch.phase === 'running' ? (
                <>
                  <motion.div className="badge-big" initial={{ scale: 0.4 }} animate={{ scale: 1 }} transition={{ type: 'spring', stiffness: 260, damping: 15 }}><CheckIcon size={52} color="#1a1208" width={2.6} /></motion.div>
                  <h2>The game is running</h2>
                  <span className="timer">{clock((now - launch.startedAt) / 1000)}</span>
                  <p className="muted" style={{ margin: 0 }}>Close the game window to come back here.</p>
                </>
              ) : launch.phase === 'exited' ? (
                <>
                  <motion.div className="badge-big" initial={{ scale: 0.4 }} animate={{ scale: 1 }} transition={{ type: 'spring', stiffness: 260, damping: 15 }}><CheckIcon size={52} color="#1a1208" width={2.6} /></motion.div>
                  <h2>Good hunt</h2>
                  <p className="muted" style={{ margin: 0 }}>Session: {hours(launch.seconds ?? 0)}</p>
                </>
              ) : (
                <>
                  <div className="ring" />
                  <h2>Getting ready</h2>
                  <Steps items={['Checking your files', 'Starting the game', 'Opening the window']} current={launchStep} />
                </>
              )}
            </motion.div>
          )}
        </AnimatePresence>

        <Toast text={toast} />
        <AnimatePresence>
          {updating && (
            <motion.div className="overlay" role="dialog" aria-modal="true" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
              <div className="glass update-box">
                <h2>
                  {updating.phase === 'failed' ? 'The update did not work' : updating.phase === 'ready' ? 'Restarting MHForever' : `Updating to ${update?.version ?? 'the new version'}`}
                </h2>
                <p className="muted">
                  {updating.phase === 'downloading' && 'Downloading from GitHub.'}
                  {updating.phase === 'verifying' && 'Checking that the download matches the release.'}
                  {updating.phase === 'installing' && 'Putting the new files in place. Your saves and settings are not touched.'}
                  {updating.phase === 'ready' && 'The new version is installed and opens in a moment.'}
                  {updating.phase === 'failed' && updating.message}
                </p>
                {updating.phase !== 'failed' && (
                  <div className="update-progress"><i style={{ width: `${Math.round((updating.phase === 'downloading' ? updating.progress : 1) * 100)}%` }} /></div>
                )}
                {updating.phase === 'failed' && <button className="ghost mid" onClick={() => setUpdating(null)}>Close</button>}
              </div>
            </motion.div>
          )}
        </AnimatePresence>
        {padMode && (
          <div className="pad-hint" aria-hidden="true"><span><kbd>A</kbd>Select</span><span><kbd>B</kbd>Back</span></div>
        )}
      </div>
    </MotionConfig>
  );
}
