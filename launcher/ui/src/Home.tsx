import { useState } from 'react';
import { motion } from 'motion/react';
import { selectedGame, type State } from './bridge';
import { HeroScenery, gameAccent, heroBorderColor, heroSky, useImageAccent } from './GameArt';
import { useGameArt } from './gameArtCache';
import { AlertIcon, CheckIcon, FolderIcon, PlayIcon, PlusIcon, SlidersIcon } from './icons';
import { hours, lastPlayed, resolutions } from './format';
import { formatRoomCode, newRoomCode } from './rooms';
import { copyEdition, editionName, formatInvite, readInvite, sharesHalls } from './roomInvite';

type Props = {
  state: State;
  onPlay: () => void;
  onAddCopy: () => void;
  onLocateExecutable: () => void;
  onSettings: () => void;
  onOnlineSettings: () => void;
  onRoom: (code: string, server?: string) => void; // '' for the public halls
  onSelectCopy: (discId: string) => Promise<void>;
  onCopy: (text: string, label: string) => void;
};

const rise = (delay: number) => ({
  initial: { opacity: 0, y: 18, filter: 'blur(6px)' },
  animate: { opacity: 1, y: 0, filter: 'blur(0px)' },
  transition: { duration: 0.6, delay, ease: [0.22, 1, 0.36, 1] as const },
});

export function Hero({ state, onPlay, onAddCopy, onLocateExecutable, onSettings }: Omit<Props, 'onOnlineSettings' | 'onRoom' | 'onCopy' | 'onSelectCopy'>) {
  const [m, setM] = useState({ x: 0, y: 0 });
  const game = selectedGame(state);
  const useCustomArt = state.settings.useCustomArt ?? true;
  const banner = useGameArt(game?.discId, 'banner', Boolean(game?.hasBanner) && useCustomArt, game?.bannerVersion);
  const bannerAccent = useImageAccent(banner);
  const iconAccent = useImageAccent(game?.iconUrl);
  const accent = gameAccent(game, bannerAccent, iconAccent);
  const border = heroBorderColor(game, accent);
  const layer = (k: number, s: number) => ({ transform: `translate3d(${m.x * -k}px, ${m.y * -k * 0.45}px, 0) scale(${s})` });
  return (
    <motion.section
      className="hero"
      {...rise(0)}
      style={{ background: heroSky(game, accent), borderColor: border }}
      onMouseMove={(e) => {
        const r = e.currentTarget.getBoundingClientRect();
        setM({ x: (e.clientX - r.left) / r.width - 0.5, y: (e.clientY - r.top) / r.height - 0.5 });
      }}
      onMouseLeave={() => setM({ x: 0, y: 0 })}
    >
      <HeroScenery game={game} layer={layer} accent={accent} />
      {banner && (
        <img
          className="hero-image"
          src={banner}
          alt=""
          draggable={false}
          decoding="async"
          onLoad={(e) => { e.currentTarget.hidden = false; }}
          onError={(e) => { e.currentTarget.hidden = true; }}
        />
      )}
      <div className="shade" aria-hidden="true" />
      <motion.div className="hero-body" {...rise(0.08)}>
        {game ? (
          <>
            <div className="row-wrap" style={{ gap: 8 }}>
              {game.verification === 'verified' ? (
                <span className="chip"><CheckIcon color="#6fd18b" /> Copy verified</span>
              ) : (
                <span className="chip warn"><AlertIcon size={14} /> Unverified dump</span>
              )}
              <span className="chip">{game.region} · {game.discId} · v{game.version}</span>
              <span className="chip">Native · {resolutions[state.settings.scale - 1]}</span>
            </div>
            <h1>{game.title}</h1>
            <p>Your own copy, running natively on PC. No emulator.</p>
            {!state.executable.found && (
              <div className="notice">
                <AlertIcon color="#f2c14e" />
                <span>Developer build: the game's decrypted executable was not found.</span>
              </div>
            )}
            <div className="row-wrap" style={{ marginTop: 6 }}>
              {state.executable.found ? (
                <button className="play" onClick={onPlay} data-nav-default>
                  <PlayIcon color="#1a1208" /> Play
                </button>
              ) : (
                <button className="play" onClick={onLocateExecutable} data-nav-default>
                  <FolderIcon color="#1a1208" /> Locate executable
                </button>
              )}
              <button className="ghost" onClick={onSettings}>
                <SlidersIcon size={18} /> Game settings
              </button>
            </div>
          </>
        ) : (
          <>
            <span className="chip" style={{ alignSelf: 'flex-start' }}>Welcome</span>
            <h1>Bring your game.</h1>
            <p>Add your own copy of Freedom or Freedom Unite and play it natively on PC. Nothing is downloaded and nothing leaves your computer.</p>
            <div className="row-wrap" style={{ marginTop: 6 }}>
              <button className="play" onClick={onAddCopy} data-nav-default>
                <PlusIcon size={22} color="#1a1208" /> Add my copy
              </button>
            </div>
          </>
        )}
      </motion.div>
    </motion.section>
  );
}

