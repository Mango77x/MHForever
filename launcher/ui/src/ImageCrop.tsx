import { useEffect, useLayoutEffect, useRef, useState, type ChangeEvent, type PointerEvent as ReactPointerEvent } from 'react';
import { createPortal } from 'react-dom';
import type { Game } from './bridge';
import { setNavigationPaused } from './gamepad';

export type ArtKind = 'cover' | 'banner';

// A crop/pan window for the two pictures a game can have: the Library cover (3:4) and the Home banner (16:9).
// The result is a JPEG data URL that stays on this computer; nothing is uploaded anywhere.
export function ArtCropModal({ game, kind, initial, onClose, onSave }: {
  game: Game;
  kind: ArtKind;
  initial?: string | null;
  onClose: () => void;
  onSave: (dataUrl: string) => void;
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
  const outputW = kind === 'cover' ? 900 : 1920;
  const outputH = kind === 'cover' ? 1200 : 800;

  useEffect(() => {
    setNavigationPaused(true);
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose(); };
    window.addEventListener('keydown', onKey);
    return () => {
      setNavigationPaused(false);
      window.removeEventListener('keydown', onKey);
    };
  }, [onClose]);

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
    const file = e.target.files?.[0];
    e.target.value = '';
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
    setPan({
      x: cx - (cx - pan.x) * (newScale / oldScale),
      y: cy - (cy - pan.y) * (newScale / oldScale),
    });
    setZoom(next);
  };

  const startDrag = (e: ReactPointerEvent<HTMLDivElement>) => {
    e.currentTarget.setPointerCapture(e.pointerId);
    drag.current = { x: e.clientX, y: e.clientY, panX: x, panY: y };
  };
  const moveDrag = (e: ReactPointerEvent<HTMLDivElement>) => {
    if (!drag.current) return;
    setPan({
      x: drag.current.panX + (e.clientX - drag.current.x),
      y: drag.current.panY + (e.clientY - drag.current.y),
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
              <button className="ghost mid" onClick={() => fileRef.current?.click()}>Choose another…</button>
            </div>
            <p className="crop-help">Drag the picture to move it. It is cropped to the {kind === 'cover' ? 'card' : 'banner'} shape and stays on this computer.</p>
          </>
        ) : (
          <div className="crop-empty">
            <p>{src ? 'Reading the picture…' : 'Choose a picture for this game.'}</p>
            <button className="ghost mid" onClick={() => fileRef.current?.click()}>Choose image…</button>
          </div>
        )}

        {error && <p className="crop-error">{error}</p>}

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
