// Bridge between the UI and the native launcher (core/launcher/native/bridge.cpp).
//
// The native side binds `mhf_*` functions on `window` (each returns a Promise of JSON) and pushes events by calling
// `window.__mhfEvent(event)`. Opened in a plain browser (`npm run dev`), a simulated backend stands in so the UI can be
// designed without the native host.

export type Verification = 'verified' | 'unverified';

export interface Game {
  id: string;
  title: string;
  region: string;
  discId: string;
  version: string;
  isoPath: string;
  verification: Verification;
  supported: boolean;
}

export interface Settings {
  scale: number; // 1..4
  fullscreen: boolean;
  smoothTextures: boolean;
  fps: number; // 30..180, a multiple of 30
  copyImage: boolean; // keep a copy of the disc image in MHForever's own folder
  musicVolume: number; // 0..100
  sfxVolume: number; // 0..100
  // Which physical key stands for which pad button: UI Events codes ("KeyK", "ArrowLeft"...), '' when unbound.
  keys: Record<string, string>;
  // Which controller button stands for which pad button: SDL gamepad names joined with '+', '' when unbound; `stick` is
  // 'left', 'right' or ''.
  pad: Record<string, string>;
  // The PSP ad hoc server used for online play, "host" or "host:port"; '' keeps the game offline.
  adhocServer: string;
  // The private room's code ("K7QF29XM"), '' for the public halls (docs/14-rooms-and-friends.md section 3).
  adhocRoom: string;
  checkUpdates: boolean; // ask GitHub for a newer release when the launcher starts
  friendRelays: string[]; // the public Nostr relays friends meet through (wss:// addresses)
}

// A line of the game's Online tab: a friend and where they are, or an invite from one.
export interface GameFriend {
  kind: 'friend' | 'invite';
  key: string;
  name: string;
  state: 'offline' | 'launcher' | 'playing';
  server: string;
  room: string;
}

// A friend, as the launcher keeps them (docs/14-rooms-and-friends.md section 4.3).
export interface FriendEntry {
  key: string; // their public key (hex)
  name: string;
  added: number; // unix seconds
}

export interface Stats {
  secondsPlayed: number;
  sessions: number;
  lastPlayed: number | null; // unix seconds
}

export interface State {
  version: string;
  platform: string;
  pseudonym: string;
  nostrKey: string; // the secret key of the player's friends identity (hex), derived from the identity seed
  friends: FriendEntry[];
  blocked: string[]; // keys whose friend requests are ignored
  settings: Settings;
  keyDefaults: Record<string, string>; // what "Restore defaults" puts back (Settings → Controls)
  padDefaults: Record<string, string>;
  games: Game[];
  selectedDisc: string; // the copy Home, Settings and Play work on (chosen in the Library)
  stats: Stats;
  executable: { path: string; found: boolean };
  savesFolder: string; // where the saves are kept (Settings > Storage)
  savesFolderCustom: boolean; // the player chose it; false: the default one
  texturePack: string; // the texture pack folder, '' when none (Settings > Storage)
  texturePackEnabled: boolean; // the game is started with it
}

export interface SaveScan {
  picked: boolean; // false when the player cancelled the folder chooser (or there is no game yet: see `error`)
  error?: string;
  note?: string; // why nothing was found
  found?: { folder: string; title: string; exists: boolean }[];
}

export interface SaveImportResult {
  imported: number;
  backup: string; // where the replaced saves were kept ('' when none was)
  error: string;
}

export type LauncherEvent =
  | { type: 'verify'; phase: 'reading' | 'hashing' | 'preparing' | 'copying'; progress: number; file: string }
  | { type: 'verified'; game: Game }
  | { type: 'verifyFailed'; message: string }
  | { type: 'launch'; phase: 'checking' | 'starting' | 'running' }
  | { type: 'exited'; code: number; seconds: number }
  | { type: 'launchFailed'; message: string }
  | { type: 'state'; state: State }
  | { type: 'gameFriend'; action: 'invite' | 'dismiss'; key: string } // the player's choice in the game's Online tab
  | ({ type: 'update' } & UpdateInfo)
  | { type: 'updateProgress'; phase: 'downloading' | 'verifying' | 'installing'; progress: number }
  | { type: 'updateReady'; version: string }
  | { type: 'updateFailed'; message: string };

// A newer release of MHForever on GitHub.
export interface UpdateInfo {
  version: string;
  page: string;
  size: number; // bytes of the archive for this system
}

