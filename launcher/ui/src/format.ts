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
  const diff = Date.now() / 1000 - unix;
  if (diff < 90) return 'Just now';
  if (diff < 3600) return `${Math.round(diff / 60)} minutes ago`;
  if (diff < 86400) return `${Math.round(diff / 3600)} hours ago`;
  if (diff < 86400 * 2) return 'Yesterday';
  if (diff < 86400 * 14) return `${Math.round(diff / 86400)} days ago`;
  return new Date(unix * 1000).toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' });
}

export const frameRates = [30, 60, 90, 120, 150, 180];
export const resolutions = ['480 × 272', '960 × 544', '1440 × 816', '1920 × 1088'];
