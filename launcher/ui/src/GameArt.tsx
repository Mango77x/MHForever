import type { CSSProperties } from 'react';
import type { Game } from './bridge';

// The launcher ships no art from the series (docs/07-launcher-ux.md §1): every scene below is original,
// abstract scenery in the launcher's flat, layered style. Each game has its own palette and silhouette,
// so copies are easy to tell apart in the Library. Freedom gets a green mountain valley; Freedom Unite
// keeps the dusk mountains it had.

type Parallax = (depth: number, scale: number) => CSSProperties;

const FREEDOM_SKY = 'linear-gradient(180deg, #1a2c21 0%, #2e4936 10%, #557257 22%, #7b9770 32%, #90a97d 45%, #9db58a 100%)';
const MHFU_HERO_SKY = 'linear-gradient(180deg, #2a2230 0%, #4a2f2a 45%, #b0613c 100%)';
const MHFU_CARD_SKY = 'linear-gradient(180deg, #2a2230, #6b3a2e 60%, #c4703f)';

function isFreedom(game?: Game): boolean {
  return game?.id === 'mhf1';
}

export function heroSky(game?: Game): string {
  return isFreedom(game) ? FREEDOM_SKY : MHFU_HERO_SKY;
}

export function cardSky(game: Game): string {
  return isFreedom(game) ? FREEDOM_SKY : MHFU_CARD_SKY;
}

export function cardBorderColor(game: Game): string | undefined {
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
      {/* Far: one background range, then the halo over it and the sun above. */}
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(12, 1.04)}>
        <path d="M0 380 L50 348 L170 220 L260 310 L370 290 L470 330 L600 188 L700 300 L820 272 L920 332 L1065 188 L1140 278 L1200 258 L1200 500 L0 500 Z" fill="#5f7b5c" />
        <circle cx="880" cy="140" r="150" fill="#ffe3a8" opacity=".12" />
        <circle cx="880" cy="140" r="75" fill="#ffe3a8" opacity=".9" />
      </svg>
      {/* Middle: the darker ridge. */}
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(26, 1.06)}>
        <path d="M0 424 L80 396 L170 414 L270 384 L370 408 L470 380 L570 406 L680 386 L790 410 L900 388 L1000 414 L1100 392 L1200 416 L1200 500 L0 500 Z" fill="#2b4633" />
      </svg>
      {/* Near: the dark foreground bank. */}
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(44, 1.08)}>
        <path d="M0 470 L90 448 L180 460 L270 438 L360 456 L450 436 L540 458 L630 440 L720 460 L810 442 L900 462 L990 444 L1080 464 L1170 446 L1200 454 L1200 500 L0 500 Z" fill="#0a1810" />
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
      <circle cx="210" cy="110" r="34" fill="#ffe3a8" />
      <path d="M0 238 L50 150 L100 240 L160 218 L210 266 L282 150 L300 184 L300 400 L0 400 Z" fill="#5f7b5c" />
      <path d="M0 310 L35 325 L90 309 L145 328 L200 310 L250 331 L300 314 L300 400 L0 400 Z" fill="#0a1810" />
    </svg>
  );
}