// Private rooms (docs/14-rooms-and-friends.md section 3): the game's Gathering Hall for the players who share a code, on
// the server chosen in Settings → Online. The game can switch too, from its Escape menu (Online tab).
function RoomCard({ state, onOnlineSettings, onRoom, onCopy, onSelectCopy, onAddCopy }: Pick<Props, 'state' | 'onOnlineSettings' | 'onRoom' | 'onCopy' | 'onSelectCopy' | 'onAddCopy'>) {
  const [joining, setJoining] = useState(false);
  const [typed, setTyped] = useState('');
  const server = state.settings.adhocServer ?? '';
  const room = state.settings.adhocRoom ?? '';
  const text = (body: string) => <p style={{ margin: 0, color: 'var(--ink-2)', fontSize: 15, lineHeight: 1.55 }}>{body}</p>;
  const game = selectedGame(state);
  const invite = readInvite(typed, server);
  const localEdition = copyEdition(game);
  const requested = invite?.edition;
  const compatible = !requested || sharesHalls(requested, localEdition);
  const installed = state.games.filter((g) => g.supported);
  const exactCopy = requested && requested !== localEdition
    ? installed.find((g) => copyEdition(g) === requested)
    : undefined;
  const alternative = exactCopy ?? (requested && !compatible
    ? installed.find((g) => sharesHalls(requested, copyEdition(g)))
    : undefined);
  const [switching, setSwitching] = useState(false);
  const [switchError, setSwitchError] = useState(false);
  const selectCopy = async (discId: string) => {
    setSwitching(true);
    setSwitchError(false);
    try {
      await onSelectCopy(discId);
    } catch {
      setSwitchError(true);
    } finally {
      setSwitching(false);
    }
  };
  const [revealedRoom, reveal] = useState('');
  const cancel = () => {
    setSwitchError(false);
    setJoining(false);
    setTyped('');
  };
  const join = () => {
    if (!invite || !compatible || switching) return;
    reveal('');
    onRoom(invite.room, invite.server);
    setSwitchError(false);
    setJoining(false);
    setTyped('');
  };
  return (
    <div className="glass lift card">
      <span className="eyebrow">Play with friends</span>
      {joining ? (
        <>
          {text('Paste an MHForever invite, or a room code if you already selected the same server.')}
          <input
            className="text-input"
            value={typed}
            placeholder="K7QF-29XM"
            aria-label="Room code or invitation"
            autoComplete="off"
            maxLength={300}
            aria-invalid={typed.trim() !== '' && !invite}
            spellCheck={false}
            autoFocus
            disabled={switching}
            onChange={(e) => { setTyped(e.target.value); setSwitchError(false); }}
            onKeyDown={(e) => { if (e.key === 'Enter') join(); if (e.key === 'Escape') cancel(); }}
          />
          {invite && <div className="invite-preview" aria-live="polite">
            <dl>
              <dt>Server</dt><dd>{invite.server}</dd>
              <dt>Host edition</dt><dd>{requested ? editionName(requested) : 'Unspecified · check with your friend'}</dd>
              {requested && <><dt>Your copy</dt><dd>{game ? `${game.title} · ${game.region}` : 'No game selected'}</dd></>}
            </dl>
            {requested && !compatible && <p>{game ? 'These copies use different halls.' : 'Add a compatible copy to join.'}</p>}
            {requested && compatible && requested !== localEdition && <p>These editions share halls, but their quests can differ.</p>}
            {alternative && <button className="ghost mid" disabled={switching} onClick={() => selectCopy(alternative.discId)}>
              Use my {alternative.title} · {alternative.region}
            </button>}
            {requested && !compatible && !alternative && <button className="ghost mid" onClick={onAddCopy}>Add a compatible copy</button>}
            {switchError && <p role="alert">Could not select that copy. Try again.</p>}
          </div>}
          {typed.trim() && !invite && text(server ? 'Enter an eight-character room code or paste the full invite.' : 'Offline. Paste the full invite or choose a server first.')}
          <div className="row-wrap" style={{ marginTop: 'auto', gap: 10 }}>
            <button className="ghost mid" onClick={join} disabled={!invite || !compatible || switching}>Join room</button>
            <button className="ghost mid" onClick={cancel} disabled={switching}>Cancel</button>
          </div>
        </>
      ) : server === '' ? (
        <>
          {text('Choose a server to create a room, or paste a friend\'s invite to use their server and room.')}
          <div className="row-wrap" style={{ marginTop: 'auto', gap: 10 }}>
            <button className="ghost mid" onClick={onOnlineSettings}>Choose a server</button>
            <button className="ghost mid" onClick={() => setJoining(true)}>Paste invite</button>
          </div>
        </>
      ) : room !== '' ? (
        <>
          <span className="stat" style={{ letterSpacing: '0.08em' }}>{revealedRoom === room ? formatRoomCode(room) : '••••-••••'}</span>
          {text(`Private room on ${server}. ${state.settings.shareFriendLocation === false ? 'Your location is hidden from friends.' : 'Your friends can see this location.'} Share an invite to meet in the same Gathering Hall.`)}
          <div className="row-wrap" style={{ marginTop: 'auto', gap: 10 }}>
            <button className="ghost mid" onClick={() => reveal(revealedRoom === room ? '' : room)}>{revealedRoom === room ? 'Hide code' : 'Show code'}</button>
            <button className="ghost mid" onClick={() => onCopy(formatInvite(server, room, localEdition), 'Room invite')}>Copy invite</button>
            <button className="ghost mid" onClick={() => onCopy(formatRoomCode(room), 'Room code')}>Copy code</button>
            <button className="ghost mid" onClick={() => { reveal(''); onRoom(''); }}>Back to public halls</button>
          </div>
        </>
      ) : (
        <>
          {text(`Public halls on ${server}, shared with PSP and PPSSPP players. Create a room to play only with friends.`)}
          <div className="row-wrap" style={{ marginTop: 'auto', gap: 10 }}>
            <button className="ghost mid" onClick={() => onRoom(newRoomCode())}>Create room</button>
            <button className="ghost mid" onClick={() => setJoining(true)}>Join with code or invite</button>
          </div>
        </>
      )}
      {server && !joining && game?.id === 'mhf1' && text('In Freedom, talk to the village elder to unlock the Online Gathering Hall. Join the same quest party to see each other’s hunters.')}
      {server && !joining && game?.id === 'mhfu' && text('Freedom Unite and Portable 2nd G use different hall names. Players using FUComplete should choose the same edition.')}
      {room && server && !joining && <button className="ghost mid" onClick={() => setJoining(true)}>Join another room</button>}
      {server && !joining && <button className="ghost mid" onClick={onOnlineSettings}>Connection settings</button>}
    </div>
  );
}

export function Home(props: Props) {
  const { state } = props;
  const s = state.stats;
  return (
    <>
      <Hero {...props} />
      <motion.div className="cards" {...rise(0.16)}>
        <RoomCard {...props} />
        <div className="glass lift card" style={{ gap: 14 }}>
          <span className="eyebrow">Activity</span>
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(2, minmax(0, 1fr))', gap: 14 }}>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
              <span className="stat">{hours(s.secondsPlayed)}</span>
              <span className="muted" style={{ fontSize: 13 }}>played</span>
            </div>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
              <span className="stat">{s.sessions}</span>
              <span className="muted" style={{ fontSize: 13 }}>{s.sessions === 1 ? 'session' : 'sessions'}</span>
            </div>
          </div>
          <span className="muted" style={{ fontSize: 13 }}>Last played: {lastPlayed(s.lastPlayed)}</span>
        </div>
      </motion.div>
    </>
  );
}
