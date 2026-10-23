// Keyboard bindings shown in Settings → Controls. A binding is a UI Events code ("KeyK", "ArrowLeft"...), which names
// the key's position and not the character printed on it: WASD keeps working the same way on AZERTY or QWERTZ. The
// native side stores them in launcher.json and hands them to the game (docs/07-launcher-ux.md section 5).

export interface KeyAction {
  id: string;
  label: string;
}

export const keyGroups: { title: string; actions: KeyAction[] }[] = [
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
  {
    title: 'Analog stick',
    actions: [
      { id: 'stick_up', label: 'Forward' },
      { id: 'stick_down', label: 'Back' },
      { id: 'stick_left', label: 'Left' },
      { id: 'stick_right', label: 'Right' },
    ],
  },
];

// Keys the game keeps for itself, whatever the player binds (docs/07-launcher-ux.md section 5).
export const reservedKeys: Record<string, string> = {
  Escape: 'it opens the in-game menu.',
  F11: 'it switches fullscreen.',
};

const namedKeys: Record<string, string> = {
  ArrowUp: '↑',
  ArrowDown: '↓',
  ArrowLeft: '←',
  ArrowRight: '→',
  Enter: 'Enter',
  NumpadEnter: 'Num Enter',
  Backspace: 'Backspace',
  Delete: 'Delete',
  Space: 'Space',
  Tab: 'Tab',
  CapsLock: 'Caps Lock',
  ShiftLeft: 'Left Shift',
  ShiftRight: 'Right Shift',
  ControlLeft: 'Left Ctrl',
  ControlRight: 'Right Ctrl',
  AltLeft: 'Left Alt',
  AltRight: 'Right Alt',
};

// The name of a key as the player knows it: "K", "↑", "Left Shift", "Not set".
export function keyLabel(code: string): string {
  if (!code) return 'Not set';
  if (namedKeys[code]) return namedKeys[code];
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit[0-9]$/.test(code)) return code.slice(5);
  if (/^Numpad[0-9]$/.test(code)) return `Num ${code.slice(6)}`;
  if (/^F([1-9]|1[0-2])$/.test(code)) return code;
  return code.replace(/([a-z])([A-Z])/g, '$1 $2');
}
