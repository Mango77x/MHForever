// Settings → Online: which PSP ad hoc server the game plays through. Offline, one of the community servers in
// servers.ts, or any other one typed in. Each choice shows how long it takes to connect to that server from here.

import { useEffect, useRef, useState } from 'react';
import { api } from './bridge';
import { adhocServers, serverPorts } from './servers';
import { checkServer, suggestedServer, type ServerCheck } from './serverCheck';

function PingBadge({ ping }: { ping: number | null }) {
  if (ping === null) return <span className="ping none">No answer</span>;
  return <span className={'ping ' + (ping < 120 ? 'good' : ping < 250 ? 'fair' : 'slow')}>{ping} ms</span>;
}

function Choice({ on, title, detail, result, onPick }: { on: boolean; title: string; detail: string; result?: ServerCheck; onPick: () => void }) {
  return (
    <button className={'server' + (on ? ' on' : '')} role="radio" aria-checked={on} onClick={onPick}>
      <span className="server-dot" />
      <span className="server-text"><b>{title}</b><span>{detail}</span></span>
      {result && <span className="server-times">
        <span>Hall <PingBadge ping={result.hall} /></span>
        <span>Relay <PingBadge ping={result.relay} /></span>
      </span>}
    </button>
  );
}

export function OnlineSettings({ server, onChange }: { server: string; onChange: (server: string) => void }) {
  const preset = adhocServers.some((s) => s.host === server);
  const [customOpen, setCustomOpen] = useState(server !== '' && !preset);
  const [custom, setCustom] = useState(preset ? '' : server);
  const [results, setResults] = useState<Record<string, ServerCheck>>({});
  const comparison = useRef(0);
  useEffect(() => () => { comparison.current++; }, []);
  useEffect(() => {
    if (server !== '' && !preset) {
      setCustom(server);
      setCustomOpen(true);
    }
  }, [server, preset]);

  const [measuring, setMeasuring] = useState(false);
  const measureAll = async () => {
    const run = ++comparison.current;
    setMeasuring(true);
    setResults({});
    // At most two servers (four sockets) at once. Leaving this page skips all remaining batches.
    for (let i = 0; i < adhocServers.length; i += 2) {
      if (comparison.current !== run) return;
      await Promise.all(adhocServers.slice(i, i + 2).map(async ({ host }) => {
        const result = await checkServer(host, api.mhf_probeServer);
        if (comparison.current === run) setResults((previous) => ({ ...previous, [host]: result }));
      }));
    }
    if (comparison.current === run) setMeasuring(false);
  };
  const count = Object.keys(results).length;
  const suggested = !measuring && count === adhocServers.length ? suggestedServer(adhocServers, results) : undefined;

  const pick = (host: string) => {
    setCustomOpen(false);
    if (host !== server) onChange(host);
  };
  const commitCustom = () => {
    const text = custom.trim();
    if (!serverPorts(text)) return;
    if (adhocServers.some(({ host }) => host === text)) setCustomOpen(false);
    if (text !== server) onChange(text);
  };
  const checkTarget = customOpen ? custom.trim() : server;
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
      <div role="status">
        {measuring && `Checked ${count} of ${adhocServers.length} servers.`}
        {!measuring && count > 0 && (suggested
          ? `Suggested server: ${suggested.name}. Both the hall and quest relay answered.`
          : 'No community server answered on both ports. Check the results below or test a custom server.')}
      </div>
      {suggested && server !== suggested.host && (
        <button className="ghost mid" onClick={() => pick(suggested.host)}>Use {suggested.name}</button>
      )}
      <div className="servers" role="radiogroup" aria-label="Ad hoc server">
        <Choice on={server === ''} title="Offline" detail="Disable in-game ad hoc play." onPick={() => pick('')} />
        {adhocServers.map((s) => (
          <Choice key={s.host} on={server === s.host} title={s.name} detail={`${s.location} · ${s.host} · ${s.note}`}
            result={results[s.host]} onPick={() => pick(s.host)} />
        ))}
        <Choice on={!!server && !preset} title="LAN, VPN or custom server" detail="Enter the address of an existing aemu server reachable on your network."
          onPick={() => setCustomOpen(true)} />
        {customOpen && (
          <div className="server-custom">
            <span className="muted">Active setting · {server || 'Offline'}</span>
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
      {serverPorts(checkTarget) && <ConnectionCheck key={checkTarget} server={checkTarget} draft={checkTarget !== server} />}
    </>
  );
}

function ConnectionCheck({ server, draft }: { server: string; draft: boolean }) {
  const ports = serverPorts(server);
  const [checking, setChecking] = useState(false);
  const [result, setResult] = useState<ServerCheck>();
  if (!ports) return <p className="server-error">Choose a valid server with room for its relay port.</p>;
  const check = async () => {
    setChecking(true);
    setResult(undefined);
    try {
      setResult(await checkServer(server, api.mhf_probeServer));
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
            <div>Hall · TCP {ports.control} <PingBadge ping={result.hall} /></div>
            <div>Quest relay · TCP {ports.relay} <PingBadge ping={result.relay} /></div>
            <span>{result.hall === null ? 'Check the server address and control port.'
              : result.relay === null ? 'The hall answered. Check that the server runs a relay on the next port and your network allows it.'
              : 'Both ports answered. Enter the same Gathering Hall in the game to check play.'}</span>
          </>}
        </div>
      </div>
      <button className="ghost mid" disabled={checking} onClick={check}>{draft ? 'Test before saving' : 'Test this server'}</button>
    </div>
  );
}
