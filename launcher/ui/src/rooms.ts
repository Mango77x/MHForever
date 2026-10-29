// Room codes for private rooms (docs/14-rooms-and-friends.md section 3): 8 characters of Crockford base32. The same rule
// as the game's (platforms/psp/runtime/src/adhocctl.cpp) and the launcher's (launcher/native/launcher.cpp, room_code).

const alphabet = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';

// "k7qf-29xm" -> "K7QF29XM" (O reads as 0, I and L as 1); '' when the text is not a room code.
export function roomCode(text: string): string {
  let code = '';
  for (const raw of text) {
    if (raw === '-' || raw === ' ') continue;
    let c = raw.toUpperCase();
    if (c === 'O') c = '0';
    if (c === 'I' || c === 'L') c = '1';
    if (!alphabet.includes(c) || c.length !== 1) return '';
    code += c;
  }
  return code.length === 8 ? code : '';
}

// "K7QF29XM" -> "K7QF-29XM", the way players read and type it.
export const formatRoomCode = (code: string) => (code.length === 8 ? `${code.slice(0, 4)}-${code.slice(4)}` : code);

export function newRoomCode(): string {
  const bytes = new Uint8Array(8);
  crypto.getRandomValues(bytes);
  return Array.from(bytes, (b) => alphabet[b & 31]).join('');
}
