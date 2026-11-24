import { useEffect, useState, type CSSProperties } from 'react';
import type { Game } from './bridge';

// The launcher ships no art from the series (docs/07-launcher-ux.md §1): every scene below is original,
// abstract scenery in the launcher's flat, layered style. Freedom gets its own blue mountain valley. Freedom
// Unite, and any other PSP game, get the same mountain range with a palette that follows the game's icon.

type Parallax = (depth: number, scale: number) => CSSProperties;

type Palette = { sky: string; far: string; mid: string; near: string; sun: string; border: string };

const FREEDOM_SKY = 'linear-gradient(180deg, #0a1d2e 0%, #12384c 10%, #1c4f68 22%, #2e7089 32%, #5b94ab 45%, #a3c2cf 100%)';
const MHFU_HERO_SKY = 'linear-gradient(180deg, #0b1d14 0%, #2a5f3b 40%, #d0eaa2 100%)';
const MHFU_CARD_SKY = 'linear-gradient(180deg, #0b1d14, #3b7248 55%, #d0eaa2)';

const MHFU_HERO: Palette = {
  sky: MHFU_HERO_SKY, far: '#4f9166', mid: '#2f6b42', near: '#123a22', sun: '#ffd9a0', border: 'rgba(124, 174, 131, 0.6)',
};
const MHFU_CARD: Palette = { ...MHFU_HERO, sky: MHFU_CARD_SKY };

function isFreedom(game?: Game): boolean {
  return game?.id === 'mhf1';
}

function isFreedomUnite(game?: Game): boolean {
  return game?.id === 'mhfu';
}

function rgbOf(accent?: string): [number, number, number] | undefined {
  if (!accent) return undefined;
  const m = accent.match(/rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/i);
  return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : undefined;
}

function rgbToHsl(r: number, g: number, b: number): [number, number, number] {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b);
  const l = (max + min) / 2;
  if (max === min) return [0, 0, l];
  const d = max - min;
  const s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
  let h = 0;
  if (max === r) h = (g - b) / d + (g < b ? 6 : 0);
  else if (max === g) h = (b - r) / d + 2;
  else h = (r - g) / d + 4;
  return [h * 60, s, l];
}

function hsl(h: number, s: number, l: number, a = 1): string {
  return `hsla(${Math.round(h)}, ${Math.round(s)}%, ${Math.round(l)}%, ${a})`;
}

function accentBorder(accent?: string): string {
  const rgb = rgbOf(accent);
  if (!rgb) return MHFU_HERO.border;
  const [h, s] = rgbToHsl(...rgb);
  return hsl(h, Math.max(35, Math.min(75, s * 100)), 65, 0.6);
}

function paletteFromAccent(accent: string | undefined, kind: 'hero' | 'card'): Palette {
  const rgb = rgbOf(accent);
  const [h, rawS] = rgb ? rgbToHsl(...rgb) : [135, 0.35];
  const s = Math.max(0.3, Math.min(0.7, rawS));
  const sky = kind === 'hero'
    ? `linear-gradient(180deg, ${hsl(h, s * 55, 8)} 0%, ${hsl(h, s * 60, 18)} 45%, ${hsl(h, s * 65, 58)} 100%)`
    : `linear-gradient(180deg, ${hsl(h, s * 55, 8)}, ${hsl(h, s * 60, 28)} 60%, ${hsl(h, s * 65, 62)})`;
  return {
    sky,
    far: hsl(h, s * 55, 36),
    mid: hsl(h, s * 60, 22),
    near: hsl(h, s * 65, 10),
    sun: hsl((h + 35) % 360, 60, 82),
    border: accentBorder(accent),
  };
}

// A game's own accent: the uploaded picture when there is one, otherwise the icon of a game we have no palette for.
// Freedom and Freedom Unite keep their tuned palettes, so their icons do not override them.
export function gameAccent(game: Game | undefined, imageAccent?: string, iconAccent?: string): string | undefined {
  if (imageAccent) return imageAccent;
  if (!game || isFreedom(game) || isFreedomUnite(game)) return undefined;
  return iconAccent;
}

const accentCache = new Map<string, string>();

// The average of the vivid pixels of an icon or uploaded picture, for the fallback palette and the card edge.
export function useImageAccent(src?: string | null): string | undefined {
  const [accent, setAccent] = useState<string | undefined>(() => (src ? accentCache.get(src) : undefined));
  useEffect(() => {
    if (!src) {
      setAccent(undefined);
      return;
    }
    const cached = accentCache.get(src);
    if (cached) {
      setAccent(cached);
      return;
    }
    let cancelled = false;
    const img = new Image();
    img.decoding = 'async';
    img.onload = () => {
      if (cancelled) return;
      try {
        const size = 32;
        const canvas = document.createElement('canvas');
        canvas.width = size;
        canvas.height = size;
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        if (!ctx) return;
        ctx.drawImage(img, 0, 0, size, size);
        const data = ctx.getImageData(0, 0, size, size).data;
        let r = 0, g = 0, b = 0, n = 0;
        for (let i = 0; i < data.length; i += 4) {
          if (data[i + 3] < 128) continue;
          const rr = data[i], gg = data[i + 1], bb = data[i + 2];
          const max = Math.max(rr, gg, bb), min = Math.min(rr, gg, bb);
          if (max < 24 || min > 240) continue; // ignore black borders and white paper
          const weight = 1 + ((max - min) / (max || 1)) * 3; // a vivid logo wins over a dull background
          r += rr * weight;
          g += gg * weight;
          b += bb * weight;
          n += weight;
        }
        if (n === 0) return;
        const color = `rgb(${Math.round(r / n)}, ${Math.round(g / n)}, ${Math.round(b / n)})`;
        accentCache.set(src, color);
        setAccent(color);
      } catch {
        // A picture the browser cannot read keeps the drawn palette.
      }
    };
    img.onerror = () => {
      if (!cancelled) setAccent(undefined);
    };
    img.src = src;
    return () => { cancelled = true; };
  }, [src]);
  return accent;
}