type Native = {
  mhf_getState(): Promise<State>;
  mhf_setSettings(settings: Partial<Settings>): Promise<State>;
  mhf_setPseudonym(name: string): Promise<State>;
  mhf_addGame(): Promise<{ started: boolean }>;
  mhf_removeGame(id: string): Promise<State>;
  mhf_pickExecutable(): Promise<State>;
  mhf_selectGame(discId: string): Promise<State>;
  mhf_play(id: string): Promise<{ started: boolean }>;
  mhf_copyText(text: string): Promise<boolean>;
  mhf_exportIdentity(): Promise<{ saved: boolean }>;
  mhf_setFriends(friends: FriendEntry[], blocked: string[]): Promise<State>;
  // Who is where, and the invites, for the game's Online tab while it runs.
  mhf_setGameFriends(entries: GameFriend[]): Promise<boolean>;
  mhf_scanSaves(gameId: string): Promise<SaveScan>; // asks for a folder, looks for the saves of that game in it
  mhf_importSaves(): Promise<SaveImportResult>; // imports what the last scan found
  mhf_probeServer(server: string): Promise<number | null>; // milliseconds to connect to an ad hoc server, null when it cannot
  // Chooses a folder for the saves (copying them there), or goes back to the default one with 'default'.
  mhf_updateInfo(): Promise<UpdateInfo | null>; // the newer release the start-up check found, if any
  mhf_installUpdate(): Promise<{ started: boolean }>; // download, check, install and restart (events: updateProgress...)
  mhf_savesFolder(mode: '' | 'default'): Promise<{ picked: boolean; copied?: number; error?: string; state: State }>;
  // A texture pack folder for the game (developer feature): '' asks for a folder, 'none' removes it, 'on'/'off' switch it.
  mhf_texturePack(mode: '' | 'on' | 'off' | 'none'): Promise<{ picked: boolean; error?: string; state: State }>;
};

const listeners = new Set<(e: LauncherEvent) => void>();
(window as unknown as { __mhfEvent: (e: LauncherEvent) => void }).__mhfEvent = (e) => listeners.forEach((l) => l(e));

