// The PSP ad hoc servers Settings → Online offers. They are community servers from PPSSPP's published list
// (https://metadata.ppsspp.org/adhoc-servers.json, checked 2026-10-27), all speaking the aemu postoffice protocol the
// game uses, so players there meet PPSSPP players too. Any other server can still be typed in.

export interface AdhocServer {
  host: string; // what the game is given: "host" or "host:port"
  name: string;
  location: string;
  note: string;
}

export const adhocServers: AdhocServer[] = [
  { host: 'socom.cc', name: 'Socom', location: 'France', note: 'The busiest server' },
  { host: 'psp.mgn.pub', name: 'Madness Gaming Network', location: 'United States', note: 'Has a Monster Hunter community' },
  { host: 'adhoc.gamesnexus.ovh', name: 'Games Nexus', location: 'Italy', note: 'For any game' },
  { host: 'psi-hate.com', name: 'Psi-Hate', location: 'United States', note: 'For any game' },
  { host: 'cat.wjg.jp', name: 'Neko PSP', location: 'Japan', note: 'For any game' },
];

// The runtime connects to the control port and opens the relay on the following port.
export function serverPorts(address: string) {
  const match = /^([A-Za-z0-9._-]+)(?::([0-9]{1,5}))?$/.exec(address);
  if (!match || match[0] !== address || address.length > 253) return null;
  const control = Number(match[2] ?? 27312);
  if (control < 1 || control >= 65535) return null;
  return { host: match[1], control, relay: control + 1 };
}
