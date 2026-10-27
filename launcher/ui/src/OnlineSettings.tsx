// Settings → Online: which PSP ad hoc server the game plays through. Offline, one of the community servers in
// servers.ts, or any other one typed in. Each choice shows how long it takes to connect to that server from here.

import { useCallback, useEffect, useState } from 'react';
import { api } from './bridge';
import { adhocServers } from './servers';

type Ping = number | null | undefined; // milliseconds, null when the server did not answer, undefined while measuring

// "host" or "host:port", the same rule as the native side (launcher.cpp, valid_server).
const validServer = (text: string) => /^[A-Za-z0-9._-]+(:([1-9][0-9]{0,4}))?$/.test(text) && Number(text.split(':')[1] ?? 1) <= 65535;

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

  const measure = useCallback((hosts: string[]) => {
    setPings((p) => ({ ...p, ...Object.fromEntries(hosts.map((h) => [h, undefined])) }));
    for (const host of hosts) api.mhf_probeServer(host).then((ms) => setPings((p) => ({ ...p, [host]: ms })), () => setPings((p) => ({ ...p, [host]: null })));
  }, []);
  // Measuring contacts every server in the list, so it waits until the player is online or asks for it.
  const [measured, setMeasured] = useState(false);
  const measureAll = useCallback(() => {
    setMeasured(true);
    measure(adhocServers.map((s) => s.host));
  }, [measure]);
  useEffect(() => {
    if (server !== '' && !measured) measureAll();
  }, [server, measured, measureAll]);
  useEffect(() => {
    if (server !== '' && !preset) measure([server]);
  }, [server, preset, measure]);

  const pick = (host: string) => {
    setCustomOpen(false);
    if (host !== server) onChange(host);
  };
  const commitCustom = () => {
    const text = custom.trim();
    if (text === '' || !validServer(text)) setCustom(preset ? '' : server);
    else if (text !== server) onChange(text);
  };
  const customError = custom.trim() !== '' && !validServer(custom.trim());

  return (
    <>
      <div className="srow">
        <div className="slabel">
          <b>Server</b>
          <span className="prose">
            The PSP ad hoc server the game plays through, the same kind PPSSPP uses: players on the same server meet in the
            game's own Gathering Hall, PPSSPP players included. The server sees your name and your game and may show them on
            its public status page. In their default mode, servers do not show your IP address to other players.
          </span>
        </div>
        <button className="ghost mid" onClick={() => { measureAll(); if (server !== '' && !preset) measure([server]); }}>
          {measured ? 'Measure again' : 'Measure ping'}
        </button>
      </div>
      <div className="servers" role="radiogroup" aria-label="Ad hoc server">
        <Choice on={server === '' && !customOpen} title="Offline" detail="Nothing is contacted." ping={false} onPick={() => pick('')} />
        {adhocServers.map((s) => (
          <Choice key={s.host} on={server === s.host && !customOpen} title={s.name} detail={`${s.location} · ${s.host} · ${s.note}`}
            ping={s.host in pings ? pings[s.host] : false} onPick={() => pick(s.host)} />
        ))}
        <Choice on={customOpen} title="Another server" detail="A server of your own or a friend's, as host or host:port."
          ping={server !== '' && !preset ? pings[server] : false} onPick={() => setCustomOpen(true)} />
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
              onBlur={commitCustom}
              onKeyDown={(e) => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); }}
            />
            {customError && <span className="server-error">Use a host name or address, optionally followed by :port.</span>}
          </div>
        )}
      </div>
    </>
  );
}
