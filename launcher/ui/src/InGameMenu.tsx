import { useState } from 'react';
import { motion } from 'motion/react';
import type { Settings, State } from './bridge';
import { CopyIcon, FriendsIcon } from './icons';
import { Volume } from './SettingsPage';
import { frameRates, resolutions } from './format';

type Props = {
  state: State;
  onChange: (s: Partial<Settings>) => void;
  onResume: () => void;
  onQuit: () => void;
  onCopyCode: () => void;
};

const rise = (delay: number) => ({
  initial: { opacity: 0, y: 16 },
  animate: { opacity: 1, y: 0 },
  transition: { duration: 0.35, delay, ease: [0.22, 1, 0.36, 1] as const },
});

// The launcher as the player sees it in the middle of a game: over the paused window, with what is needed while
// playing (docs/07-launcher-ux.md section 3c). Escape or B resumes.
export function InGameMenu({ state, onChange, onResume, onQuit, onCopyCode }: Props) {
  const [confirmQuit, setConfirmQuit] = useState(false);
  const s = state.settings;
  const game = state.games[0];
  return (
    <div className="ingame" role="dialog" aria-label="Game menu" data-nav-scope>
      <motion.div className="ingame-panel" initial={{ opacity: 0, x: -24 }} animate={{ opacity: 1, x: 0 }} transition={{ duration: 0.3, ease: [0.22, 1, 0.36, 1] }}>
        <motion.div className="ingame-head" {...rise(0)}>
          <span className="eyebrow">Paused</span>
          <h2>{game ? game.title : 'Game menu'}</h2>
          <span className="muted">{state.pseudonym}</span>
        </motion.div>

        <motion.button className="play" onClick={onResume} data-nav-default {...rise(0.04)}>Resume</motion.button>

        <motion.div className="glass section" {...rise(0.08)}>
          <h3>Friends and room</h3>
          <div className="srow">
            <div className="slabel"><b>Your friend code</b><span>Share it so a friend can add you.</span></div>
            <div className="code-box">
              <span className="code">{state.friendCode}</span>
              <button className="copy" onClick={onCopyCode} aria-label="Copy friend code"><CopyIcon /> Copy</button>
            </div>
          </div>
          <div className="srow">
            <div className="slabel"><b>Rooms and invites</b><span>Friends, rooms and invites arrive with online play. No accounts and no servers.</span></div>
            <FriendsIcon size={26} color="var(--accent)" />
          </div>
        </motion.div>

        <motion.div className="glass section" {...rise(0.12)}>
          <h3>Sound</h3>
          <Volume id="game-vol-music" label="Music" value={s.musicVolume} onCommit={(v) => onChange({ musicVolume: v })} />
          <Volume id="game-vol-sfx" label="Sound effects" value={s.sfxVolume} onCommit={(v) => onChange({ sfxVolume: v })} />
        </motion.div>

        <motion.div className="glass section" {...rise(0.16)}>
          <h3>Video</h3>
          <div className="srow">
            <div className="slabel">
              <b>{resolutions[s.scale - 1]} · {s.fps} fps{s.smoothTextures ? ' · smooth textures' : ''}</b>
              <span>Video settings are changed from the launcher's Settings and apply the next time the game starts. F11 switches fullscreen.</span>
            </div>
          </div>
          <div className="srow">
            <div className="slabel"><b>Frame rates</b><span>{frameRates.join(' · ')} fps</span></div>
          </div>
        </motion.div>

        <motion.div className="ingame-foot" {...rise(0.2)}>
          {confirmQuit ? (
            <>
              <span className="muted">Quit without saving? Progress since your last save is lost.</span>
              <button className="ghost mid danger" onClick={onQuit} data-nav-default>Quit the game</button>
              <button className="ghost mid" onClick={() => setConfirmQuit(false)}>Cancel</button>
            </>
          ) : (
            <button className="ghost mid danger" onClick={() => setConfirmQuit(true)}>Quit the game</button>
          )}
        </motion.div>
      </motion.div>
    </div>
  );
}
