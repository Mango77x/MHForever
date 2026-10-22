import { useEffect, useState, type CSSProperties } from 'react';
import { motion } from 'motion/react';
import type { Settings, State } from './bridge';
import { CopyIcon } from './icons';
import { frameRates, resolutions } from './format';

const rise = (delay: number) => ({
  initial: { opacity: 0, y: 18, filter: 'blur(6px)' },
  animate: { opacity: 1, y: 0, filter: 'blur(0px)' },
  transition: { duration: 0.6, delay, ease: [0.22, 1, 0.36, 1] as const },
});

type Props = {
  state: State;
  onChange: (s: Partial<Settings>) => void;
  onPseudonym: (name: string) => void;
  onCopyCode: () => void;
  onChangeCopy: () => void;
  onRemoveCopy: (id: string) => void;
  onLocateExecutable: () => void;
};

function Toggle({ on, onToggle, label }: { on: boolean; onToggle: () => void; label: string }) {
  return <button className={'tog' + (on ? ' on' : '')} onClick={onToggle} role="switch" aria-checked={on} aria-label={label} />;
}

export function Volume({ id, label, value, onCommit }: { id: string; label: string; value: number; onCommit: (v: number) => void }) {
  const [v, setV] = useState(value);
  useEffect(() => setV(value), [value]);
  return (
    <div className="srow">
      <label htmlFor={id} className="slabel" style={{ flex: '0 0 160px' }}><b>{label}</b></label>
      <div className="slider">
        <input
          id={id}
          className="rng"
          type="range"
          min={0}
          max={100}
          value={v}
          style={{ ['--v' as string]: `${v}%` }}
          onChange={(e) => { const n = Number(e.target.value); setV(n); onCommit(n); }}
        />
        <output htmlFor={id}>{v}</output>
      </div>
    </div>
  );
}

