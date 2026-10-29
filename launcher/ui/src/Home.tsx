import { useState } from 'react';
import { motion } from 'motion/react';
import { selectedGame, type State } from './bridge';
import { AlertIcon, CheckIcon, FolderIcon, PlayIcon, PlusIcon, SlidersIcon } from './icons';
import { hours, lastPlayed, resolutions } from './format';
import { formatRoomCode, newRoomCode, roomCode } from './rooms';

type Props = {
  state: State;
  onPlay: () => void;
  onAddCopy: () => void;
  onLocateExecutable: () => void;
  onSettings: () => void;
  onOnlineSettings: () => void;
  onRoom: (code: string) => void; // '' for the public halls
  onCopy: (text: string) => void;
};

const rise = (delay: number) => ({
  initial: { opacity: 0, y: 18, filter: 'blur(6px)' },
  animate: { opacity: 1, y: 0, filter: 'blur(0px)' },
  transition: { duration: 0.6, delay, ease: [0.22, 1, 0.36, 1] as const },
});

export function Hero({ state, onPlay, onAddCopy, onLocateExecutable, onSettings }: Omit<Props, 'onOnlineSettings' | 'onRoom' | 'onCopy'>) {
  const [m, setM] = useState({ x: 0, y: 0 });
  const game = selectedGame(state);
  const layer = (k: number, s: number) => ({ transform: `translate3d(${m.x * -k}px, ${m.y * -k * 0.45}px, 0) scale(${s})` });
  return (
    <motion.section
      className="hero"
      {...rise(0)}
      onMouseMove={(e) => {
        const r = e.currentTarget.getBoundingClientRect();
        setM({ x: (e.clientX - r.left) / r.width - 0.5, y: (e.clientY - r.top) / r.height - 0.5 });
      }}
      onMouseLeave={() => setM({ x: 0, y: 0 })}
    >
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(12, 1.04)}>
        <circle cx="880" cy="150" r="70" fill="#ffd9a0" opacity=".9" />
        <circle cx="880" cy="150" r="130" fill="#ffd9a0" opacity=".12" />
        <path d="M0 330 L160 210 L280 290 L420 150 L560 280 L700 190 L860 300 L1000 170 L1200 280 L1200 500 L0 500 Z" fill="#3b2a3a" opacity=".85" />
      </svg>
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(26, 1.06)}>
        <path d="M0 390 L140 300 L250 360 L390 250 L520 350 L640 290 L780 380 L930 280 L1080 360 L1200 320 L1200 500 L0 500 Z" fill="#26192a" />
        <path d="M390 250 L420 272 L405 270 L390 262 L372 270 Z" fill="#f4efe6" opacity=".7" />
        <path d="M930 280 L955 300 L940 297 L930 290 L914 298 Z" fill="#f4efe6" opacity=".6" />
      </svg>
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(44, 1.08)}>
        <path d="M0 440 L120 400 L230 430 L360 380 L470 425 L610 395 L760 440 L900 410 L1050 445 L1200 415 L1200 500 L0 500 Z" fill="#140e16" />
      </svg>
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
            <p>Add your own copy of Freedom Unite and play it natively on PC. Nothing is downloaded and nothing leaves your computer.</p>
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
function RoomCard({ state, onOnlineSettings, onRoom, onCopy }: Pick<Props, 'state' | 'onOnlineSettings' | 'onRoom' | 'onCopy'>) {
  const [joining, setJoining] = useState(false);
  const [typed, setTyped] = useState('');
  const server = state.settings.adhocServer ?? '';
  const room = state.settings.adhocRoom ?? '';
  const text = (body: string) => <p style={{ margin: 0, color: 'var(--ink-2)', fontSize: 15, lineHeight: 1.55 }}>{body}</p>;
  const typedCode = roomCode(typed);
  const join = () => {
    if (!typedCode) return;
    onRoom(typedCode);
    setJoining(false);
    setTyped('');
  };
  return (
    <div className="glass lift card">
      <span className="eyebrow">Play with friends</span>
      {server === '' ? (
        <>
          {text('Online play is off. Choose a server first; then create a room and share its code, or join a friend\'s.')}
          <div className="row-wrap" style={{ marginTop: 'auto', gap: 10 }}>
            <button className="ghost mid" onClick={onOnlineSettings}>Choose a server</button>
          </div>
        </>
      ) : room !== '' ? (
        <>
          <span className="stat" style={{ letterSpacing: '0.08em' }}>{formatRoomCode(room)}</span>
          {text(`Your private room on ${server}. Friends who play with this code meet you in the Gathering Hall's halls; nobody else sees them. Only MHForever players can join.`)}
          <div className="row-wrap" style={{ marginTop: 'auto', gap: 10 }}>
            <button className="ghost mid" onClick={() => onCopy(formatRoomCode(room))}>Copy code</button>
            <button className="ghost mid" onClick={() => onRoom('')}>Back to public halls</button>
          </div>
        </>
      ) : joining ? (
        <>
          {text('Type or paste the code your friend sent you.')}
          <input
            className="text-input"
            value={typed}
            placeholder="K7QF-29XM"
            aria-label="Room code"
            aria-invalid={typed.trim() !== '' && !typedCode}
            spellCheck={false}
            autoFocus
            onChange={(e) => setTyped(e.target.value)}
            onKeyDown={(e) => { if (e.key === 'Enter') join(); if (e.key === 'Escape') setJoining(false); }}
          />
          <div className="row-wrap" style={{ marginTop: 'auto', gap: 10 }}>
            <button className="ghost mid" onClick={join} disabled={!typedCode}>Join room</button>
            <button className="ghost mid" onClick={() => setJoining(false)}>Cancel</button>
          </div>
        </>
      ) : (
        <>
          {text(`You play in the public halls of ${server}, with PPSSPP and PSP players too. A room keeps the halls to you and your friends.`)}
          <div className="row-wrap" style={{ marginTop: 'auto', gap: 10 }}>
            <button className="ghost mid" onClick={() => onRoom(newRoomCode())}>Create room</button>
            <button className="ghost mid" onClick={() => setJoining(true)}>Join with code</button>
          </div>
        </>
      )}
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
