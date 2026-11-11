import { useEffect, useRef, useState, type CSSProperties } from 'react';
import { motion } from 'motion/react';
import { api, regionTag, selectedGame, type SaveImportResult, type SaveScan, type Settings, type State } from './bridge';
import { CopyIcon } from './icons';
import { FucOptions } from './FucOptions';
import { OnlineSettings } from './OnlineSettings';
import { TexturePackSection } from './TexturePack';
import { frameRates, hudSizes, resolutions, songPlaces } from './format';
import { setNavigationPaused } from './gamepad';
import { keyGroups, keyLabel, reservedKeys } from './keys';
import { anySourcePressed, assignSource, padGroups, padLabel, pressedSource, stickChoices } from './pad';

const rise = (delay: number) => ({
  initial: { opacity: 0, y: 18, filter: 'blur(6px)' },
  animate: { opacity: 1, y: 0, filter: 'blur(0px)' },
  transition: { duration: 0.6, delay, ease: [0.22, 1, 0.36, 1] as const },
});

// The languages every regional disc of the game offers. '' leaves the choice to the game's own language screen, which is
// what the original shows at every start; picking one makes the game boot in that language and answer its screen
// (docs/06-mhfu-module.md).
const languages = [['', 'Ask'], ['en', 'EN'], ['fr', 'FR'], ['de', 'DE'], ['es', 'ES'], ['it', 'IT']] as const;
const languageNames: Record<string, string> = { en: 'English', fr: 'Français', de: 'Deutsch', es: 'Español', it: 'Italiano' };

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

