import { serverPorts } from './servers.ts';

export interface ServerCheck {
  hall: number | null;
  relay: number | null;
}

// Each probe times one outbound TCP connection. A failure on one port keeps the other port's result.
export async function checkServer(address: string, probe: (address: string) => Promise<number | null>): Promise<ServerCheck> {
  const ports = serverPorts(address);
  if (!ports) return { hall: null, relay: null };
  const measure = async (target: string) => {
    try {
      return await probe(target);
    } catch {
      return null;
    }
  };
  const [hall, relay] = await Promise.all([measure(address), measure(`${ports.host}:${ports.relay}`)]);
  return { hall, relay };
}

// Consider only complete pairs. Visit servers in display order so an equal score has a stable winner.
export function suggestedServer<T extends { host: string }>(servers: readonly T[], results: Record<string, ServerCheck>) {
  let best: T | undefined;
  let bestTime = Infinity;
  for (const server of servers) {
    const result = results[server.host];
    if (!result || result.hall === null || result.relay === null) continue;
    const slowerPort = Math.max(result.hall, result.relay);
    if (slowerPort < bestTime) {
      best = server;
      bestTime = slowerPort;
    }
  }
  return best;
}
