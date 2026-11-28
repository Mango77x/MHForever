// Settings → Online: which PSP ad hoc server the game plays through. Offline, one of the community servers in
// servers.ts, or any other one typed in. Each choice shows how long it takes to connect to that server from here.

import { useEffect, useState } from 'react';
import { api } from './bridge';
import { adhocServers, serverPorts } from './servers';

type Ping = number | null | undefined; // milliseconds, null when the server did not answer, undefined while measuring

function PingBadge({ ping }: { ping: Ping }) {
  if (ping === undefined) return <span className="ping">…</span>;
  if (ping === null) return <span className="ping none">No answer</span>;
  return <span className={'ping ' + (ping < 120 ? 'good' : ping < 250 ? 'fair' : 'slow')}>{ping} ms</span>;
}

function Choice({ on, title, detail, ping, onPick }: { on: boolean; title: string; detail: string; ping: Ping | false; onPick: () => void }) {
  return (
    <button className={'server' + (on ? ' on' : '')} role="radio" aria-checked={on} onClick={onPick}>
      <span className="server-dot" />
      <span className="server-text"><b>{title}</b><span>{detail}</span></span>
      {ping === false ? null : <PingBadge ping={ping} />}
    </button>
  );
}

export function OnlineSettings({ server, onChange }: { server: string; onChange: (server: string) => void }) {
  const preset = adhocServers.some((s) => s.host === server);
  const [customOpen, setCustomOpen] = useState(server !== '' && !preset);
  const [custom, setCustom] = useState(preset ? '' : server);
  const [pings, setPings] = useState<Record<string, Ping>>({});
  useEffect(() => {
    if (server !== '' && !preset) {
      setCustom(server);
      setCustomOpen(true);
    }
  }, [server, preset]);

  const [measuring, setMeasuring] = useState(false);
  const measureAll = async () => {
    setMeasuring(true);
    setPings(Object.fromEntries(adhocServers.map(({ host }) => [host, undefined])));
    try {
      await Promise.all(adhocServers.map(async ({ host }) => {
        const ms = await api.mhf_probeServer(host).catch(() => null);
        setPings((previous) => ({ ...previous, [host]: ms }));
      }));
    } finally {
      setMeasuring(false);
    }
  };

  const pick = (host: string) => {
    setCustomOpen(false);
    if (host !== server) onChange(host);
  };
  const commitCustom = () => {
    const text = custom.trim();
    if (serverPorts(text) && text !== server) onChange(text);
  };
  const customError = custom.trim() !== '' && !serverPorts(custom.trim());

  return (
    <>
      <div className="srow">
        <div className="slabel">
          <b>Server</b>
          <span className="prose">
            The ad hoc server used for online play. Players on the same server, including PPSSPP users, meet in the Gathering
            Hall. The server can see your name and game and may list them on its status page. By default, your IP address is
            not shown to other players.
          </span>
        </div>
        <button className="ghost mid" disabled={measuring} onClick={measureAll}>
          {measuring ? 'Checking…' : 'Compare community servers'}
        </button>
      </div>
      <div className="servers" role="radiogroup" aria-label="Ad hoc server">
        <Choice on={server === '' && !customOpen} title="Offline" detail="Disable in-game ad hoc play." ping={false} onPick={() => pick('')} />
        {adhocServers.map((s) => (
          <Choice key={s.host} on={server === s.host && !customOpen} title={s.name} detail={`${s.location} · ${s.host} · ${s.note}`}
            ping={s.host in pings ? pings[s.host] : false} onPick={() => pick(s.host)} />
        ))}
        <Choice on={customOpen} title="LAN, VPN or custom server" detail="Enter the address of an existing aemu server reachable on your network."
          ping={false} onPick={() => setCustomOpen(true)} />
        {customOpen && (
          <div className="server-custom">
            <input
              id="adhoc-server"
              className="text-input"
              value={custom}
              placeholder="host or host:port"
              aria-label="Server address"
              aria-invalid={customError}
              spellCheck={false}
              autoFocus
              onChange={(e) => setCustom(e.target.value.trim())}
              maxLength={253}
              onKeyDown={(e) => { if (e.key === 'Enter') commitCustom(); }}
            />
            <button className="ghost mid" disabled={!serverPorts(custom.trim()) || custom.trim() === server} onClick={commitCustom}>Use this server</button>
            {customError && <span className="server-error">Enter a hostname or IPv4 address. An optional control port must be between 1 and 65534.</span>}
          </div>
        )}
      </div>
      {server && <ConnectionCheck key={server} server={server} />}
    </>
  );
}

function ConnectionCheck({ server }: { server: string }) {
  const ports = serverPorts(server);
  const [checking, setChecking] = useState(false);
  const [result, setResult] = useState<[Ping, Ping]>();
  if (!ports) return <p className="server-error">Choose a valid server with room for its relay port.</p>;
  const check = async () => {
    setChecking(true);
    setResult(undefined);
    try {
      const relay = ports.host + ':' + ports.relay;
      const probe = (address: string) => api.mhf_probeServer(address).catch(() => null);
      setResult(await Promise.all([probe(server), probe(relay)]));
    } finally {
      setChecking(false);
    }
  };
  return (
    <div className="srow">
      <div className="slabel">
        <b>Connection check · {server}</b>
        <span>Opens two TCP connections. No nickname or room code is sent.</span>
        <div role="status">
          {checking && 'Checking hall and relay…'}
          {result && <>
            <div>Hall · TCP {ports.control} <PingBadge ping={result[0]} /></div>
            <div>Quest relay · TCP {ports.relay} <PingBadge ping={result[1]} /></div>
            <span>{result[0] === null ? 'Check the server address and control port.'
              : result[1] === null ? 'The hall answered. Check that the server runs a relay on the next port and your network allows it.'
              : 'Both ports answered. Enter the same Gathering Hall in the game to check play.'}</span>
          </>}
        </div>
      </div>
      <button className="ghost mid" disabled={checking} onClick={check}>Test this server</button>
    </div>
  );
}