// A setting in percent between `min` and `max`, in steps of `step`, with its help line.
function PercentSlider({ id, label, help, min, max, step, value, onCommit }: {
  id: string; label: string; help: string; min: number; max: number; step: number; value: number; onCommit: (v: number) => void;
}) {
  const [v, setV] = useState(value);
  useEffect(() => setV(value), [value]);
  return (
    <div className="srow">
      <label htmlFor={id} className="slabel"><b>{label}</b><span className="prose">{help}</span></label>
      <div className="slider" style={{ flex: '0 1 300px' }}>
        <input
          id={id}
          className="rng"
          type="range"
          min={min}
          max={max}
          step={step}
          value={v}
          style={{ ['--v' as string]: `${((v - min) / (max - min)) * 100}%` }}
          onChange={(e) => { const n = Number(e.target.value); setV(n); onCommit(n); }}
        />
        <output htmlFor={id} style={{ width: 52 }}>{v}%</output>
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

export function SettingsPage({ state, onChange, onPseudonym, onCopyCode, friendCode, onSavesFolder }: Pick<Props, 'state' | 'onChange' | 'onPseudonym' | 'onCopyCode'> & {
  friendCode: string;
  onSavesFolder: (backToDefault: boolean) => void;
}) {
  const s = state.settings;
  const [name, setName] = useState(state.pseudonym);
  useEffect(() => setName(state.pseudonym), [state.pseudonym]);
  const commitName = () => {
    const clean = name.trim().slice(0, 16);
    if (clean && clean !== state.pseudonym) onPseudonym(clean);
    else setName(state.pseudonym);
  };
  return (
    <section className="settings">
      <motion.div className="page-title" {...rise(0)}>
        <h2>Settings</h2>
        <p>These apply to MHForever and to all your games. Each game has its own video, audio and controls under Game settings.</p>
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
            <span className="code">{friendCode}</span>
            <button className="copy" onClick={onCopyCode} aria-label="Copy friend code"><CopyIcon /> Copy</button>
          </div>
        </div>
      </motion.div>
      <motion.div className="glass section" {...rise(0.2)}>
        <h3>Online (experimental)</h3>
        <OnlineSettings server={s.adhocServer ?? ''} onChange={(adhocServer) => onChange({ adhocServer })} />
      </motion.div>

      <motion.div className="glass section" {...rise(0.1)}>
        <h3>Storage</h3>
        <div className="srow">
          <div className="slabel"><b>Keep a copy of my disc image</b><span>Copies the image into MHForever's own folder the next time you add your game, so it keeps working if the original moves. Needs about 1 GB.</span></div>
          <Toggle on={s.copyImage} onToggle={() => onChange({ copyImage: !s.copyImage })} label="Keep a copy of my disc image" />
        </div>
        <div className="srow">
          <div className="slabel">
            <b>Saves folder</b>
            <span className="prose">
              Where your saves are kept, one folder per game. Choosing another copies your saves there; the old folder keeps its
              copy, and a game that already has saves in the new folder keeps those.
            </span>
            <span className="path">{state.savesFolder}</span>
          </div>
          <div className="row-actions">
            <button className="ghost mid" onClick={() => onSavesFolder(false)}>Change…</button>
            {state.savesFolderCustom && <button className="ghost mid" onClick={() => onSavesFolder(true)}>Use default</button>}
          </div>
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
        <div className="srow">
          <div className="slabel">
            <b>Check for updates</b>
            <span className="prose">When MHForever starts, it asks GitHub whether a newer release is out and offers to install it. Nothing else is sent.</span>
          </div>
          <Toggle on={s.checkUpdates ?? true} onToggle={() => onChange({ checkUpdates: !(s.checkUpdates ?? true) })} label="Check for updates" />
        </div>
      </motion.div>
    </section>
  );
}

export function GameSettingsPage({ state, onChange, onChangeCopy, onRemoveCopy, onLocateExecutable, onState, say }: Omit<Props, 'onPseudonym' | 'onCopyCode'> & {
  onState: (s: State) => void;
  say: (text: string) => void;
}) {
  const s = state.settings;
  const game = selectedGame(state) ?? state.games[0];
  const fucomplete = game?.discId === 'ULJM05500-FUC'; // FUComplete's own options apply
  return (
    <section className="settings">
      <motion.div className="page-title" {...rise(0)}>
        <h2>{game ? `${game.title} (${regionTag(game)})` : 'Game settings'}</h2>
        <p>Saved automatically for this copy of the game. Changes apply the next time the game starts.</p>
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
        <div className="srow">
          <div className="slabel">
            <b>HUD scale</b>
            <span>{(s.hudSize ?? 100) === 100 ? 'Original size.' : `${s.hudSize}% of the original size in quests: the health bar, the minimap and the items, each in its corner.`}</span>
          </div>
          <div className="seg wide" role="radiogroup" aria-label="HUD scale" style={{ '--n': hudSizes.length } as CSSProperties}>
            <span className="pill" style={{ transform: `translateX(${Math.max(0, hudSizes.indexOf(s.hudSize ?? 100)) * 100}%)` }} />
            {hudSizes.map((n) => (
              <button key={n} className={(s.hudSize ?? 100) === n ? 'on' : ''} role="radio" aria-checked={(s.hudSize ?? 100) === n} onClick={() => onChange({ hudSize: n })}>{n}%</button>
            ))}
          </div>
        </div>
      </motion.div>
      <motion.div className="glass section" {...rise(0.065)}>
        <h3>Game</h3>
        <div className="srow">
          <div className="slabel"><b>Show true weapon values</b><span>{fucomplete
            ? "FUComplete's true raw display: menus show attack, element and status without the display multipliers."
            : 'Optional enhancement: menus show attack and element without the display multipliers. Off is faithful to the original.'}</span></div>
          <Toggle on={s.trueWeaponValues} onToggle={() => onChange({ trueWeaponValues: !s.trueWeaponValues })} label="Show true weapon values" />
        </div>
        <div className="srow">
          <div className="slabel"><b>Fast supply box</b><span>Optional enhancement: after you take an item out of the supply box, the menu waits 2 frames instead of 30 before it accepts the next button. Off is faithful to the original.</span></div>
          <Toggle on={s.fastSupplyBox ?? false} onToggle={() => onChange({ fastSupplyBox: !(s.fastSupplyBox ?? false) })} label="Fast supply box" />
        </div>
        <div className="srow">
          <div className="slabel"><b>Smithy: browse with panels open</b><span>Optional enhancement: in the smithy, the list keeps scrolling while the materials (R) or Info panel is open, and the panel shows the highlighted item. Off is faithful to the original.</span></div>
          <Toggle on={s.smithyBrowse ?? false} onToggle={() => onChange({ smithyBrowse: !(s.smithyBrowse ?? false) })} label="Smithy: browse with panels open" />
        </div>
        {s.smithyBrowse && (
          <div className="srow">
            <div className="slabel"><b>Smithy: improved layout</b><span>Optional enhancement: with Info open (X), Info sits on the left, the list stays visible with the materials under it, and the left stick turns Info's pages. Off is faithful to the original.</span></div>
            <Toggle on={s.smithyLayout ?? false} onToggle={() => onChange({ smithyLayout: !(s.smithyLayout ?? false) })} label="Smithy: improved layout" />
          </div>
        )}
        <div className="srow">
          <div className="slabel"><b>Analog camera</b><span>Optional enhancement: the right stick turns the camera as fast as you push it, and tilts it smoothly from the game's lowest view to its highest. Off is faithful to the original (the stick works as the d-pad).</span></div>
          <Toggle on={s.analogCamera ?? false} onToggle={() => onChange({ analogCamera: !(s.analogCamera ?? false) })} label="Analog camera" />
        </div>
        {s.analogCamera && (
          <>
            <PercentSlider id="camera-turn-speed" label="Analog camera turn speed" help="How fast a full push turns the camera. 100% is as fast as the d-pad." min={50} max={200} step={10} value={s.analogCameraTurnSpeed ?? 100} onCommit={(v) => onChange({ analogCameraTurnSpeed: v })} />
            <PercentSlider id="camera-tilt-speed" label="Analog camera tilt speed" help="How fast a full push tilts the camera. At 100% it goes from the lowest view to the highest in about a second." min={50} max={200} step={10} value={s.analogCameraTiltSpeed ?? 100} onCommit={(v) => onChange({ analogCameraTiltSpeed: v })} />
            <div className="srow">
              <div className="slabel"><b>Mouse camera</b><span>The mouse turns and tilts the camera too, and aims a bow or a bowgun, while the game window holds it (its pointer is hidden). Escape lets the mouse go and opens the in-game menu.</span></div>
              <Toggle on={s.mouseCamera ?? false} onToggle={() => onChange({ mouseCamera: !(s.mouseCamera ?? false) })} label="Mouse camera" />
            </div>
            {s.mouseCamera && (
              <PercentSlider id="mouse-camera-speed" label="Mouse sensitivity" help="How far the camera moves for the same mouse motion." min={25} max={300} step={25} value={s.mouseCameraSpeed ?? 100} onCommit={(v) => onChange({ mouseCameraSpeed: v })} />
            )}
          </>
        )}
        <div className="srow">
          <div className="slabel"><b>Show hunting horn songs</b><span>Optional enhancement: in quests with a hunting horn, the songs it can play are listed on screen, the one being played lights up and the ones in effect turn green. Off is faithful to the original.</span></div>
          <Toggle on={s.hornSongs ?? false} onToggle={() => onChange({ hornSongs: !(s.hornSongs ?? false) })} label="Show hunting horn songs" />
        </div>
        {s.hornSongs && (
          <div className="srow">
            <div className="slabel"><b>Hunting horn songs position</b><span>{(s.hornSongsPlace ?? '').startsWith('custom:') ? 'A spot of your own, moved in the game. Pick one of these to use it instead.' : "Where the list is shown, out of the way of the game's HUD. In the game, Esc > Game moves it too, anywhere you like."}</span></div>
            <div className="seg wide" role="radiogroup" aria-label="Hunting horn songs position" style={{ '--n': songPlaces.length } as CSSProperties}>
              {!(s.hornSongsPlace ?? '').startsWith('custom:') && <span className="pill" style={{ transform: `translateX(${Math.max(0, songPlaces.findIndex((p) => p.id === (s.hornSongsPlace ?? 'next-to-sheet'))) * 100}%)` }} />}
              {songPlaces.map((p) => (
                <button key={p.id} className={(s.hornSongsPlace ?? 'next-to-sheet') === p.id ? 'on' : ''} role="radio" aria-checked={(s.hornSongsPlace ?? 'next-to-sheet') === p.id} onClick={() => onChange({ hornSongsPlace: p.id })}>{p.label}</button>
              ))}
            </div>
          </div>
        )}
      </motion.div>
      <motion.div className="glass section" {...rise(0.07)}>
        <h3>Texture pack</h3>
        <TexturePackSection state={state} onState={onState} say={say} toggle={(p) => <Toggle {...p} />} />
      </motion.div>
      {fucomplete && <motion.div className="glass section" {...rise(0.07)}>
        <h3>FUComplete</h3>
        <FucOptions s={s} onChange={onChange} toggle={(p) => <Toggle {...p} />} />
      </motion.div>}
      {game?.market !== 'JP' && <motion.div className="glass section" {...rise(0.07)}>
        <h3>Language</h3>
        <div className="srow">
          <div className="slabel">
            <b>Language</b>
            <span>
              {s.language === ''
                ? 'The game shows its own language screen at every start, as the original does.'
                : `The game starts in ${languageNames[s.language] ?? s.language} and skips its language screen.`}
            </span>
          </div>
          <div className="seg wide" role="radiogroup" aria-label="Language" style={{ '--n': languages.length } as CSSProperties}>
            <span className="pill" style={{ transform: `translateX(${Math.max(0, languages.findIndex(([id]) => id === s.language)) * 100}%)` }} />
            {languages.map(([id, label]) => (
              <button key={id} className={s.language === id ? 'on' : ''} role="radio" aria-checked={s.language === id}
                onClick={() => onChange({ language: id })}>{label}</button>
            ))}
          </div>
        </div>
      </motion.div>}
      <motion.div className="glass section" {...rise(0.08)}>
        <h3>Controls</h3>
        <Controls keys={s.keys ?? {}} pad={s.pad ?? {}} keyDefaults={state.keyDefaults ?? {}} padDefaults={state.padDefaults ?? {}} onChange={onChange} />
      </motion.div>
      <motion.div className="glass section" {...rise(0.12)}>
        <h3>Audio</h3>
        <Volume id="vol-music" label="Music" value={s.musicVolume} onCommit={(v) => onChange({ musicVolume: v })} />
        <Volume id="vol-sfx" label="Sound effects" value={s.sfxVolume} onCommit={(v) => onChange({ sfxVolume: v })} />
      </motion.div>
      <motion.div className="glass section" {...rise(0.21)}>
        <h3>Saves</h3>
        <ImportSaves gameId={game?.supported ? game.id : undefined} />
      </motion.div>
      <motion.div className="glass section" {...rise(0.24)}>
        <h3>Your copy</h3>
        <div className="srow">
          <div className="slabel"><b>Your copy</b><span>{game ? game.isoPath : 'No copy added yet'}</span></div>
          <div className="row-wrap" style={{ gap: 10 }}>
            <button className="ghost mid" onClick={onChangeCopy}>{game ? 'Change' : 'Add my copy'}</button>
            {game && <button className="ghost mid danger" onClick={() => onRemoveCopy(game.discId)}>Remove</button>}
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
    </section>
  );
}
