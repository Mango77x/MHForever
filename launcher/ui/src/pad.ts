// Controller bindings shown in Settings → Controls. A binding is one or more SDL gamepad names ("south", "left_shoulder",
// "dpad_up", "left_trigger"...) joined with '+': they describe the position of a button on a standard layout, whatever
// the maker of the controller calls it. '' means the pad button has no source. The native side stores them in
// launcher.json and hands them to the game (docs/07-launcher-ux.md section 5).

// The pad buttons the player can bind: the same groups as the keyboard, but the analog stick is one choice, not four keys.
export const padGroups: { title: string; actions: { id: string; label: string }[] }[] = [
  {
    title: 'Face buttons',
    actions: [
      { id: 'south', label: 'Cross' },
      { id: 'east', label: 'Circle' },
      { id: 'west', label: 'Square' },
      { id: 'north', label: 'Triangle' },
    ],
  },
  {
    title: 'Shoulders',
    actions: [
      { id: 'left_shoulder', label: 'L' },
      { id: 'right_shoulder', label: 'R' },
    ],
  },
  {
    title: 'Menu',
    actions: [
      { id: 'start', label: 'Start' },
      { id: 'select', label: 'Select' },
    ],
  },
  {
    title: 'D-pad',
    actions: [
      { id: 'up', label: 'Up' },
      { id: 'down', label: 'Down' },
      { id: 'left', label: 'Left' },
      { id: 'right', label: 'Right' },
    ],
  },
];

export const stickChoices: { value: string; label: string }[] = [
  { value: 'left', label: 'Left stick' },
  { value: 'right', label: 'Right stick' },
  { value: '', label: 'None' },
];

// The browser's Gamepad API numbers the buttons of a "standard" controller like this; the native side uses the SDL names.
const standardButtons: Record<number, string> = {
  0: 'south', 1: 'east', 2: 'west', 3: 'north',
  4: 'left_shoulder', 5: 'right_shoulder', 6: 'left_trigger', 7: 'right_trigger',
  8: 'back', 9: 'start', 10: 'left_stick', 11: 'right_stick',
  12: 'dpad_up', 13: 'dpad_down', 14: 'dpad_left', 15: 'dpad_right',
};

// The source that was just pressed on `pad`, or null. The Guide button belongs to the system, so it is never offered.
export function pressedSource(pad: Gamepad): string | null {
  for (const [index, name] of Object.entries(standardButtons)) {
    if (pad.buttons[Number(index)]?.pressed) return name;
  }
  return null;
}

export function anySourcePressed(): boolean {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  return Array.from(pads).some((pad) => pad && pad.connected && pressedSource(pad) !== null);
}

const sourceNames: Record<string, string> = {
  south: '✕ · A', east: '○ · B', west: '□ · X', north: '△ · Y',
  left_shoulder: 'L1 · LB', right_shoulder: 'R1 · RB', left_trigger: 'L2 · LT', right_trigger: 'R2 · RT',
  back: 'Select · Back', start: 'Start', guide: 'Guide', left_stick: 'L3', right_stick: 'R3',
  dpad_up: 'D-pad ↑', dpad_down: 'D-pad ↓', dpad_left: 'D-pad ←', dpad_right: 'D-pad →',
};

// The name of a binding as the player knows it: "✕ · A", "L1 · LB + L2 · LT", "Not set".
export function padLabel(binding: string): string {
  if (!binding) return 'Not set';
  return binding.split('+').map((source) => sourceNames[source] ?? source).join(' + ');
}

// `source` pressed for `action`: no other button keeps it. A button that is left empty takes what `action` had, so
// pressing a source that is already used swaps the two.
export function assignSource(pad: Record<string, string>, action: string, source: string): Record<string, string> {
  const next = { ...pad };
  const previous = next[action] ?? '';
  for (const [other, value] of Object.entries(next)) {
    if (other === action || other === 'stick' || !value) continue;
    const rest = value.split('+').filter((part) => part !== source);
    if (rest.length === value.split('+').length) continue;
    next[other] = rest.length > 0 ? rest.join('+') : previous;
  }
  next[action] = source;
  return next;
}
