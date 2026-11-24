import { useEffect, useState } from 'react';
import { api } from './bridge';

export type ArtKind = 'cover' | 'banner';

// The cover/banner files live in the launcher's data folder. The UI fetches them once and caches them here, so a
// settings change never carries the image bytes again.
const cache = new Map<string, string>();
const key = (discId: string, kind: ArtKind) => `${discId}:${kind}`;

export async function getGameArt(discId: string, kind: ArtKind): Promise<string | undefined> {
  if (!discId) return undefined;
  const k = key(discId, kind);
  const cached = cache.get(k);
  if (cached) return cached;
  const url = await api.mhf_getGameArt(discId, kind);
  if (url) cache.set(k, url);
  return url || undefined;
}

export function clearGameArt(discId: string, kind?: ArtKind) {
  if (!kind) {
    cache.delete(key(discId, 'cover'));
    cache.delete(key(discId, 'banner'));
  } else {
    cache.delete(key(discId, kind));
  }
}

export function useGameArt(discId: string | undefined, kind: ArtKind, has: boolean, version?: number): string | undefined {
  const [url, setUrl] = useState<string | undefined>(() =>
    discId && has ? cache.get(key(discId, kind)) : undefined
  );
  useEffect(() => {
    if (!discId || !has) {
      setUrl(undefined);
      return;
    }
    const cached = cache.get(key(discId, kind));
    if (cached) {
      setUrl(cached);
      return;
    }
    setUrl(undefined);
    let cancelled = false;
    getGameArt(discId, kind).then((next) => {
      if (!cancelled) setUrl(next);
    });
    return () => { cancelled = true; };
  }, [discId, kind, has, version]);
  return url;
}
