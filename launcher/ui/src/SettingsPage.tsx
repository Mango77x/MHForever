import { useEffect, useRef, useState, type CSSProperties } from 'react';
import { motion } from 'motion/react';
import { api, type SaveImportResult, type SaveScan, type Settings, type State } from './bridge';
import { CopyIcon } from './icons';
import { frameRates, resolutions } from './format';
import { setNavigationPaused } from './gamepad';
import { keyGroups, keyLabel, reservedKeys } from './keys';
import { anySourcePressed, assignSource, padGroups, padLabel, pressedSource, stickChoices } from './pad';

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

function Volume({ id, label, value, onCommit }: { id: string; label: string; value: number; onCommit: (v: number) => void }) {
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

// Bringing in saves made by a PSP or PPSSPP: choose the folder, see what was found, confirm.
function ImportSaves({ gameId }: { gameId: string | undefined }) {
  const [scan, setScan] = useState<SaveScan | null>(null);
  const [done, setDone] = useState<SaveImportResult | null>(null);
  const [busy, setBusy] = useState(false);
  const choose = async () => {
    if (!gameId || busy) return;
    setBusy(true);
    setDone(null);
    try {
      const r = await api.mhf_scanSaves(gameId);
      if (r.picked || r.error) setScan(r);
    } finally {
      setBusy(false);
    }
  };
  const apply = async () => {
    setBusy(true);
    try {
      setDone(await api.mhf_importSaves());
      setScan(null);
    } finally {
      setBusy(false);
    }
  };
  const found = scan?.found ?? [];
  const replaces = found.some((f) => f.exists);
  return (
    <>
      <div className="srow">
        <div className="slabel">
          <b>Import saves</b>
          <span>Bring your Freedom Unite saves from a PSP memory stick or from PPSSPP. Choose the folder that holds them (the PSP/SAVEDATA folder, or PPSSPP's memory stick folder). Only saves of your own version of the game can be used.</span>
        </div>
        <button className="ghost mid" onClick={choose} disabled={!gameId || busy}>Choose folder…</button>
      </div>
      {scan && (
        <div className="srow" style={{ flexDirection: 'column', alignItems: 'stretch', gap: 10 }}>
          {scan.error && <span className="muted">{scan.error}</span>}
          {found.length === 0 && !scan.error && <span className="muted">{scan.note}</span>}
          {found.map((f) => (
            <div key={f.folder} className="slabel">
              <b>{f.title || 'Game data'} <span style={{ opacity: 0.6 }}>({f.folder})</span></b>
              <span>{f.exists ? 'Replaces the save you have now. Yours is kept in a backup folder.' : 'A new save.'}</span>
            </div>
          ))}
          {found.length > 0 && (
            <div className="row-wrap" style={{ gap: 10 }}>
              <button className={replaces ? 'ghost mid danger' : 'ghost mid'} onClick={apply} disabled={busy}>
                {found.length === 1 ? 'Import this save' : `Import ${found.length} saves`}
              </button>
              <button className="ghost mid" onClick={() => setScan(null)} disabled={busy}>Cancel</button>
            </div>
          )}
        </div>
      )}
      {done && (
        <div className="srow">
          <div className="slabel">
            <b>{done.error ? 'The import did not finish' : done.imported === 1 ? 'Save imported' : `${done.imported} saves imported`}</b>
            <span>
              {done.error || 'Press Play and load it from the game.'}
              {done.backup && ` Your previous save is kept in ${done.backup}.`}
            </span>
          </div>
        </div>
      )}
    </>
  );
}

// Keyboard bindings: click a key and press the one you want. Two buttons can never share a key (the other one takes
// the key this one had), Backspace leaves a button without a key, Esc cancels (docs/07-launcher-ux.md section 5).
function KeyboardControls({ keys, defaults, onChange }: {
  keys: Record<string, string>;
  defaults: Record<string, string>;
  onChange: (s: Partial<Settings>) => void;
}) {
  const [capturing, setCapturing] = useState<string | null>(null);
  const [notice, setNotice] = useState('');
  const latest = useRef({ keys, onChange });
  latest.current = { keys, onChange };

  useEffect(() => {
    if (!capturing) return;
    setNavigationPaused(true); // the keys being recorded must not move the launcher around
    const record = (e: KeyboardEvent) => {
      e.preventDefault();
      e.stopImmediatePropagation();
      if (e.repeat) return;
      const action = capturing;
      if (e.code === 'Escape') {
        setCapturing(null);
        return;
      }
      if (reservedKeys[e.code]) {
        setNotice(`${keyLabel(e.code)} is kept by the game: ${reservedKeys[e.code]} Press another key, or Esc to cancel.`);
        return;
      }
      setNotice('');
      const next = { ...latest.current.keys };
      const previous = next[action] ?? '';
      if (e.code !== 'Backspace' && e.code !== 'Delete') {
        for (const [other, value] of Object.entries(next)) {
          if (other !== action && value === e.code) next[other] = previous;
        }
        next[action] = e.code;
      } else {
        next[action] = '';
      }
      latest.current.onChange({ keys: next });
      setCapturing(null);
    };
    window.addEventListener('keydown', record, true);
    return () => {
      window.removeEventListener('keydown', record, true);
      setNavigationPaused(false);
    };
  }, [capturing]);

  return (
    <>
      <div className="srow">
        <div className="slabel">
          <b>Keyboard</b>
          <span>Click a key and press the one you want. Backspace removes it, Esc cancels.</span>
        </div>
        <button className="ghost mid" onClick={() => { setNotice(''); onChange({ keys: { ...defaults } }); }}>Restore defaults</button>
      </div>
      {notice && (
        <div className="srow">
          <span className="slabel" style={{ color: 'var(--warn)', fontSize: 13 }}>{notice}</span>
        </div>
      )}
      {keyGroups.map((group) => (
        <div className="srow" key={group.title}>
          <div className="slabel"><b>{group.title}</b></div>
          <div className="row-wrap" style={{ gap: 8 }}>
            {group.actions.map((action) => {
              const code = keys[action.id] ?? '';
              const listening = capturing === action.id;
              return (
                <button
                  key={action.id}
                  className={'keycap' + (listening ? ' listening' : '') + (code ? '' : ' unset')}
                  aria-label={`${action.label}: ${listening ? 'press a key' : keyLabel(code)}`}
                  onClick={() => { setNotice(''); setCapturing(listening ? null : action.id); }}
                >
                  <span className="keycap-name">{action.label}</span>
                  <span className="keycap-key">{listening ? 'Press a key…' : keyLabel(code)}</span>
                </button>
              );
            })}
          </div>
        </div>
      ))}
    </>
  );
}

// Controller bindings: click a button and press the one you want on the controller. Two buttons never share a source (the
// other one takes the source this one had), Backspace or Delete on the keyboard leaves a button empty, Esc cancels. The
// controller's own navigation is paused while recording (docs/07-launcher-ux.md section 5).
function PadControls({ pad, defaults, onChange }: {
  pad: Record<string, string>;
  defaults: Record<string, string>;
  onChange: (s: Partial<Settings>) => void;
}) {
  const [capturing, setCapturing] = useState<string | null>(null);
  const [connected, setConnected] = useState(false);
  const latest = useRef({ pad, onChange });
  latest.current = { pad, onChange };

  useEffect(() => {
    const check = () => setConnected(Array.from(navigator.getGamepads ? navigator.getGamepads() : []).some((g) => g && g.connected));
    check();
    window.addEventListener('gamepadconnected', check);
    window.addEventListener('gamepaddisconnected', check);
    return () => {
      window.removeEventListener('gamepadconnected', check);
      window.removeEventListener('gamepaddisconnected', check);
    };
  }, []);

  useEffect(() => {
    if (!capturing) return;
    setNavigationPaused(true);
    let frame = 0;
    let released = !anySourcePressed(); // the press that started the recording must not be the answer
    const poll = () => {
      const pads = navigator.getGamepads ? navigator.getGamepads() : [];
      let pressed: string | null = null;
      for (const g of pads) {
        if (g && g.connected && pressed === null) pressed = pressedSource(g);
      }
      if (pressed === null) released = true;
      else if (released) {
        latest.current.onChange({ pad: assignSource(latest.current.pad, capturing, pressed) });
        setCapturing(null);
        return;
      }
      frame = requestAnimationFrame(poll);
    };
    frame = requestAnimationFrame(poll);
    const key = (e: KeyboardEvent) => {
      if (e.code === 'Escape') {
        e.preventDefault();
        e.stopImmediatePropagation();
        setCapturing(null);
      } else if (e.code === 'Backspace' || e.code === 'Delete') {
        e.preventDefault();
        e.stopImmediatePropagation();
        latest.current.onChange({ pad: { ...latest.current.pad, [capturing]: '' } });
        setCapturing(null);
      }
    };
    window.addEventListener('keydown', key, true);
    return () => {
      cancelAnimationFrame(frame);
      window.removeEventListener('keydown', key, true);
      setNavigationPaused(false);
    };
  }, [capturing]);

  const stick = pad.stick ?? 'left';
  const stickIndex = Math.max(0, stickChoices.findIndex((c) => c.value === stick));
  return (
    <>
      <div className="srow">
        <div className="slabel">
          <b>Controller</b>
          <span>
            {connected
              ? 'Click a button and press the one you want on your controller. Backspace removes it, Esc cancels.'
              : 'No controller detected. Connect one and press any button, then click a button to change it.'}
          </span>
        </div>
        <button className="ghost mid" onClick={() => onChange({ pad: { ...defaults } })}>Restore defaults</button>
      </div>
      {padGroups.map((group) => (
        <div className="srow" key={group.title}>
          <div className="slabel"><b>{group.title}</b></div>
          <div className="row-wrap" style={{ gap: 8 }}>
            {group.actions.map((action) => {
              const value = pad[action.id] ?? '';
              const listening = capturing === action.id;
              return (
                <button
                  key={action.id}
                  className={'keycap' + (listening ? ' listening' : '') + (value ? '' : ' unset')}
                  aria-label={`${action.label}: ${listening ? 'press a button' : padLabel(value)}`}
                  onClick={() => setCapturing(listening ? null : action.id)}
                >
                  <span className="keycap-name">{action.label}</span>
                  <span className="keycap-key">{listening ? 'Press a button…' : padLabel(value)}</span>
                </button>
              );
            })}
          </div>
        </div>
      ))}
      <div className="srow">
        <div className="slabel"><b>Analog stick</b><span>Which stick moves the character.</span></div>
        <div className="seg wide" role="radiogroup" aria-label="Analog stick" style={{ '--n': stickChoices.length } as CSSProperties}>
          <span className="pill" style={{ transform: `translateX(${stickIndex * 100}%)` }} />
          {stickChoices.map((choice) => (
            <button key={choice.value} className={stick === choice.value ? 'on' : ''} role="radio" aria-checked={stick === choice.value}
              onClick={() => onChange({ pad: { ...pad, stick: choice.value } })}>{choice.label}</button>
          ))}
        </div>
      </div>
    </>
  );
}

function Controls({ keys, pad, keyDefaults, padDefaults, onChange }: {
  keys: Record<string, string>;
  pad: Record<string, string>;
  keyDefaults: Record<string, string>;
  padDefaults: Record<string, string>;
  onChange: (s: Partial<Settings>) => void;
}) {
  const [device, setDevice] = useState<'keyboard' | 'controller'>('keyboard');
  const options = [['keyboard', 'Keyboard'], ['controller', 'Controller']] as const;
  return (
    <>
      <div className="srow">
        <div className="slabel"><b>Edit the controls of</b><span>Both work at the same time while playing.</span></div>
        <div className="seg wide" role="radiogroup" aria-label="Device" style={{ '--n': options.length } as CSSProperties}>
          <span className="pill" style={{ transform: `translateX(${device === 'keyboard' ? 0 : 100}%)` }} />
          {options.map(([id, label]) => (
            <button key={id} className={device === id ? 'on' : ''} role="radio" aria-checked={device === id} onClick={() => setDevice(id)}>{label}</button>
          ))}
        </div>
      </div>
      {device === 'keyboard'
        ? <KeyboardControls keys={keys} defaults={keyDefaults} onChange={onChange} />
        : <PadControls pad={pad} defaults={padDefaults} onChange={onChange} />}
    </>
  );
}

export function SettingsPage({ state, onChange, onPseudonym, onCopyCode, onChangeCopy, onRemoveCopy, onLocateExecutable }: Props) {
  const s = state.settings;
  const [name, setName] = useState(state.pseudonym);
  useEffect(() => setName(state.pseudonym), [state.pseudonym]);
  const [server, setServer] = useState(state.settings.adhocServer ?? '');
  useEffect(() => setServer(state.settings.adhocServer ?? ''), [state.settings.adhocServer]);
  const commitServer = () => {
    if (server !== (state.settings.adhocServer ?? '')) onChange({ adhocServer: server });
    else setServer(state.settings.adhocServer ?? '');
  };
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
        <p>Saved automatically. Video, audio and control changes apply the next time the game starts.</p>
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

      <motion.div className="glass section" {...rise(0.08)}>
        <h3>Controls</h3>
        <Controls keys={s.keys ?? {}} pad={s.pad ?? {}} keyDefaults={state.keyDefaults ?? {}} padDefaults={state.padDefaults ?? {}} onChange={onChange} />
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

      <motion.div className="glass section" {...rise(0.2)}>
        <h3>Online play (experimental)</h3>
        <div className="srow">
          <label htmlFor="adhoc-server" className="slabel">
            <b>Ad hoc server</b>
            <span>
              The PSP ad hoc server the game plays through, as <code>host</code> or <code>host:port</code>, the same kind PPSSPP uses.
              Players on the same server and group meet in the game's own lobby. Leave it empty to stay offline: nothing is contacted.
              The server sees your name and your game, and may show them publicly.
            </span>
          </label>
          <input
            id="adhoc-server"
            className="text-input"
            value={server}
            placeholder="Offline"
            spellCheck={false}
            onChange={(e) => setServer(e.target.value.trim())}
            onBlur={commitServer}
            onKeyDown={(e) => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); }}
            style={{ width: 280 }}
          />
        </div>
      </motion.div>

      <motion.div className="glass section" {...rise(0.21)}>
        <h3>Saves</h3>
        <ImportSaves gameId={game?.supported ? game.id : undefined} />
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
