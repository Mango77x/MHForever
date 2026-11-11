import type { CSSProperties, ReactElement } from 'react';
import type { Settings } from './bridge';

// FUComplete's own options (launcher/native/fuc_config.hpp), written into its CONFIG.BIN before each start. Its true raw
// display is the "Show true weapon values" switch of the Game section, and its minimap size is not offered: the HUD size
// scales the whole quest HUD instead.

const saveRegions = [['ULJM05500', 'JP'], ['ULUS10391', 'NA'], ['ULES01213', 'EU']] as const;
const saveRegionNames: Record<string, string> = { ULJM05500: 'Japanese (Portable 2nd G)', ULUS10391: 'North American', ULES01213: 'European' };
const fieldsOfView = [['default', 'Default'], ['minor', 'Minor'], ['moderate', 'Moderate'], ['extreme', 'Extreme']] as const;
const cameraPositions = [1, 2, 3, 4, 5] as const;

type Toggle = (props: { on: boolean; onToggle: () => void; label: string }) => ReactElement;

function Segments<T extends string | number>({ label, value, choices, onPick }: {
  label: string;
  value: T;
  choices: readonly (readonly [T, string])[];
  onPick: (v: T) => void;
}) {
  const at = Math.max(0, choices.findIndex(([id]) => id === value));
  return (
    <div className="seg wide" role="radiogroup" aria-label={label} style={{ '--n': choices.length } as CSSProperties}>
      <span className="pill" style={{ transform: `translateX(${at * 100}%)` }} />
      {choices.map(([id, text]) => (
        <button key={String(id)} className={value === id ? 'on' : ''} role="radio" aria-checked={value === id} onClick={() => onPick(id)}>{text}</button>
      ))}
    </div>
  );
}

export function FucOptions({ s, onChange, toggle }: { s: Settings; onChange: (s: Partial<Settings>) => void; toggle: Toggle }) {
  const region = s.fucSaveRegion ?? 'ULUS10391';
  const switches: [keyof Settings, string, string][] = [
    ['fucDosAudio', 'Dos battle themes', 'Replaces the Jungle, Desert, Swamp, Snowy Mountain, Volcano and Tower battle themes with those from Monster Hunter Dos.'],
    ['fucHornTweak', 'Hunting horn tweak', 'Notes clear only when the weapon is sheathed; side swings deal more damage and the hilt jab cuts.'],
    ['fucLaoEarlyKill', 'Early Lao-Shan Lung kill', 'Lao-Shan Lung, Ash Lao-Shan Lung and Shen Gaoren can be killed before area 5. Online, a player with the patch must enter the area first.'],
    ['fucFullChest', 'Guildhall full chest', 'Equipment can be changed at the Guildhall item chest. Offline only.'],
    ['fucDrinkBuff', 'Guildhall drink buff', 'Press Circle at the Guildhall table for a health and stamina bonus based on your Felyne chefs.'],
  ];
  return (
    <>
      <div className="srow">
        <div className="slabel"><b>Saves</b><span>{`FUComplete uses ${saveRegionNames[region] ?? region} saves.`}</span></div>
        <Segments label="FUComplete saves" value={region} choices={saveRegions} onPick={(v) => onChange({ fucSaveRegion: v })} />
      </div>
      <div className="srow">
        <div className="slabel"><b>Field of view</b><span>Camera field of view in quests.</span></div>
        <Segments label="Field of view" value={s.fucFov ?? 'default'} choices={fieldsOfView} onPick={(v) => onChange({ fucFov: v })} />
      </div>
      <div className="srow">
        <div className="slabel"><b>Starting camera height</b><span>Initial camera height in quests, from 1 (lowest) to 5 (highest). Default: 4.</span></div>
        <Segments label="Starting camera height" value={s.fucCamera ?? 4} choices={cameraPositions.map((n) => [n, String(n)] as const)} onPick={(v) => onChange({ fucCamera: v })} />
      </div>
      {switches.map(([key, title, text]) => (
        <div className="srow" key={key}>
          <div className="slabel"><b>{title}</b><span>{text}</span></div>
          {toggle({ on: Boolean(s[key]), onToggle: () => onChange({ [key]: !s[key] } as Partial<Settings>), label: title })}
        </div>
      ))}
    </>
  );
}
