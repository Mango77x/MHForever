import { useEffect, useLayoutEffect, useRef, useState, type ChangeEvent, type PointerEvent as ReactPointerEvent } from 'react';
import { createPortal } from 'react-dom';
import type { Game } from './bridge';
import { markPadUsed, setNavigationPaused } from './gamepad';

export type ArtKind = 'cover' | 'banner';

// A crop/pan window for the two pictures a game can have: the Library cover (3:4) and the Home banner (16:9).
// The result is a JPEG data URL that stays on this system; nothing is uploaded anywhere.
export function ArtCropModal({ game, kind, initial, padMode, onClose, onSave, onClosed }: {
  game: Game;
  kind: ArtKind;
  initial?: string | null;
  padMode: boolean;
  onClose: () => void;
  onSave: (dataUrl: string) => void;
  onClosed?: () => void;
}) {
  const [src, setSrc] = useState<string | undefined>(initial ?? undefined);
  const [natural, setNatural] = useState<{ w: number; h: number } | null>(null);
  const [viewport, setViewport] = useState({ w: 0, h: 0 });
  const [zoom, setZoom] = useState(1);
  const [pan, setPan] = useState({ x: 0, y: 0 });
  const [error, setError] = useState('');
  const viewportRef = useRef<HTMLDivElement>(null);
  const fileRef = useRef<HTMLInputElement>(null);
  const drag = useRef<{ x: number; y: number; panX: number; panY: number } | null>(null);
  const naturalRef = useRef(natural);
  naturalRef.current = natural;
  const viewportSizeRef = useRef(viewport);
  viewportSizeRef.current = viewport;
  const filePickerGuard = useRef(false);
  const filePickerSince = useRef(0);
  const closeRef = useRef(onClose);
  closeRef.current = onClose;
  const onClosedRef = useRef(onClosed);
  onClosedRef.current = onClosed;
  const padModeRef = useRef(padMode);
  padModeRef.current = padMode;
  const outputW = kind === 'cover' ? 900 : 1920;
  const outputH = kind === 'cover' ? 1200 : 800;

  // The modal can be mounted before the game's artwork arrives from the native state; keep src in sync.
  useEffect(() => {
    setSrc(initial ?? undefined);
  }, [initial]);

  useEffect(() => {
    setNavigationPaused(true);
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') closeRef.current(); };
    window.addEventListener('keydown', onKey);
    return () => {
      const active = document.activeElement;
      if (active instanceof HTMLElement && active !== document.body) active.blur();
      window.removeEventListener('keydown', onKey);
      // Do not let the A/B press that closed the dialog activate the page behind it.
      const started = performance.now();
      const resume = () => {
        const pads = navigator.getGamepads ? navigator.getGamepads() : [];
        const pressed = pads.some((p) => p?.connected && (p.buttons[0]?.pressed || p.buttons[1]?.pressed));
        if (pressed && performance.now() - started < 2000) requestAnimationFrame(resume);
        else {
          setNavigationPaused(false);
          onClosedRef.current?.();
        }
      };
      requestAnimationFrame(resume);
    };
  }, []);

  // The native picker can leave focus on the hidden file input; a later pad A would open it again.
  useEffect(() => {
    const onFocus = () => {
      if (document.activeElement === fileRef.current) fileRef.current?.blur();
    };
    window.addEventListener('focus', onFocus);
    return () => window.removeEventListener('focus', onFocus);
  }, []);

  useEffect(() => {
    setNatural(null);
    setError('');
    if (!src) return;
    let cancelled = false;
    const img = new Image();
    img.onload = () => {
      if (!cancelled) setNatural({ w: img.naturalWidth, h: img.naturalHeight });
    };
    img.onerror = () => {
      if (!cancelled) {
        setNatural(null);
        setError('This picture could not be read. Choose another one.');
      }
    };
    img.src = src;
    return () => { cancelled = true; };
  }, [src]);

  useLayoutEffect(() => {
    const el = viewportRef.current;
    if (!el) return;
    const measure = () => {
      const r = el.getBoundingClientRect();
      setViewport({ w: r.width, h: r.height });
    };
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(el);
    return () => observer.disconnect();
  }, [src, natural]);

  useEffect(() => {
    if (!natural || !viewport.w || !viewport.h) return;
    const base = Math.max(viewport.w / natural.w, viewport.h / natural.h);
    setZoom(1);
    setPan({ x: (viewport.w - natural.w * base) / 2, y: (viewport.h - natural.h * base) / 2 });
  }, [natural, viewport.w, viewport.h]);

  const baseScale = natural ? Math.max(viewport.w / natural.w, viewport.h / natural.h) : 1;
  const scale = baseScale * zoom;
  const dw = natural ? natural.w * scale : 0;
  const dh = natural ? natural.h * scale : 0;
  const x = Math.min(0, Math.max(viewport.w - dw, pan.x));
  const y = Math.min(0, Math.max(viewport.h - dh, pan.y));

  const choose = (e: ChangeEvent<HTMLInputElement>) => {
    const input = e.currentTarget;
    const file = e.target.files?.[0];
    e.target.value = '';
    input.blur(); // the hidden input must not keep focus: a later pad A would click it again
    if (!file) return;
    if (!file.type.startsWith('image/')) {
      setError('Please choose a picture file.');
      return;
    }
    if (file.size > 25 * 1024 * 1024) {
      setError('This picture is larger than 25 MB. Choose a smaller one.');
      return;
    }
    setError('');
    const reader = new FileReader();
    reader.onload = () => setSrc(String(reader.result));
    reader.onerror = () => setError('This picture could not be read. Choose another one.');
    reader.readAsDataURL(file);
  };

  const changeZoom = (next: number) => {
    if (!natural || !viewport.w || !viewport.h) {
      setZoom(next);
      return;
    }
    const oldScale = Math.max(viewport.w / natural.w, viewport.h / natural.h) * zoom;
    const newScale = Math.max(viewport.w / natural.w, viewport.h / natural.h) * next;
    const cx = viewport.w / 2;
    const cy = viewport.h / 2;
    const nextDw = natural.w * newScale;
    const nextDh = natural.h * newScale;
    setPan({
      x: Math.min(0, Math.max(viewport.w - nextDw, cx - (cx - pan.x) * (newScale / oldScale))),
      y: Math.min(0, Math.max(viewport.h - nextDh, cy - (cy - pan.y) * (newScale / oldScale))),
    });
    setZoom(next);
  };

  const startDrag = (e: ReactPointerEvent<HTMLDivElement>) => {
    e.currentTarget.setPointerCapture(e.pointerId);
    drag.current = { x: e.clientX, y: e.clientY, panX: x, panY: y };
  };
  const moveDrag = (e: ReactPointerEvent<HTMLDivElement>) => {
    if (!drag.current) return;
    const minX = Math.min(0, viewport.w - dw);
    const minY = Math.min(0, viewport.h - dh);
    setPan({
      x: Math.min(0, Math.max(minX, drag.current.panX + (e.clientX - drag.current.x))),
      y: Math.min(0, Math.max(minY, drag.current.panY + (e.clientY - drag.current.y))),
    });
  };
  const endDrag = (e: ReactPointerEvent<HTMLDivElement>) => {
    drag.current = null;
    if (e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
  };

  const save = () => {
    if (!src || !natural || !viewport.w || !viewport.h) return;
    const img = new Image();
    img.onload = () => {
      const canvas = document.createElement('canvas');
      canvas.width = outputW;
      canvas.height = outputH;
      const ctx = canvas.getContext('2d');
      if (!ctx) return;
      ctx.fillStyle = '#0b0b0d';
      ctx.fillRect(0, 0, outputW, outputH);
      const rx = outputW / viewport.w;
      const ry = outputH / viewport.h;
      ctx.drawImage(img, x * rx, y * ry, dw * rx, dh * ry);
      onSave(canvas.toDataURL('image/jpeg', 0.9));
    };
    img.onerror = () => setError('This picture could not be saved. Choose another one.');
    img.src = src;
  };

  // Opening the picker must not be triggered again by the controller button that opened it.
  const openFilePicker = () => {
    filePickerGuard.current = true;
    filePickerSince.current = performance.now();
    fileRef.current?.click();
  };

  const remove = () => {
    if (initial) onSave('');
  };

  // Keep the latest actions for the gamepad loop, which must not restart on every render.
  const saveRef = useRef(save);
  saveRef.current = save;
  const canSaveRef = useRef(Boolean(src && natural));
  canSaveRef.current = Boolean(src && natural);
  const removeRef = useRef(remove);
  removeRef.current = remove;
  const canRemoveRef = useRef(Boolean(initial));
  canRemoveRef.current = Boolean(initial);
  const changeZoomRef = useRef(changeZoom);
  changeZoomRef.current = changeZoom;
  const zoomRef = useRef(zoom);
  zoomRef.current = zoom;

  // The launcher is gamepad-first: the crop window is usable without a mouse too.
  // Left stick / D-pad moves the picture, triggers or the right stick zoom, A saves, B cancels, Y chooses a file.
  useEffect(() => {
    let raf = 0;
    let last = performance.now();
    const edges = new Map<string, boolean>();
    const seeded = new Set<number>();
    const loop = (now: number) => {
      const dt = Math.min(0.05, Math.max(0, (now - last) / 1000));
      last = now;
      const pads = navigator.getGamepads ? navigator.getGamepads() : [];
      for (const pad of pads) {
        if (!pad || !pad.connected) continue;
        const b = (i: number) => !!pad.buttons[i]?.pressed;
        if (!seeded.has(pad.index)) {
          seeded.add(pad.index);
          edges.set(`${pad.index}:a`, b(0));
          edges.set(`${pad.index}:b`, b(1));
          edges.set(`${pad.index}:x`, b(2));
          edges.set(`${pad.index}:y`, b(3));
        }
        const edge = (key: string, down: boolean) => {
          const was = edges.get(key) ?? false;
          edges.set(key, down);
          return down && !was;
        };
        const ax = pad.axes[0] ?? 0;
        const ay = pad.axes[1] ?? 0;
        const dead = 0.22;
        const stickX = Math.abs(ax) > dead ? ax : 0;
        const stickY = Math.abs(ay) > dead ? ay : 0;
        const dpadX = (b(15) ? 1 : 0) - (b(14) ? 1 : 0);
        const dpadY = (b(13) ? 1 : 0) - (b(12) ? 1 : 0);
        const mx = stickX !== 0 ? stickX : dpadX;
        const my = stickY !== 0 ? stickY : dpadY;
        if (mx !== 0 || my !== 0) {
          const nat = naturalRef.current;
          const vp = viewportSizeRef.current;
          if (nat && vp.w && vp.h) {
            const base = Math.max(vp.w / nat.w, vp.h / nat.h);
            const dw = nat.w * base * zoomRef.current;
            const dh = nat.h * base * zoomRef.current;
            const minX = Math.min(0, vp.w - dw);
            const minY = Math.min(0, vp.h - dh);
            setPan((p) => ({
              x: Math.min(0, Math.max(minX, p.x + mx * 420 * dt)),
              y: Math.min(0, Math.max(minY, p.y + my * 420 * dt)),
            }));
          }
        }
        const rz = pad.axes[3] ?? 0;
        const zoomIn = b(7) || b(5) || rz < -0.3;
        const zoomOut = b(6) || b(4) || rz > 0.3;
        if ((mx !== 0 || my !== 0 || zoomIn || zoomOut || b(0) || b(1) || b(2) || b(3) || b(4) || b(5) || b(6) || b(7)) && !padModeRef.current) {
          markPadUsed(true);
        }
        if (zoomIn || zoomOut) {
          const next = Math.min(3, Math.max(1, zoomRef.current + (zoomIn ? 1 : -1) * 0.9 * dt));
          if (Math.abs(next - zoomRef.current) > 0.0005) {
            zoomRef.current = next;
            changeZoomRef.current(next);
          }
        }
        const aDown = b(0);
        const yDown = b(3);
        const xDown = b(2);
        if (filePickerGuard.current) {
          if ((!aDown && !yDown && !xDown) || performance.now() - filePickerSince.current > 30000) {
            filePickerGuard.current = false;
          }
        } else {
          if (edge(`${pad.index}:a`, aDown)) {
            if (canSaveRef.current) saveRef.current();
            else openFilePicker();
          }
          if (edge(`${pad.index}:y`, yDown)) openFilePicker();
          if (edge(`${pad.index}:x`, xDown) && canRemoveRef.current) removeRef.current();
        }
        if (edge(`${pad.index}:b`, b(1))) closeRef.current();
      }
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, []);

  return createPortal(
    <div className="scrim crop-scrim" onClick={onClose}>
      <div className="crop-modal glass" role="dialog" aria-modal="true" aria-label={kind === 'cover' ? 'Library cover' : 'Home banner'} onClick={(e) => e.stopPropagation()}>
        <div className="crop-head">
          <div>
            <h3>{kind === 'cover' ? 'Library cover' : 'Home banner'}</h3>
            <span>{game.title}</span>
          </div>
          <button className="ghost mid" onClick={onClose}>Close</button>
        </div>

        <input ref={fileRef} type="file" accept="image/*" hidden onChange={choose} />

        {src && natural ? (
          <>
            <div
              ref={viewportRef}
              className={`crop-viewport ${kind}`}
              style={{ aspectRatio: kind === 'cover' ? '3 / 4' : '12 / 5' }}
              onPointerDown={startDrag}
              onPointerMove={moveDrag}
              onPointerUp={endDrag}
              onPointerCancel={endDrag}
            >
              <img
                src={src}
                alt=""
                draggable={false}
                style={{ width: dw, height: dh, transform: `translate(${x}px, ${y}px)` }}
              />
              <div className="crop-grid" aria-hidden="true" />
            </div>
            <div className="crop-controls">
              <label>
                <span>Zoom</span>
                <input
                  className="rng"
                  type="range"
                  min={1}
                  max={3}
                  step={0.01}
                  value={zoom}
                  style={{ ['--v' as string]: `${((zoom - 1) / 2) * 100}%` }}
                  onChange={(e) => changeZoom(Number(e.target.value))}
                />
              </label>
              <button className="ghost mid" onClick={openFilePicker}>Choose another…</button>
            </div>
            <p className="crop-help">Drag the picture to move it. It is cropped to the {kind === 'cover' ? 'card' : 'banner'} shape and stays on this system.</p>
          </>
        ) : (
          <div className="crop-empty">
            <p>{src ? 'Reading the picture…' : 'Choose a picture for this game.'}</p>
            <button className="ghost mid" onClick={openFilePicker}>Choose image…</button>
          </div>
        )}

        {error && <p className="crop-error">{error}</p>}

        {padMode && (
          <p className="crop-pad-hint">
            <span><kbd>L</kbd> Move</span>
            <span><kbd>LT/RT</kbd> Zoom</span>
            <span><kbd>A</kbd> Save</span>
            <span><kbd>B</kbd> Cancel</span>
            {initial && <span><kbd>X</kbd> Remove</span>}
            <span><kbd>Y</kbd> Choose image</span>
          </p>
        )}

        <div className="crop-actions">
          {initial && <button className="ghost mid danger" style={{ marginRight: 'auto' }} onClick={() => onSave('')}>Remove</button>}
          <button className="ghost mid" onClick={onClose}>Cancel</button>
          <button className="play small" disabled={!src || !natural} onClick={save}>Save</button>
        </div>
      </div>
    </div>,
    document.body,
  );
}
