// Plain text for sharing outside the friends network. Parsing never opens a connection.
import { roomCode, formatRoomCode } from './rooms';
import { serverPorts } from './servers';

export function formatInvite(server: string, room: string) {
  return `MHForever ${server} ${formatRoomCode(room)}`;
}

export function readInvite(text: string, currentServer: string) {
  const code = roomCode(text.trim());
  if (code && serverPorts(currentServer)) return { server: currentServer, room: code };
  const parts = text.trim().split(/\s+/);
  if (parts.length !== 3 || parts[0] !== 'MHForever' || !serverPorts(parts[1])) return null;
  const room = roomCode(parts[2]);
  return room ? { server: parts[1], room } : null;
}
