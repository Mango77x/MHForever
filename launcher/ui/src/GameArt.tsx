import type { CSSProperties } from 'react';
import type { Game } from './bridge';

// The launcher ships no art from the series (docs/07-launcher-ux.md §1): every scene below is original,
// abstract scenery in the launcher's flat, layered style. Each game has its own palette and silhouette,
// so copies are easy to tell apart in the Library. Freedom gets a green, pine-covered mountain valley;
// Freedom Unite keeps the dusk mountains it had.

type Parallax = (depth: number, scale: number) => CSSProperties;

type PineProps = { x: number; y: number; width: number; height: number };

const FREEDOM_SKY = 'linear-gradient(180deg, #1a2c21 0%, #2e4936 10%, #557257 22%, #7b9770 32%, #90a97d 45%, #9db58a 100%)';
const MHFU_HERO_SKY = 'linear-gradient(180deg, #2a2230 0%, #4a2f2a 45%, #b0613c 100%)';
const MHFU_CARD_SKY = 'linear-gradient(180deg, #2a2230, #6b3a2e 60%, #c4703f)';

const HERO_BACK_PINES: PineProps[] = [
  { x: 500, y: 330, width: 22, height: 110 },
  { x: 532, y: 340, width: 24, height: 110 },
  { x: 566, y: 335, width: 20, height: 105 },
  { x: 600, y: 345, width: 26, height: 115 },
  { x: 638, y: 340, width: 22, height: 105 },
  { x: 676, y: 350, width: 24, height: 100 },
  { x: 716, y: 345, width: 20, height: 100 },
  { x: 754, y: 355, width: 18, height: 90 },
];

const HERO_FRONT_PINES: PineProps[] = [
  { x: 18, y: 300, width: 36, height: 200 },
  { x: 70, y: 320, width: 40, height: 180 },
  { x: 126, y: 360, width: 30, height: 140 },
  { x: 640, y: 390, width: 28, height: 110 },
  { x: 1068, y: 300, width: 36, height: 200 },
  { x: 1120, y: 340, width: 32, height: 160 },
  { x: 1164, y: 380, width: 24, height: 120 },
];

const CARD_BACK_PINES: PineProps[] = [
  { x: 102, y: 240, width: 20, height: 110 },
  { x: 124, y: 246, width: 22, height: 112 },
  { x: 148, y: 250, width: 18, height: 104 },
  { x: 172, y: 242, width: 22, height: 118 },
  { x: 198, y: 252, width: 18, height: 104 },
  { x: 222, y: 262, width: 16, height: 94 },
];

const CARD_FRONT_PINES: PineProps[] = [
  { x: 9, y: 198, width: 30, height: 180 },
  { x: 45, y: 220, width: 34, height: 168 },
  { x: 88, y: 255, width: 26, height: 136 },
  { x: 215, y: 205, width: 32, height: 176 },
  { x: 250, y: 250, width: 26, height: 138 },
  { x: 277, y: 290, width: 18, height: 100 },
];

/** One stylised pine, drawn in a 100 x 110 box and stretched to its place in the scene. */
function Pine({ x, y, width, height }: PineProps) {
  return (
    <path
      d="M50 0 L78 52 L62 52 L100 96 L64 96 L64 110 L36 110 L36 96 L0 96 L38 52 L22 52 Z"
      transform={`translate(${x} ${y}) scale(${width / 100} ${height / 110})`}
    />
  );
}

export function isFreedom(game?: Game): boolean {
  return game?.id === 'mhf1';
}

export function heroSky(game?: Game): string {
  return isFreedom(game) ? FREEDOM_SKY : MHFU_HERO_SKY;
}

export function cardSky(game: Game): string {
  return isFreedom(game) ? FREEDOM_SKY : MHFU_CARD_SKY;
}

export function cardBorder(game: Game): string | undefined {
  return isFreedom(game) ? 'rgba(190, 210, 180, 0.35)' : undefined;
}

