import { useEffect, useState, type ReactElement } from 'react';
import { api, onEvent, type PackReview, type State } from './bridge';

const bytes = (n: number) => (n >= 1 << 30 ? `${(n / (1 << 30)).toFixed(1)} GB` : `${Math.max(1, Math.round(n / (1 << 20)))} MB`);
const count = (n: number) => n.toLocaleString('en');

// Game settings > Texture pack: an HD texture pack in PPSSPP's format (a folder with textures.ini, such as KHD) for this
// copy of the game. Choosing a folder reviews the pack first (the ini this release uses, how many textures, missing
// images, size); nothing is used or copied until the player says how: copied into MHForever's folder (it keeps working
// if the download is deleted), or used where it is (no space taken, but it must stay there).
export function TexturePackSection({ state, onState, say, toggle }: {
  state: State;
  onState: (s: State) => void;
  say: (text: string) => void;
  toggle: (props: { on: boolean; onToggle: () => void; label: string }) => ReactElement;
}) {
  const [review, setReview] = useState<PackReview | null>(null);
  const [copying, setCopying] = useState<{ progress: number; file: string } | null>(null);
  const [confirmRemove, setConfirmRemove] = useState(false);
  const s = state.settings;
  const pack = s.texturePack ?? '';

  useEffect(() => onEvent((e) => {
    if (e.type === 'texturePackProgress') setCopying({ progress: e.progress, file: e.file });
    else if (e.type === 'texturePackDone') {
      setCopying(null);
      setReview(null);
      say('Texture pack copied and turned on');
    } else if (e.type === 'texturePackFailed') {
      setCopying(null);
      if (e.message !== 'Cancelled') say(e.message);
    }
  }), [say]);

  const run = (mode: Parameters<typeof api.mhf_texturePack>[0]) => api.mhf_texturePack(mode).then((r) => {
    onState(r.state);
    if (r.error) say(r.error);
    return r;
  });
  const choose = () => run('review').then((r) => {
    if (r.picked && r.review) {
      setReview(r.review);
      if (!r.review.ok) say(r.review.error);
    }
  });

  return (
    <>
      <div className="srow">
        <div className="slabel">
          <b>HD textures</b>
          <span className="prose">
            Texture packs made for PPSSPP (a folder with a textures.ini), such as KHD, replace the game's textures with sharper
            ones. None is included: download one, unzip it, and choose its folder. This copy uses the pack's textures for its release.
          </span>
          {pack ? (
            <>
              <span className="path">{pack}</span>
              <span>{s.texturePackSummary ?? ''}{state.texturePackInstalled ? ' · copied into MHForever' : ' · used where it is'}</span>
            </>
          ) : (
            <span className="path">No pack chosen</span>
          )}
        </div>
        <div className="row-actions">
          {!copying && <button className="ghost mid" onClick={choose}>{pack ? 'Change…' : 'Choose a pack…'}</button>}
          {pack && !copying && !confirmRemove && (
            <button className="ghost mid" onClick={() => (state.texturePackInstalled ? setConfirmRemove(true) : run('none'))}>Remove</button>
          )}
          {confirmRemove && (
            <>
              <button className="ghost mid danger" onClick={() => run('none').then(() => { setConfirmRemove(false); say('Texture pack deleted'); })}>Delete the copy</button>
              <button className="ghost mid" onClick={() => setConfirmRemove(false)}>Keep it</button>
            </>
          )}
          {pack && toggle({ on: !!s.texturePackEnabled, onToggle: () => run(s.texturePackEnabled ? 'off' : 'on'), label: 'Use the texture pack' })}
        </div>
      </div>

      {review && review.ok && !copying && (
        <div className="srow" style={{ flexDirection: 'column', alignItems: 'stretch', gap: 10 }}>
          <div className="slabel">
            <b>Pack found</b>
            <span className="path">{review.folder}</span>
            <span>
              {review.label}: {count(review.keys)} textures ({review.ini})
              {review.missing ? `, ${count(review.missing)} image${review.missing === 1 ? '' : 's'} missing: the game's own texture stays for ${review.missing === 1 ? 'it' : 'those'}` : ''}
            </span>
            <span>{count(review.images)} images, {bytes(review.bytes)}.{review.installed ? ' This is the copy MHForever already has.' : ''}</span>
            {review.warnings.map((w) => <span key={w} className="prose">{w}</span>)}
          </div>
          <div className="row-actions">
            {!review.installed && <button className="ghost mid" onClick={() => run('copy').then((r) => { if (!r.error) setCopying({ progress: 0, file: '' }); })}>Copy into MHForever ({bytes(review.bytes)})</button>}
            <button className="ghost mid" onClick={() => run('use').then((r) => { if (!r.error) { setReview(null); say('Texture pack turned on'); } })}>Use it where it is</button>
            <button className="ghost mid" onClick={() => setReview(null)}>Cancel</button>
          </div>
        </div>
      )}

      {copying && (
        <div className="srow" style={{ flexDirection: 'column', alignItems: 'stretch', gap: 10 }}>
          <div className="slabel">
            <b>Copying the texture pack… {Math.round(copying.progress * 100)}%</b>
            <span className="path">{copying.file}</span>
          </div>
          <div className="progress" aria-hidden="true"><div style={{ width: `${Math.round(copying.progress * 100)}%` }} /></div>
          <div className="row-actions">
            <button className="ghost mid" onClick={() => run('cancel')}>Cancel</button>
          </div>
        </div>
      )}
    </>
  );
}
