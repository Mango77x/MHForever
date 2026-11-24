import { useState } from 'react';
import { motion } from 'motion/react';
import type { Game } from './bridge';
import { CardScenery, cardBorderColor, cardSky, gameAccent, useImageAccent } from './GameArt';
import { AlertIcon, PlusIcon } from './icons';

const rise = (delay: number) => ({
  initial: { opacity: 0, y: 18, filter: 'blur(6px)' },
  animate: { opacity: 1, y: 0, filter: 'blur(0px)' },
  transition: { duration: 0.6, delay, ease: [0.22, 1, 0.36, 1] as const },
});

function GameCard({ game, selected, useCustomArt, showGameIcon, onOpen }: { game: Game; selected: boolean; useCustomArt: boolean; showGameIcon: boolean; onOpen: () => void }) {
  const [t, setT] = useState({ x: 0, y: 0, on: false });
  const cover = useCustomArt ? game.coverUrl : undefined;
  const coverAccent = useImageAccent(cover);
  const iconAccent = useImageAccent(game.iconUrl);
  const accent = gameAccent(game, coverAccent, iconAccent);
  const border = cardBorderColor(game, accent);
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
      style={{ transform: `perspective(900px) rotateY(${t.x * 14}deg) rotateX(${t.y * -14}deg) scale(${t.on ? 1.03 : 1})`, background: cardSky(game, accent), borderColor: border }}
      aria-label={`${game.title}, ${game.region}`}
    >
      <CardScenery game={game} accent={accent} />
      {cover && (
        <img
          key={`${game.discId}-cover`}
          className="card-pic"
          src={cover}
          alt=""
          draggable={false}
          decoding="async"
          onLoad={(e) => { e.currentTarget.hidden = false; }}
          onError={(e) => { e.currentTarget.hidden = true; }}
        />
      )}
      {showGameIcon && game.iconUrl?.startsWith('data:image/png;base64,') && (
        <img
          key={`${game.discId}-icon`}
          className="card-icon"
          src={game.iconUrl}
          alt=""
          draggable={false}
          decoding="async"
          style={{ borderColor: border }}
          onLoad={(e) => { e.currentTarget.hidden = false; }}
          onError={(e) => { e.currentTarget.hidden = true; }}
        />
      )}
      <div className="shine" style={{ background: `radial-gradient(60% 50% at ${(t.x + 0.5) * 100}% ${(t.y + 0.5) * 100}%, rgba(255,240,210,.28), transparent 60%)` }} />
      <div className="meta">
        {game.supported ? (
          game.verification === 'verified' ? (
            <span className="chip" style={{ alignSelf: 'flex-start' }}><span className="dot" /> {selected ? 'Selected · ready to play' : 'Ready to play'}</span>
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

export function Library({ games, selectedDisc, useCustomArt, showGameIcon, onAddCopy, onOpen }: { games: Game[]; selectedDisc: string; useCustomArt: boolean; showGameIcon: boolean; onAddCopy: () => void; onOpen: (game: Game) => void }) {
  return (
    <section style={{ display: 'flex', flexDirection: 'column', gap: 22 }}>
      <motion.div className="page-title" {...rise(0)}>
        <h2>Library</h2>
        <p>Games you have added from your own copies.</p>
      </motion.div>
      <div className="grid">
        {games.map((g) => <GameCard key={g.id + g.discId} game={g} selected={g.discId === selectedDisc} useCustomArt={useCustomArt} showGameIcon={showGameIcon} onOpen={() => onOpen(g)} />)}
        <motion.button className="addcard" {...rise(0.16)} onClick={onAddCopy} data-nav-default={games.length === 0 ? '' : undefined}>
          <span className="plus"><PlusIcon /></span>
          <b>{games.length === 0 ? 'Add my copy' : 'Add another copy'}</b>
          <span>Pick your ISO. It is checked on this computer and never uploaded anywhere.</span>
        </motion.button>
      </div>
    </section>
  );
}