export function SettingsPage({ state, onChange, onPseudonym, onCopyCode, onChangeCopy, onRemoveCopy, onLocateExecutable }: Props) {
  const s = state.settings;
  const [name, setName] = useState(state.pseudonym);
  useEffect(() => setName(state.pseudonym), [state.pseudonym]);
  const commitName = () => {
    const clean = name.trim().slice(0, 16);
    if (clean && clean !== state.pseudonym) onPseudonym(clean);
    else setName(state.pseudonym);
  };
  const game = state.games[0];
  return (
    <section className="settings">
      <motion.div className="page-title" {...rise(0)}>
        <h2>Settings</h2>
        <p>Saved automatically. Video and audio changes apply the next time the game starts.</p>
      </motion.div>

      <motion.div className="glass section" {...rise(0.06)}>
        <h3>Video</h3>
        <div className="srow">
          <div className="slabel"><b>Internal resolution</b><span>{resolutions[s.scale - 1]}{s.scale === 4 ? ' · recommended' : ''}</span></div>
          <div className="seg wide" role="radiogroup" aria-label="Internal resolution" style={{ '--n': resolutions.length } as CSSProperties}>
            <span className="pill" style={{ transform: `translateX(${(s.scale - 1) * 100}%)` }} />
            {resolutions.map((_, i) => i + 1).map((n) => (
              <button key={n} className={s.scale === n ? 'on' : ''} role="radio" aria-checked={s.scale === n} onClick={() => onChange({ scale: n })}>{n}×</button>
            ))}
          </div>
        </div>
        <div className="srow">
          <div className="slabel"><b>Frame rate</b><span>{s.fps === 30 ? 'As the original game: 30 fps.' : `${s.fps} fps: the game still runs at 30, in-between pictures are drawn. Experimental.`}</span></div>
          <div className="seg wide" role="radiogroup" aria-label="Frame rate" style={{ '--n': frameRates.length } as CSSProperties}>
            <span className="pill" style={{ transform: `translateX(${Math.max(0, frameRates.indexOf(s.fps)) * 100}%)` }} />
            {frameRates.map((n) => (
              <button key={n} className={s.fps === n ? 'on' : ''} role="radio" aria-checked={s.fps === n} onClick={() => onChange({ fps: n })}>{n}</button>
            ))}
          </div>
        </div>
        <div className="srow">
          <div className="slabel"><b>Fullscreen</b><span>Start the game in fullscreen. F11 or Alt+Enter switches while playing.</span></div>
          <Toggle on={s.fullscreen} onToggle={() => onChange({ fullscreen: !s.fullscreen })} label="Fullscreen" />
        </div>
        <div className="srow">
          <div className="slabel"><b>Smooth texture filtering</b><span>Optional enhancement: softens pixelated textures. Off is faithful to the original.</span></div>
          <Toggle on={s.smoothTextures} onToggle={() => onChange({ smoothTextures: !s.smoothTextures })} label="Smooth texture filtering" />
        </div>
      </motion.div>

      <motion.div className="glass section" {...rise(0.1)}>
        <h3>Game files</h3>
        <div className="srow">
          <div className="slabel"><b>Keep a copy of my disc image</b><span>Copies the image into MHForever's own folder the next time you add your game, so it keeps working if the original moves. Needs about 1 GB.</span></div>
          <Toggle on={s.copyImage} onToggle={() => onChange({ copyImage: !s.copyImage })} label="Keep a copy of my disc image" />
        </div>
      </motion.div>

      <motion.div className="glass section" {...rise(0.12)}>
        <h3>Audio</h3>
        <Volume id="vol-music" label="Music" value={s.musicVolume} onCommit={(v) => onChange({ musicVolume: v })} />
        <Volume id="vol-sfx" label="Sound effects" value={s.sfxVolume} onCommit={(v) => onChange({ sfxVolume: v })} />
      </motion.div>

      <motion.div className="glass section" {...rise(0.18)}>
        <h3>Profile</h3>
        <div className="srow">
          <label htmlFor="pseudonym" className="slabel"><b>Name</b><span>Shown to friends, and typed into the game when it asks for a name.</span></label>
          <input
            id="pseudonym"
            className="text-input"
            value={name}
            maxLength={16}
            onChange={(e) => setName(e.target.value)}
            onBlur={commitName}
            onKeyDown={(e) => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); }}
            style={{ width: 240 }}
          />
        </div>
        <div className="srow">
          <div className="slabel"><b>Friend code</b><span>Stored only on this computer. Back it up from the profile menu.</span></div>
          <div className="code-box" style={{ minWidth: 300 }}>
            <span className="code">{state.friendCode}</span>
            <button className="copy" onClick={onCopyCode} aria-label="Copy friend code"><CopyIcon /> Copy</button>
          </div>
        </div>
      </motion.div>

      <motion.div className="glass section" {...rise(0.24)}>
        <h3>Game files</h3>
        <div className="srow">
          <div className="slabel"><b>Your copy</b><span>{game ? game.isoPath : 'No copy added yet'}</span></div>
          <div className="row-wrap" style={{ gap: 10 }}>
            <button className="ghost mid" onClick={onChangeCopy}>{game ? 'Change' : 'Add my copy'}</button>
            {game && <button className="ghost mid danger" onClick={() => onRemoveCopy(game.id)}>Remove</button>}
          </div>
        </div>
        <div className="srow">
          <div className="slabel">
            <b>Decrypted executable (developer build)</b>
            <span>{state.executable.found ? state.executable.path : 'Not found'}</span>
          </div>
          <button className="ghost mid" onClick={onLocateExecutable}>Locate</button>
        </div>
      </motion.div>

      <motion.div className="glass section" {...rise(0.3)}>
        <h3>About</h3>
        <div className="srow">
          <div className="slabel">
            <b>MHForever {state.version}</b>
            <span>Free software (GPL-3.0 or later). It contains none of the game's assets: it runs the copy you own.</span>
          </div>
        </div>
      </motion.div>
    </section>
  );
}
