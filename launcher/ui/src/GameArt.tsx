import type { CSSProperties } from 'react';
import type { Game } from './bridge';

// The launcher ships no art from the series (docs/07-launcher-ux.md §1): every scene below is original,
// abstract scenery in the launcher's flat, layered style. Each game has its own palette and silhouette,
// so copies are easy to tell apart in the Library. Freedom gets a green forest valley; Freedom Unite
// keeps the dusk mountains it had.

type Parallax = (depth: number, scale: number) => CSSProperties;

const FREEDOM_HERO_SKY = 'linear-gradient(180deg, #0c2628 0%, #2a5946 42%, #d9a55f 100%)';
const FREEDOM_CARD_SKY = 'linear-gradient(180deg, #0c2628 0%, #2a5946 58%, #b18450 100%)';
const MHFU_HERO_SKY = 'linear-gradient(180deg, #2a2230 0%, #4a2f2a 45%, #b0613c 100%)';
const MHFU_CARD_SKY = 'linear-gradient(180deg, #2a2230, #6b3a2e 60%, #c4703f)';

export function isFreedom(game?: Game): boolean {
  return game?.id === 'mhf1';
}

export function heroSky(game?: Game): string {
  return isFreedom(game) ? FREEDOM_HERO_SKY : MHFU_HERO_SKY;
}