export function onEvent(listener: (e: LauncherEvent) => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

const w = window as unknown as Partial<Native>;
export const isNative = typeof w.mhf_getState === 'function';

// ------------------------------------------------------------------ simulated backend (browser only)

function simulated(): Native {
  const emit = (e: LauncherEvent) => setTimeout(() => (window as unknown as { __mhfEvent: (e: LauncherEvent) => void }).__mhfEvent(e), 0);
  const defaultKeys: Record<string, string> = {
    south: 'KeyK', east: 'KeyL', west: 'KeyJ', north: 'KeyI',
    left_shoulder: 'KeyQ', right_shoulder: 'KeyE', start: 'Enter', select: 'Backspace',
    up: 'ArrowUp', down: 'ArrowDown', left: 'ArrowLeft', right: 'ArrowRight',
    stick_up: 'KeyW', stick_down: 'KeyS', stick_left: 'KeyA', stick_right: 'KeyD',
  };
  const defaultPad: Record<string, string> = {
    south: 'south', east: 'east', west: 'west', north: 'north',
    left_shoulder: 'left_shoulder+left_trigger', right_shoulder: 'right_shoulder+right_trigger', start: 'start', select: 'back',
    up: 'dpad_up', down: 'dpad_down', left: 'dpad_left', right: 'dpad_right', stick: 'left',
  };
  const state: State = {
    version: '0.8.0-dev',
    platform: 'browser',
    pseudonym: 'Hunter',
    nostrKey: Array.from(crypto.getRandomValues(new Uint8Array(32)), (b) => b.toString(16).padStart(2, '0')).join(''),
    friends: [],
    blocked: [],
    settings: {
      scale: 4, fullscreen: false, smoothTextures: false, fps: 30, copyImage: false, musicVolume: 80, sfxVolume: 90,
      keys: { ...defaultKeys }, pad: { ...defaultPad }, adhocServer: '', adhocRoom: '', checkUpdates: true,
      friendRelays: ['wss://nos.lol', 'wss://relay.primal.net', 'wss://nostr.mom', 'wss://offchain.pub', 'wss://relay.nostr.net'],
    },
    keyDefaults: defaultKeys,
    padDefaults: defaultPad,
    games: [],
    selectedDisc: '',
    stats: { secondsPlayed: 0, sessions: 0, lastPlayed: null },
    executable: { path: '', found: true },
    savesFolder: 'C:\\Users\\Hunter\\Documents\\MHForever\\saves',
    savesFolderCustom: false,
    texturePack: '', texturePackEnabled: false,
  };
  const copy = () => JSON.parse(JSON.stringify(state)) as State;
  return {
    async mhf_getState() { return copy(); },
    async mhf_setSettings(s) { Object.assign(state.settings, s); return copy(); },
    async mhf_setPseudonym(n) { state.pseudonym = n; return copy(); },
    async mhf_addGame() {
      let p = 0;
      const file = 'ULES01213.iso';
      emit({ type: 'verify', phase: 'reading', progress: 0, file });
      const tick = setInterval(() => {
        p += 0.07;
        if (p >= 1) {
          clearInterval(tick);
          state.games = [{ id: 'mhfu', title: 'Freedom Unite', region: 'Europe', discId: 'ULES01213', version: '1.01', isoPath: 'C:\\Games\\' + file, verification: 'verified', supported: true }];
          emit({ type: 'verified', game: state.games[0] });
          emit({ type: 'state', state: copy() });
        } else {
          emit({ type: 'verify', phase: 'hashing', progress: p, file });
        }
      }, 120);
      return { started: true };
    },
    async mhf_removeGame(id) { state.games = state.games.filter((g) => g.id !== id && g.discId !== id); return copy(); },
    async mhf_selectGame(discId) { state.selectedDisc = discId; return copy(); },
    async mhf_pickExecutable() { return copy(); },
    async mhf_play() {
      emit({ type: 'launch', phase: 'checking' });
      setTimeout(() => emit({ type: 'launch', phase: 'starting' }), 700);
      setTimeout(() => emit({ type: 'launch', phase: 'running' }), 1600);
      setTimeout(() => {
        state.stats.sessions += 1;
        state.stats.secondsPlayed += 3725;
        state.stats.lastPlayed = Math.floor(Date.now() / 1000);
        emit({ type: 'exited', code: 0, seconds: 3725 });
        emit({ type: 'state', state: copy() });
      }, 6000);
      return { started: true };
    },
    async mhf_copyText(text) { try { await navigator.clipboard.writeText(text); } catch { /* not allowed here */ } return true; },
    async mhf_exportIdentity() { return { saved: true }; },
    async mhf_setFriends(friends, blocked) { state.friends = friends; state.blocked = blocked; return copy(); },
    async mhf_setGameFriends() { return true; },
    async mhf_scanSaves() { return { picked: true, found: [{ folder: 'ULES01213', title: 'Game Data', exists: true }], note: '' }; },
    async mhf_importSaves() { return { imported: 1, backup: 'C:\\Users\\Hunter\\Documents\\MHForever\\saves\\ULES01213\\backups\\20261022-170000', error: '' }; },
    async mhf_probeServer(server) { await new Promise((r) => setTimeout(r, 300 + Math.random() * 700)); return server.includes('nowhere') ? null : 20 + Math.round(Math.random() * 250); },
    async mhf_updateInfo() { return { version: '0.7.0', page: 'https://github.com/Mango77x/MHForever/releases', size: 73_000_000 }; },
    async mhf_installUpdate() {
      let p = 0;
      const tick = setInterval(() => {
        p += 0.08;
        if (p < 1) emit({ type: 'updateProgress', phase: 'downloading', progress: p });
        else {
          clearInterval(tick);
          emit({ type: 'updateProgress', phase: 'verifying', progress: 1 });
          setTimeout(() => emit({ type: 'updateProgress', phase: 'installing', progress: 1 }), 500);
          setTimeout(() => emit({ type: 'updateReady', version: '0.7.0' }), 1200);
        }
      }, 120);
      return { started: true };
    },
    async mhf_savesFolder(mode) {
      state.savesFolderCustom = mode !== 'default';
      state.savesFolder = mode === 'default' ? 'C:\\Users\\Hunter\\Documents\\MHForever\\saves' : 'D:\\Games\\MHForever saves';
      return { picked: true, copied: mode === 'default' ? 0 : 1, error: '', state: copy() };
    },
    async mhf_texturePack(mode) {
      if (mode === 'on' || mode === 'off') state.texturePackEnabled = mode === 'on';
      else if (mode === 'none') { state.texturePack = ''; state.texturePackEnabled = false; }
      else { state.texturePack = 'D:\\Mods\\MHFU textures'; state.texturePackEnabled = true; }
      return { picked: mode === '', error: '', state: copy() };
    },
  };
}

export const api: Native = isNative ? (w as Native) : simulated();

/** The copy the player chose in the Library, else the first one that can be played. */
export function selectedGame(state: State): Game | undefined {
  return state.games.find((g) => g.supported && g.discId === state.selectedDisc) ?? state.games.find((g) => g.supported);
}

/** The short region tag shown next to a game's name: (EU), (NA), (JP). */
export function regionTag(game: Game): string {
  switch (game.region) {
    case 'Europe': return 'EU';
    case 'North America': return 'NA';
    case 'Japan': return 'JP';
    default: return game.region;
  }
}
