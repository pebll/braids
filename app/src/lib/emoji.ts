// emoji.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// Emoji data loaded the first time it is needed, shared by
// emoji picker and the ":name" suggestions appears.

export interface Emoji { unicode: string; label: string; tags?: string[]; group?: number; order?: number }

let data: Promise<Emoji[]> | null = null;

export function loadEmoji(): Promise<Emoji[]> {
  data ??= import('emojibase-data/en/compact.json').then((m) =>
    (m.default as Emoji[])
      .filter((e) => e.group !== undefined && e.group !== 2)
      .map((e) => ({ ...e, unicode: e.unicode.replace(/[\uFE0E\uFE0F]/g, '') }))
      .sort((a, b) => (a.order ?? 0) - (b.order ?? 0)));
  return data;
}
export function searchEmoji(all: Emoji[], query: string, limit: number): Emoji[] {
  const words = query.toLowerCase().replace(/_/g, ' ').split(/\s+/).filter(Boolean);
  if (!words.length) return [];
  const q = words.join(' ');
  const found: { e: Emoji; rank: number }[] = [];
  for (const e of all) {
    if (!words.every((w) => e.label.includes(w) || e.tags?.some((t) => t.startsWith(w)))) continue;
    const rank = e.label.startsWith(q) ? 0 : e.label.split(' ').some((w) => w.startsWith(words[0])) ? 1 : 2;
    found.push({ e, rank });
  }
  return found.sort((a, b) => a.rank - b.rank).slice(0, limit).map((f) => f.e);
}