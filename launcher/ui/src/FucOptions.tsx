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
    ['fucDosAudio', 'Dos battle themes', 'The Jungle, Desert, Swamp, Snowy Mountain, Volcano and Tower battle themes are replaced with their Monster Hunter Dos counterparts.'],
    ['fucHornTweak', 'Hunting horn tweak', "Notes only clear when the weapon is sheathed, as in Frontier; the side swings hit harder and the hilt jab cuts."],
    ['fucLaoEarlyKill', 'Early Lao-Shan Lung kill', 'Lao-Shan Lung, Ash Lao-Shan Lung and Shen Gaoren on the Fortress can be killed before area 5. Online, a patched player must enter the area first.'],
    ['fucFullChest', 'Guildhall full chest', 'Equipment can be changed at the Guildhall item chest. Offline only.'],
    ['fucDrinkBuff', 'Guildhall drink buff', 'Press circle at the Guildhall table for more health and stamina, by the number of Felyne chefs in your kitchen.'],
  ];
  return (
    <>
      <div className="srow">
        <div className="slabel"><b>Saves</b><span>{`FUComplete loads and writes ${saveRegionNames[region] ?? region} saves.`}</span></div>
        <Segments label="FUComplete saves" value={region} choices={saveRegions} onPick={(v) => onChange({ fucSaveRegion: v })} />
      </div>
      <div className="srow">
        <div className="slabel"><b>Field of view</b><span>The camera's field of view in quests. Default is the original.</span></div>
        <Segments label="Field of view" value={s.fucFov ?? 'default'} choices={fieldsOfView} onPick={(v) => onChange({ fucFov: v })} />
      </div>
      <div className="srow">
        <div className="slabel"><b>Starting camera height</b><span>Where the camera points when a quest starts, from 1 (the ground) to 5 (the sky). 4 is the original.</span></div>
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
