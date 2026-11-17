import type { CSSProperties, ReactElement } from 'react';
import type { Settings } from './bridge';

// Freedom DX's own options (launcher/native/fdx_config.hpp), written into FDXDAT/CONFIG.BIN before each start. Its
// minimap scale is not offered: the HUD size scales the whole quest HUD instead.

// The fields of view FUComplete offers, as Freedom DX's byte holds them (95 is the game's own).
const fieldsOfView = [[95, 'Default'], [111, 'Minor'], [127, 'Moderate'], [143, 'Extreme']] as const;
const cameraPositions = [1, 2, 3, 4, 5] as const;
const dosBonuses = [[0, 'Off'], [1, 'Blue Poogie'], [2, 'Pink Poogie']] as const;

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

export function FdxOptions({ s, onChange, toggle }: { s: Settings; onChange: (s: Partial<Settings>) => void; toggle: Toggle }) {
  const fov = fieldsOfView.some(([v]) => v === s.fdxFov) ? (s.fdxFov as number) : 95;
  const switches: [keyof Settings, string, string][] = [
    ['fdxHoldToGather', 'Hold to gather', 'Hold the gather button while crouched to keep gathering.'],
    ['fdxTrueRaw', 'True raw', 'Weapons show their true raw attack.'],
    ['fdxInstantSupply', 'Instant supply box', 'Removes the delay when taking items from the supply box.'],
    ['fdxFelyneSkills', 'Visible Felyne skills', "Shows each Felyne's skill set."],
    ['fdxDrinkBuff', 'Gathering Hall drink buff', 'Press Circle at the Gathering Hall table for a health and stamina bonus based on your Felyne chefs.'],
    ['fdxLaoEarlyKill', 'Early Lao-Shan Lung kill', 'Lao-Shan Lung can be killed before the last area.'],
    ['fdxSnsDebuff', 'Sword & Shield as in Dos', 'Sword & Shield sharpness and damage multiplier of 1.2 instead of 1.5.'],
    ['fdxOfflineTreasure', 'Offline treasure quests', 'Treasure quests can be taken alone in the offline Gathering Hall.'],
    ['fdxFileReplacer', 'File replacer', 'Loads replacement files from FDXDAT/NATIVEPSP on the memory stick.'],
  ];
  return (
    <>
      <div className="srow">
        <div className="slabel"><b>Field of view</b><span>Camera field of view in quests.</span></div>
        <Segments label="Field of view" value={fov} choices={fieldsOfView} onPick={(v) => onChange({ fdxFov: v })} />
      </div>
      <div className="srow">
        <div className="slabel"><b>Starting camera height</b><span>Initial camera height in quests, from 1 (lowest) to 5 (highest). Default: 4.</span></div>
        <Segments label="Starting camera height" value={s.fdxCamera ?? 4} choices={cameraPositions.map((n) => [n, String(n)] as const)} onPick={(v) => onChange({ fdxCamera: v })} />
      </div>
      <div className="srow">
        <div className="slabel"><b>Dos connection bonuses</b><span>The rewards of connecting to Monster Hunter Dos, with the Poogie outfit chosen.</span></div>
        <Segments label="Dos connection bonuses" value={s.fdxDosBonus ?? 0} choices={dosBonuses} onPick={(v) => onChange({ fdxDosBonus: v })} />
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
