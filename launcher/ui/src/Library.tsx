import { useState } from 'react';
import { motion } from 'motion/react';
import type { Game } from './bridge';
import { AlertIcon, PlusIcon } from './icons';

const rise = (delay: number) => ({
  initial: { opacity: 0, y: 18, filter: 'blur(6px)' },
  animate: { opacity: 1, y: 0, filter: 'blur(0px)' },
  transition: { duration: 0.6, delay, ease: [0.22, 1, 0.36, 1] as const },
});

function GameCard({ game, onOpen }: { game: Game; onOpen: () => void }) {
  const [t, setT] = useState({ x: 0, y: 0, on: false });
  return (
    <motion.button
      className="card3d"
      {...rise(0.08)}
      onClick={onOpen}
      onMouseMove={(e) => {
        const r = e.currentTarget.getBoundingClientRect();
        setT({ x: (e.clientX - r.left) / r.width - 0.5, y: (e.clientY - r.top) / r.height - 0.5, on: true });
      }}
      onMouseLeave={() => setT({ x: 0, y: 0, on: false })}
      style={{ transform: `perspective(900px) rotateY(${t.x * 14}deg) rotateX(${t.y * -14}deg) scale(${t.on ? 1.03 : 1})` }}
      aria-label={`${game.title}, ${game.region}`}
    >
      <svg viewBox="0 0 300 400" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
        <circle cx="210" cy="110" r="34" fill="#ffd9a0" opacity=".85" />
        <path d="M0 270 L60 200 L110 245 L170 160 L230 240 L300 190 L300 400 L0 400 Z" fill="#2c1d2b" />
        <path d="M0 330 L70 290 L140 320 L210 285 L300 320 L300 400 L0 400 Z" fill="#160f17" />
      </svg>
      <div className="shine" style={{ background: `radial-gradient(60% 50% at ${(t.x + 0.5) * 100}% ${(t.y + 0.5) * 100}%, rgba(255,240,210,.28), transparent 60%)` }} />
      <div className="meta">
        {game.supported ? (
          game.verification === 'verified' ? (
            <span className="chip" style={{ alignSelf: 'flex-start' }}><span className="dot" /> Ready to play</span>
          ) : (
            <span className="chip warn" style={{ alignSelf: 'flex-start' }}><AlertIcon size={14} /> Unverified dump</span>
          )
        ) : (
          <span className="chip warn" style={{ alignSelf: 'flex-start' }}>Not supported yet</span>
        )}
        <b>{game.title}</b>
        <span style={{ color: 'var(--ink-2)', fontSize: 13 }}>{game.region} · {game.discId}</span>
      </div>
    </motion.button>
  );
}

export function Library({ games, onAddCopy, onOpen }: { games: Game[]; onAddCopy: () => void; onOpen: () => void }) {
  return (
    <section style={{ display: 'flex', flexDirection: 'column', gap: 22 }}>
      <motion.div className="page-title" {...rise(0)}>
        <h2>Library</h2>
        <p>Games you have added from your own copies.</p>
      </motion.div>
      <div className="grid">
        {games.map((g) => <GameCard key={g.id + g.discId} game={g} onOpen={onOpen} />)}
        <motion.button className="addcard" {...rise(0.16)} onClick={onAddCopy} data-nav-default={games.length === 0 ? '' : undefined}>
          <span className="plus"><PlusIcon /></span>
          <b>{games.length === 0 ? 'Add my copy' : 'Add another copy'}</b>
          <span>Pick your ISO. It is checked on this computer and never uploaded anywhere.</span>
        </motion.button>
      </div>
    </section>
  );
}