export function cardSky(game: Game): string {
  return isFreedom(game) ? FREEDOM_CARD_SKY : MHFU_CARD_SKY;
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
      {/* Far: dawn sun, the distant ridge and a little morning mist. */}
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(12, 1.04)}>
        <circle cx="904" cy="150" r="70" fill="#ffe7b5" opacity=".95" />
        <circle cx="904" cy="150" r="144" fill="#ffe7b5" opacity=".13" />
        <path d="M0 330 L110 262 L188 296 L300 190 L396 264 L494 218 L600 306 L716 204 L812 274 L918 226 L1032 306 L1122 256 L1200 292 L1200 500 L0 500 Z" fill="#3b6655" opacity=".95" />
        <path d="M716 204 L742 226 L727 224 L716 215 L700 224 Z" fill="#eaf1dd" opacity=".44" />
        <path d="M1102 258 L1122 276 L1110 274 L1102 266 L1090 274 Z" fill="#eaf1dd" opacity=".3" />
        <ellipse cx="560" cy="326" rx="340" ry="25" fill="#d7e8d5" opacity=".08" />
        <path d="M690 116 q11 -9 22 0 M716 106 q9 -7 18 0 M664 132 q8 -7 16 0" stroke="#f7efdf" strokeWidth="3" fill="none" opacity=".55" strokeLinecap="round" />
      </svg>
      {/* Middle: forested hills around a stream, with the light catching the water. */}
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(26, 1.06)}>
        <path d="M0 384 C70 352 138 330 212 342 C290 354 336 386 400 392 C448 396 480 408 504 426 L504 500 L0 500 Z" fill="#2a5b45" />
        <path d="M1200 346 C1132 320 1058 314 986 332 C912 350 866 384 812 394 C780 399 756 408 738 426 L738 500 L1200 500 Z" fill="#224d3b" />
        <path d="M786 330 C770 356 742 370 724 390 C704 412 706 434 688 452 C668 472 640 482 620 500 L842 500 C822 478 818 456 834 436 C850 414 878 400 890 378 C900 358 894 342 878 332 Z" fill="#63918b" opacity=".82" />
        <path d="M796 340 C780 364 754 378 738 398 C720 420 722 438 704 458" stroke="#b9d4c8" strokeWidth="2" fill="none" opacity=".35" />
        <g stroke="#f5dda8" strokeWidth="2.5" strokeLinecap="round" opacity=".42">
          <path d="M700 470 h32" />
          <path d="M756 450 h38" />
          <path d="M716 490 h24" />
          <path d="M784 478 h26" />
        </g>
        <g fill="#173a2b">
          <path d="M38 374 l9 -26 9 26 z M54 366 l11 -30 11 30 z M74 378 l8 -22 8 22 z M92 368 l12 -34 12 34 z M112 376 l9 -24 9 24 z" />
          <path d="M154 352 l11 -32 11 32 z M172 360 l8 -22 8 22 z M190 348 l13 -38 13 38 z M210 362 l9 -26 9 26 z M230 354 l11 -30 11 30 z M248 366 l8 -22 8 22 z" />
          <path d="M298 366 l10 -28 10 28 z M316 374 l8 -22 8 22 z M334 364 l12 -34 12 34 z M354 372 l9 -24 9 24 z" />
          <path d="M880 376 l9 -26 9 26 z M896 368 l12 -34 12 34 z M918 380 l8 -22 8 22 z M934 368 l13 -38 13 38 z M956 380 l9 -24 9 24 z" />
          <path d="M990 340 l11 -32 11 32 z M1008 348 l9 -26 9 26 z M1026 336 l13 -38 13 38 z M1048 350 l10 -28 10 28 z M1068 342 l8 -22 8 22 z M1086 354 l11 -30 11 30 z" />
        </g>
        <g stroke="#1b4534" strokeWidth="3" strokeLinecap="round" fill="none">
          <path d="M586 500 q1 -22 -7 -36 M602 500 q0 -18 8 -32 M618 500 q-2 -20 -10 -34" />
          <path d="M862 500 q-1 -22 7 -36 M846 500 q0 -18 -8 -32 M830 500 q2 -20 10 -34" />
        </g>
      </svg>
      {/* Near: dark banks, framing pines and a few fireflies in the shade. */}
      <svg className="layer" viewBox="0 0 1200 500" preserveAspectRatio="xMidYMax slice" aria-hidden="true" style={layer(44, 1.08)}>
        <path d="M0 452 C90 424 172 430 252 454 C330 478 388 482 448 472 C500 464 528 478 560 486 L560 500 L0 500 Z" fill="#0e221a" />
        <path d="M1200 438 C1128 416 1064 422 1002 444 C950 462 914 474 878 480 C934 492 1052 500 1200 500 Z" fill="#0a1a14" />
        <g fill="#091712">
          <path d="M142 300 L176 384 L160 380 L184 452 L100 452 L124 380 L108 384 Z" />
          <path d="M92 338 L118 406 L106 402 L126 466 L58 466 L78 402 L66 406 Z" />
          <path d="M196 356 L220 416 L208 412 L228 474 L164 474 L184 412 L172 416 Z" />
          <path d="M1126 290 L1160 374 L1144 370 L1168 442 L1084 442 L1108 370 L1092 374 Z" />
          <path d="M1076 328 L1102 396 L1090 392 L1110 456 L1042 456 L1062 392 L1050 396 Z" />
          <path d="M1180 346 L1204 406 L1192 402 L1212 464 L1148 464 L1168 402 L1156 406 Z" />
        </g>
        <g stroke="#0b1b15" strokeWidth="3" strokeLinecap="round" fill="none">
          <path d="M18 500 q2 -34 -8 -54 M38 500 q0 -26 10 -46 M58 500 q-2 -30 -12 -48 M78 500 q1 -22 -6 -38" />
          <path d="M1140 500 q-2 -34 8 -54 M1160 500 q0 -26 -10 -46 M1180 500 q2 -30 12 -48" />
        </g>
        <g fill="#ffdf9e">
          <circle cx="368" cy="438" r="2.6" opacity=".9" />
          <circle cx="398" cy="456" r="1.9" opacity=".6" />
          <circle cx="430" cy="432" r="2.2" opacity=".75" />
          <circle cx="350" cy="468" r="1.7" opacity=".5" />
          <circle cx="460" cy="454" r="1.5" opacity=".5" />
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
      <circle cx="208" cy="106" r="34" fill="#ffe7b5" opacity=".95" />
      <circle cx="208" cy="106" r="64" fill="#ffe7b5" opacity=".14" />
      <path d="M0 266 L52 210 L96 244 L154 172 L206 236 L246 202 L300 242 L300 400 L0 400 Z" fill="#3b6655" opacity=".95" />
      <path d="M154 172 L174 190 L164 188 L154 180 L143 188 Z" fill="#eaf1dd" opacity=".34" />
      <path d="M0 320 C24 302 48 296 74 304 C102 312 118 330 142 334 C156 336 166 346 174 360 L174 400 L0 400 Z" fill="#2a5b45" />
      <path d="M300 300 C276 288 252 286 230 296 C208 306 194 324 178 332 C164 338 154 350 148 364 L148 400 L300 400 Z" fill="#224d3b" />
      <path d="M174 258 C168 276 152 286 146 302 C140 320 148 336 138 352 C128 370 108 382 98 400 L196 400 C184 380 184 360 196 344 C208 326 222 314 224 296 C226 280 216 266 206 258 Z" fill="#63918b" opacity=".8" />
      <path d="M180 268 C174 286 158 296 152 312 C146 330 154 344 144 360" stroke="#b9d4c8" strokeWidth="1.8" fill="none" opacity=".35" />
      <g stroke="#f5dda8" strokeWidth="2" strokeLinecap="round" opacity=".42">
        <path d="M122 348 h18" />
        <path d="M158 334 h20" />
        <path d="M142 368 h14" />
        <path d="M176 356 h15" />
      </g>
      <g fill="#173a2b">
        <path d="M14 316 l6 -18 6 18 z M26 310 l8 -22 8 22 z M42 318 l5 -14 5 14 z M54 310 l9 -26 9 26 z M70 318 l6 -18 6 18 z" />
        <path d="M230 306 l6 -18 6 18 z M242 298 l8 -22 8 22 z M258 308 l5 -14 5 14 z M270 300 l9 -26 9 26 z M286 310 l6 -18 6 18 z" />
      </g>
      <g fill="#091712">
        <path d="M270 236 L300 320 L286 316 L306 386 L236 386 L258 316 L244 320 Z" />
        <path d="M230 286 L252 346 L242 342 L258 400 L202 400 L220 342 L210 346 Z" opacity=".92" />
        <path d="M294 300 L312 348 L304 345 L316 390 L270 390 L282 345 L274 348 Z" opacity=".8" />
      </g>
      <path d="M0 356 C24 344 50 344 74 356 C92 364 104 374 114 384 L114 400 L0 400 Z" fill="#0e221a" />
      <path d="M300 356 C280 346 260 348 242 358 C226 368 216 378 208 388 L208 400 L300 400 Z" fill="#0a1a14" />
      <g stroke="#0b1b15" strokeWidth="2" strokeLinecap="round" fill="none">
        <path d="M12 400 q1 -20 -5 -32 M24 400 q0 -16 6 -28 M36 400 q-1 -18 -7 -29" />
      </g>
      <g fill="#ffdf9e">
        <circle cx="110" cy="350" r="1.6" opacity=".8" />
        <circle cx="126" cy="362" r="1.2" opacity=".55" />
        <circle cx="98" cy="368" r="1.1" opacity=".5" />
      </g>
    </svg>
  );
}