export function heroSky(game?: Game, accent?: string): string {
  if (isFreedom(game)) return FREEDOM_SKY;
  if (isFreedomUnite(game)) return MHFU_HERO_SKY;
  return paletteFromAccent(accent, 'hero').sky;
}

export function cardSky(game: Game, accent?: string): string {
  if (isFreedom(game)) return FREEDOM_SKY;
  if (isFreedomUnite(game)) return MHFU_CARD_SKY;
  return paletteFromAccent(accent, 'card').sky;
}

export function cardBorderColor(game: Game, accent?: string): string {
  if (accent) return accentBorder(accent);
  if (isFreedom(game)) return 'rgba(126, 175, 214, 0.6)';
  if (isFreedomUnite(game)) return MHFU_HERO.border;
  return accentBorder(undefined);
}

/** The wide banner's edge follows its picture, or its own drawn palette when there is none. */
export function heroBorderColor(game: Game | undefined, accent?: string): string | undefined {
  return game ? cardBorderColor(game, accent) : undefined;
}

/** The wide scene behind the game's name on Home. */
export function HeroScenery({ game, layer, accent }: { game?: Game; layer: Parallax; accent?: string }) {
  if (isFreedom(game)) {
    return (
      <>
        {/* Far: one background range, then the halo over it and the sun above. */}
        <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(12, 1.04)}>
          <path d="M0 380 L50 348 L170 220 L260 310 L370 290 L470 330 L600 188 L700 300 L820 272 L920 332 L1065 188 L1140 278 L1200 258 L1200 500 L0 500 Z" fill="#2f5d74" />
          <circle cx="880" cy="140" r="150" fill="#ffe3a8" opacity=".12" />
          <circle cx="880" cy="140" r="75" fill="#ffe3a8" opacity=".9" />
        </svg>
        {/* Middle: the darker ridge. */}
        <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(26, 1.06)}>
          <path d="M0 424 L80 396 L170 414 L270 384 L370 408 L470 380 L570 406 L680 386 L790 410 L900 388 L1000 414 L1100 392 L1200 416 L1200 500 L0 500 Z" fill="#1b3e50" />
        </svg>
        {/* Near: the dark foreground bank. */}
        <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(44, 1.08)}>
          <path d="M0 470 L90 448 L180 460 L270 438 L360 456 L450 436 L540 458 L630 440 L720 460 L810 442 L900 462 L990 444 L1080 464 L1170 446 L1200 454 L1200 500 L0 500 Z" fill="#0a1822" />
        </svg>
      </>
    );
  }

  const p = isFreedomUnite(game) ? MHFU_HERO : paletteFromAccent(accent, 'hero');
  return (
    <>
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(12, 1.04)}>
        <circle cx="880" cy="150" r="70" fill={p.sun} opacity=".9" />
        <circle cx="880" cy="150" r="130" fill={p.sun} opacity=".12" />
        <path d="M0 330 L160 210 L280 290 L420 150 L560 280 L700 190 L860 300 L1000 170 L1200 280 L1200 500 L0 500 Z" fill={p.far} opacity=".85" />
      </svg>
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(26, 1.06)}>
        <path d="M0 390 L140 300 L250 360 L390 250 L520 350 L640 290 L780 380 L930 280 L1080 360 L1200 320 L1200 500 L0 500 Z" fill={p.mid} />
        <path d="M390 250 L420 272 L405 270 L390 262 L372 270 Z" fill={p.sun} opacity=".7" />
        <path d="M930 280 L955 300 L940 297 L930 290 L914 298 Z" fill={p.sun} opacity=".6" />
      </svg>
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(44, 1.08)}>
        <path d="M0 440 L120 400 L230 430 L360 380 L470 425 L610 395 L760 440 L900 410 L1050 445 L1200 415 L1200 500 L0 500 Z" fill={p.near} />
      </svg>
    </>
  );
}

/** The portrait scene on a Library card. */
export function CardScenery({ game, accent }: { game: Game; accent?: string }) {
  if (isFreedom(game)) {
    return (
      <svg viewBox="0 0 300 400" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
        <circle cx="210" cy="110" r="34" fill="#ffe3a8" />
        <path d="M0 238 L50 150 L100 240 L160 218 L210 266 L282 150 L300 184 L300 400 L0 400 Z" fill="#2f5d74" />
        <path d="M0 310 L35 325 L90 309 L145 328 L200 310 L250 331 L300 314 L300 400 L0 400 Z" fill="#0a1822" />
      </svg>
    );
  }

  const p = isFreedomUnite(game) ? MHFU_CARD : paletteFromAccent(accent, 'card');
  return (
    <svg viewBox="0 0 300 400" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
      <circle cx="210" cy="110" r="34" fill={p.sun} opacity=".85" />
      <path d="M0 270 L60 200 L110 245 L170 160 L230 240 L300 190 L300 400 L0 400 Z" fill={p.far} />
      <path d="M0 330 L70 290 L140 320 L210 285 L300 320 L300 400 L0 400 Z" fill={p.near} />
    </svg>
  );
}
