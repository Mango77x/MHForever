// Stroke icons drawn for the launcher (no third-party icon set).

type P = { size?: number; color?: string; width?: number };

const base = (size: number, color: string, width: number) => ({
  width: size,
  height: size,
  viewBox: '0 0 24 24',
  fill: 'none',
  stroke: color,
  strokeWidth: width,
  strokeLinecap: 'round' as const,
  strokeLinejoin: 'round' as const,
  'aria-hidden': true,
});

export const HomeIcon = ({ size = 22, color = 'currentColor', width = 2 }: P) => (
  <svg {...base(size, color, width)}><path d="M3 11 12 4l9 7" /><path d="M5 10v10h14V10" /></svg>
);
export const LibraryIcon = ({ size = 22, color = 'currentColor', width = 2 }: P) => (
  <svg {...base(size, color, width)}><rect x="3" y="3" width="7" height="7" rx="2" /><rect x="14" y="3" width="7" height="7" rx="2" /><rect x="3" y="14" width="7" height="7" rx="2" /><rect x="14" y="14" width="7" height="7" rx="2" /></svg>
);
export const FriendsIcon = ({ size = 22, color = 'currentColor', width = 2 }: P) => (
  <svg {...base(size, color, width)}><circle cx="9" cy="8" r="3.5" /><path d="M2.5 20c.8-3.6 3.4-5.5 6.5-5.5s5.7 1.9 6.5 5.5" /><path d="M16 4.5a3.5 3.5 0 0 1 0 7" /><path d="M18 14.8c1.9.7 3.1 2.4 3.5 5.2" /></svg>
);
export const SlidersIcon = ({ size = 22, color = 'currentColor', width = 2 }: P) => (
  <svg {...base(size, color, width)}><path d="M4 7h10M18 7h2M4 17h4M12 17h8" /><circle cx="16" cy="7" r="2" /><circle cx="10" cy="17" r="2" /></svg>
);
export const PlayIcon = ({ size = 22, color = 'currentColor' }: P) => (
  <svg width={size} height={size} viewBox="0 0 24 24" fill={color} aria-hidden="true"><path d="M7 4.5v15a1 1 0 0 0 1.5.86l12.5-7.5a1 1 0 0 0 0-1.72L8.5 3.64A1 1 0 0 0 7 4.5Z" /></svg>
);
export const CheckIcon = ({ size = 14, color = 'currentColor', width = 3 }: P) => (
  <svg {...base(size, color, width)}><path d="M5 12l5 5L20 7" /></svg>
);
export const CopyIcon = ({ size = 16, color = 'currentColor', width = 2 }: P) => (
  <svg {...base(size, color, width)}><rect x="9" y="9" width="11" height="11" rx="2.5" /><path d="M5 15V6a2 2 0 0 1 2-2h9" /></svg>
);
export const ChevronIcon = ({ size = 16, color = 'currentColor', width = 2.4 }: P) => (
  <svg {...base(size, color, width)}><path d="M6 9l6 6 6-6" /></svg>
);
export const PlusIcon = ({ size = 26, color = 'currentColor', width = 2.4 }: P) => (
  <svg {...base(size, color, width)}><path d="M12 5v14M5 12h14" /></svg>
);
export const CloseIcon = ({ size = 20, color = 'currentColor', width = 2.2 }: P) => (
  <svg {...base(size, color, width)}><path d="M6 6l12 12M18 6 6 18" /></svg>
);
export const AlertIcon = ({ size = 18, color = 'currentColor', width = 2.2 }: P) => (
  <svg {...base(size, color, width)}><path d="M12 3 2 20h20L12 3Z" /><path d="M12 10v4M12 17.5v.01" /></svg>
);
export const FolderIcon = ({ size = 18, color = 'currentColor', width = 2 }: P) => (
  <svg {...base(size, color, width)}><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7Z" /></svg>
);
