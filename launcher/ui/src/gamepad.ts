// Gamepad and arrow-key navigation: the whole launcher can be used without a mouse (docs/07-launcher-ux.md §1).
//
// D-pad / left stick (or arrow keys) move the focus spatially between controls, A activates, B goes back. Range inputs
// take left/right as value changes. Navigation is confined to the topmost open layer (an element with
// `data-nav-scope`), so menus, drawers and dialogs trap the focus like a console UI.

type Dir = 'up' | 'down' | 'left' | 'right';

const FOCUSABLE = 'button:not(:disabled), input:not(:disabled), [tabindex]:not([tabindex="-1"])';

function scopeRoot(): ParentNode {
  const scopes = Array.from(document.querySelectorAll<HTMLElement>('[data-nav-scope]'));
  return scopes.length > 0 ? scopes[scopes.length - 1] : document;
}

function visible(el: HTMLElement): boolean {
  const r = el.getBoundingClientRect();
  if (r.width === 0 || r.height === 0) return false;
  const style = getComputedStyle(el);
  return style.visibility !== 'hidden' && style.pointerEvents !== 'none';
}

function focusEl(el: HTMLElement) {
  (el as HTMLElement & { focus(o?: { focusVisible?: boolean; preventScroll?: boolean }): void }).focus({ focusVisible: true });
  el.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
}

export function move(dir: Dir) {
  const root = scopeRoot();
  const items = Array.from(root.querySelectorAll<HTMLElement>(FOCUSABLE)).filter(visible);
  if (items.length === 0) return;
  const current = document.activeElement as HTMLElement | null;
  if (!current || !items.includes(current)) {
    focusEl(items.find((i) => i.dataset.navDefault !== undefined) ?? items[0]);
    return;
  }
  if (current instanceof HTMLInputElement && current.type === 'range' && (dir === 'left' || dir === 'right')) {
    const step = Number(current.step || 1) * 5;
    const next = Math.min(Number(current.max), Math.max(Number(current.min), Number(current.value) + (dir === 'right' ? step : -step)));
    const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set;
    setter?.call(current, String(next));
    current.dispatchEvent(new Event('input', { bubbles: true }));
    current.dispatchEvent(new Event('change', { bubbles: true }));
    return;
  }
  const a = current.getBoundingClientRect();
  const ax = a.left + a.width / 2, ay = a.top + a.height / 2;
  let best: HTMLElement | null = null;
  let bestScore = Infinity;
  for (const el of items) {
    if (el === current) continue;
    const b = el.getBoundingClientRect();
    const bx = b.left + b.width / 2, by = b.top + b.height / 2;
    const dx = bx - ax, dy = by - ay;
    let primary: number, cross: number;
    if (dir === 'right') { primary = dx; cross = Math.abs(dy); }
    else if (dir === 'left') { primary = -dx; cross = Math.abs(dy); }
    else if (dir === 'down') { primary = dy; cross = Math.abs(dx); }
    else { primary = -dy; cross = Math.abs(dx); }
    if (primary <= 4) continue;
    const score = primary + cross * 2.2;
    if (score < bestScore) { bestScore = score; best = el; }
  }
  if (best) focusEl(best);
}

export function back() {
  window.dispatchEvent(new CustomEvent('mhf-back'));
}

function activate() {
  const el = document.activeElement as HTMLElement | null;
  if (el && el !== document.body) el.click();
  else move('down');
}

let started = false;
let paused = false;

// While the player is recording a key in Settings, neither the keyboard nor a pad may move the focus around.
export function setNavigationPaused(value: boolean) {
  paused = value;
}

export function startNavigation(onPadUsed: (used: boolean) => void) {
  if (started) return;
  started = true;

  window.addEventListener('keydown', (e) => {
    if (paused) return;
    const target = e.target as HTMLElement;
    const typing = target instanceof HTMLInputElement && target.type === 'text';
    const map: Record<string, Dir> = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };
    if (e.key === 'Escape') { back(); return; }
    if (typing || !(e.key in map)) return;
    if (target instanceof HTMLInputElement && target.type === 'range' && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) return;
    e.preventDefault();
    move(map[e.key]);
  });
  window.addEventListener('mousemove', () => onPadUsed(false), { passive: true });

  // Edge-triggered buttons with key repeat for directions (first repeat after 380 ms, then every 110 ms).
  const held = new Map<string, number>();
  const press = (key: string, pressed: boolean, now: number, action: () => void, repeat: boolean) => {
    if (!pressed) { held.delete(key); return; }
    const since = held.get(key);
    if (since === undefined) { held.set(key, now); action(); onPadUsed(true); return; }
    if (repeat && now - since > 380) { held.set(key, now - 380 + 110); action(); }
  };
  const loop = (now: number) => {
    if (paused) {
      held.clear();
      requestAnimationFrame(loop);
      return;
    }
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const pad of pads) {
      if (!pad || !pad.connected) continue;
      const b = (i: number) => !!pad.buttons[i]?.pressed;
      const ax = pad.axes[0] ?? 0, ay = pad.axes[1] ?? 0;
      press(pad.index + 'up', b(12) || ay < -0.55, now, () => move('up'), true);
      press(pad.index + 'down', b(13) || ay > 0.55, now, () => move('down'), true);
      press(pad.index + 'left', b(14) || ax < -0.55, now, () => move('left'), true);
      press(pad.index + 'right', b(15) || ax > 0.55, now, () => move('right'), true);
      press(pad.index + 'a', b(0), now, activate, false);
      press(pad.index + 'b', b(1), now, back, false);
    }
    requestAnimationFrame(loop);
  };
  requestAnimationFrame(loop);
}