/** The wide scene behind the game's name on Home. */
export function HeroScenery({ game, layer }: { game?: Game; layer: Parallax }) {
  if (!isFreedom(game)) {
    return (
      <>
        <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(12, 1.04)}>
          <circle cx="880" cy="150" r="70" fill="#ffd9a0" opacity=".9" />
          <circle cx="880" cy="150" r="130" fill="#ffd9a0" opacity=".12" />
          <path d="M0 330 L160 210 L280 290 L420 150 L560 280 L700 190 L860 300 L1000 170 L1200 280 L1200 500 L0 500 Z" fill="#3b2a3a" opacity=".85" />
        </svg>
        <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(26, 1.06)}>
          <path d="M0 390 L140 300 L250 360 L390 250 L520 350 L640 290 L780 380 L930 280 L1080 360 L1200 320 L1200 500 L0 500 Z" fill="#26192a" />
          <path d="M390 250 L420 272 L405 270 L390 262 L372 270 Z" fill="#f4efe6" opacity=".7" />
          <path d="M930 280 L955 300 L940 297 L930 290 L914 298 Z" fill="#f4efe6" opacity=".6" />
        </svg>
        <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(44, 1.08)}>
          <path d="M0 440 L120 400 L230 430 L360 380 L470 425 L610 395 L760 440 L900 410 L1050 445 L1200 415 L1200 500 L0 500 Z" fill="#140e16" />
        </svg>
      </>
    );
  }

  return (
    <>
      {/* Far: the sun, flat and without a halo, the left ridge and its rock spires. */}
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(12, 1.04)}>
        <circle cx="860" cy="150" r="75" fill="#ffe3a8" />
        <path d="M0 380 L30 330 L90 240 L160 300 L240 330 L330 338 L420 330 L520 350 L620 370 L720 390 L800 420 L800 500 L0 500 Z" fill="#4e6a4e" />
        <path d="M186 312 L194 250 L202 312 Z M268 334 L276 270 L284 334 Z M350 342 L356 300 L362 342 Z" fill="#4e6a4e" />
      </svg>
      {/* Middle: the light slope on the right, then the dark ridge with its pines. */}
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(26, 1.06)}>
        <path d="M1200 228 L1080 286 L1000 326 L940 372 L900 430 L880 500 L1200 500 Z" fill="#adc293" />
        <path d="M1006 326 L1014 246 L1022 326 Z" fill="#adc293" />
        <path d="M0 420 L80 392 L160 410 L250 382 L340 402 L430 380 L520 404 L620 386 L720 408 L820 390 L920 412 L1020 394 L1120 416 L1200 402 L1200 500 L0 500 Z" fill="#2b4633" />
        <g fill="#0d1d12">
          {HERO_BACK_PINES.map((p) => <Pine key={p.x} {...p} />)}
        </g>
      </svg>
      {/* Near: the dark foreground bank and the tall pines that frame it. */}
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(44, 1.08)}>
        <path d="M0 460 L80 438 L160 452 L250 430 L340 448 L430 428 L520 450 L620 432 L720 452 L820 434 L920 454 L1020 436 L1120 456 L1200 444 L1200 500 L0 500 Z" fill="#0a1810" />
        <g fill="#08150d">
          {HERO_FRONT_PINES.map((p) => <Pine key={p.x} {...p} />)}
        </g>
      </svg>
    </>
  );
}

/** The portrait scene on a Library card. */
export function CardScenery({ game }: { game: Game }) {
  if (!isFreedom(game)) {
    return (
      <svg viewBox="0 0 300 400" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
        <circle cx="210" cy="110" r="34" fill="#ffd9a0" opacity=".85" />
        <path d="M0 270 L60 200 L110 245 L170 160 L230 240 L300 190 L300 400 L0 400 Z" fill="#2c1d2b" />
        <path d="M0 330 L70 290 L140 320 L210 285 L300 320 L300 400 L0 400 Z" fill="#160f17" />
      </svg>
    );
  }

  return (
    <svg viewBox="0 0 300 400" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
      <circle cx="178" cy="110" r="30" fill="#ffe3a8" />
      <path d="M0 244 L20 202 L38 150 L64 176 L88 190 L112 198 L138 192 L164 206 L190 216 L218 228 L248 238 L274 250 L300 262 L300 400 L0 400 Z" fill="#4e6a4e" />
      <path d="M54 180 L60 134 L66 180 Z M82 192 L88 146 L94 192 Z M108 200 L113 164 L118 200 Z" fill="#4e6a4e" />
      <path d="M300 164 L246 204 L214 232 L196 274 L178 330 L166 400 L300 400 Z" fill="#adc293" />
      <path d="M216 236 L221 180 L226 236 Z" fill="#adc293" />
      <path d="M0 292 L42 276 L86 290 L128 264 L174 288 L218 270 L260 288 L300 268 L300 400 L0 400 Z" fill="#2b4633" />
      <g fill="#0d1d12">
        {CARD_BACK_PINES.map((p) => <Pine key={p.x} {...p} />)}
      </g>
      <path d="M0 316 L38 294 L82 306 L126 286 L170 302 L214 290 L258 308 L300 296 L300 400 L0 400 Z" fill="#0a1810" />
      <g fill="#08150d">
        {CARD_FRONT_PINES.map((p) => <Pine key={p.x} {...p} />)}
      </g>
    </svg>
  );
}
