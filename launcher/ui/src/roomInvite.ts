// Clipboard invites are parsed locally. The optional edition identifies the halls and quest set to use.
import type { Game } from './bridge';
import { roomCode, formatRoomCode } from './rooms.ts';
import { serverPorts } from './servers.ts';

const editions = {
  Freedom: { name: 'Freedom', halls: 'freedom' },
  'Freedom-DX': { name: 'Freedom DX', halls: 'freedom' },
  'Freedom-Unite': { name: 'Freedom Unite (EU / NA)', halls: 'unite' },
  'Portable-2nd-G': { name: 'Portable 2nd G', halls: 'second-g' },
  FUComplete: { name: 'FUComplete', halls: 'second-g' },
} as const;

export type Edition = keyof typeof editions;

export function copyEdition(game?: Pick<Game, 'id' | 'discId'>): Edition | undefined {
  if (game?.id === 'mhf1') return game.discId.includes('-FDX') ? 'Freedom-DX' : 'Freedom';
  if (game?.id !== 'mhfu') return undefined;
  if (game.discId === 'ULJM05500-FUC') return 'FUComplete';
  return game.discId === 'ULJM05500' ? 'Portable-2nd-G' : 'Freedom-Unite';
}

export function editionName(edition: Edition) { return editions[edition].name; }

export function sharesHalls(host: Edition, local?: Edition) {
  return local !== undefined && editions[host].halls === editions[local].halls;
}

export function formatInvite(server: string, room: string, edition?: Edition) {
  const place = `MHForever ${server} ${formatRoomCode(room)}`;
  return edition ? `${place} ${edition}` : place;
}

interface RoomInvite { server: string; room: string; edition?: Edition }

export function readInvite(text: string, currentServer: string): RoomInvite | null {
  const code = roomCode(text.trim());
  if (code && serverPorts(currentServer)) return { server: currentServer, room: code };
  const parts = text.trim().split(/\s+/);
  if ((parts.length !== 3 && parts.length !== 4) || parts[0] !== 'MHForever' || !serverPorts(parts[1])) return null;
  const room = roomCode(parts[2]);
  if (!room) return null;
  if (parts.length === 3) return { server: parts[1], room };
  if (!Object.hasOwn(editions, parts[3])) return null;
  return { server: parts[1], room, edition: parts[3] as Edition };
}
