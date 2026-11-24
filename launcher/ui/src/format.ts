export function hours(seconds: number): string {
  if (seconds < 60) return '0h';
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return h > 0 ? `${h}h ${m.toString().padStart(2, '0')}m` : `${m}m`;
}

export function clock(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  const mm = m.toString().padStart(2, '0'), ss = s.toString().padStart(2, '0');
  return h > 0 ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
}

export function lastPlayed(unix: number | null): string {
  if (unix === null) return 'Never';
  const now = new Date();
  const played = new Date(unix * 1000);
  // Compare local calendar dates without counting the hours in each day.
  const day = (date: Date) => Date.UTC(date.getFullYear(), date.getMonth(), date.getDate()) / 86_400_000;
  const daysAgo = day(now) - day(played);
  if (daysAgo === 1) return 'Yesterday';
  if (daysAgo >= 2 && daysAgo < 14) return `${daysAgo} days ago`;
  if (daysAgo >= 14) return played.toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' });

  const minutes = Math.floor((now.getTime() - played.getTime()) / 60_000);
  if (minutes < 1) return 'Just now';
  if (minutes < 60) return minutes === 1 ? '1 minute ago' : `${minutes} minutes ago`;
  const hours = Math.floor(minutes / 60);
  return hours === 1 ? '1 hour ago' : `${hours} hours ago`;
}

export const frameRates = [30, 60, 90, 120, 150, 180];
// HUD size in percent, as the in-game menu offers it (platforms/psp/frontend/psp_frontend.cpp, kHudSizes).
export const hudSizes = [100, 90, 80, 70, 60, 50];
// Anisotropic filtering of texture packs, 1 being off (the launcher's anisotropy setting, the game's --anisotropy).
export const anisotropyLevels = [1, 2, 4, 8, 16];
// Where the hunting horn's songs can go (the game's names: core/overlay/song_panel.hpp, kSongPlaceNames).
export const songPlaces = [
  { id: 'next-to-sheet', label: 'Next to the sheet' },
  { id: 'top-middle', label: 'Top middle' },
  { id: 'under-names', label: 'Under the names' },
  { id: 'bottom-left', label: 'Bottom left' },
] as const;
// Internal resolutions: whole multiples of the PSP's 480 × 272 (1× to 10×), so 2D art and text stay sharp.
export const resolutions = Array.from({ length: 10 }, (_, i) => `${480 * (i + 1)} × ${272 * (i + 1)}`);
