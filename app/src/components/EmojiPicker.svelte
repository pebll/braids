<!--
  EmojiPicker.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  All emoji by category, search by name and keyword, recently used first.
  The emoji data (emojibase) is loaded when the picker opens for the first
  time. Esc or a click outside closes it.
-->
<script module lang="ts">
  const RECENT_KEY = 'braids.emoji.recent';
  function recent(): string[] {
    try {
      return JSON.parse(localStorage.getItem(RECENT_KEY) ?? '[]');
    } catch {
      return [];
    }
  }
</script>

<script lang="ts">
  import { Apple, Car, Clock, Flag, Heart, Lightbulb, PawPrint, Search, Smile, Trophy, Users } from '@lucide/svelte';
  import { onMount, tick } from 'svelte';
  import { loadEmoji, type Emoji } from '../lib/emoji';

  let { onPick, onClose }: { onPick: (emoji: string) => void; onClose: () => void } = $props();
  let root: HTMLDivElement | undefined = $state();

  function onOutside(e: PointerEvent) {
    const t = e.target as HTMLElement;
    if (root && !root.contains(t) && !t.closest('[aria-label="Emoji"]')) onClose();
  }

  const GROUPS = [
    { id: -1, label: 'Recently used', icon: Clock },
    { id: 0, label: 'Smileys & emotion', icon: Smile },
    { id: 1, label: 'People & body', icon: Users },
    { id: 3, label: 'Animals & nature', icon: PawPrint },
    { id: 4, label: 'Food & drink', icon: Apple },
    { id: 5, label: 'Travel & places', icon: Car },
    { id: 6, label: 'Activities', icon: Trophy },
    { id: 7, label: 'Objects', icon: Lightbulb },
    { id: 8, label: 'Symbols', icon: Heart },
    { id: 9, label: 'Flags', icon: Flag },
  ];

  let all = $state<Emoji[]>([]);
  let query = $state('');
  let recents = $state<string[]>(recent());
  let body: HTMLDivElement | undefined = $state();
  let searchInput: HTMLInputElement | undefined = $state();

  onMount(() => {
    loadEmoji().then((list) => (all = list));
    tick().then(() => searchInput?.focus());
  });

  const results = $derived.by(() => {
    const q = query.trim().toLowerCase();
    if (!q) return [];
    const words = q.split(/\s+/);
    return all.filter((e) => words.every((w) => e.label.includes(w) || e.tags?.some((t) => t.startsWith(w)))).slice(0, 160);
  });

  const sections = $derived(
    GROUPS.map((g) => ({ ...g, items: g.id === -1 ? recents : all.filter((e) => e.group === g.id).map((e) => e.unicode) }))
      .filter((s) => s.items.length));

  const labels = $derived(new Map(all.map((e) => [e.unicode, e.label])));

  function pick(emoji: string) {
    recents = [emoji, ...recents.filter((e) => e !== emoji)].slice(0, 24);
    try {
      localStorage.setItem(RECENT_KEY, JSON.stringify(recents));
    } catch {
      // not kept
    }
    onPick(emoji);
  }

  function jump(id: number) {
    query = '';
    tick().then(() => body?.querySelector(`[data-group="${id}"]`)?.scrollIntoView({ block: 'start' }));
  }

  function onKey(e: KeyboardEvent) {
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      onClose();
    } else if (e.key === 'Enter' && results.length) {
      e.preventDefault();
      pick(results[0].unicode);
    }
  }
</script>

<svelte:window onpointerdown={onOutside} />

<div class="picker" role="dialog" aria-label="Emoji picker" bind:this={root}>
  <label class="find">
    <Search size={15} />
    <input bind:this={searchInput} bind:value={query} placeholder="Search emoji" aria-label="Search emoji" onkeydown={onKey} />
  </label>
  {#if !query.trim()}
    <div class="tabs">
      {#each sections as s (s.id)}
        <button class="tab" title={s.label} aria-label={s.label} onclick={() => jump(s.id)}><s.icon size={16} /></button>
      {/each}
    </div>
  {/if}
  <div class="body scroll" bind:this={body}>
    {#if query.trim()}
      <div class="grid">
        {#each results as e (e.unicode)}
          <button class="em" title={e.label} aria-label={e.label} onclick={() => pick(e.unicode)}>{e.unicode}</button>
        {:else}
          <p class="none">{all.length ? 'No emoji found.' : 'Loading…'}</p>
        {/each}
      </div>
    {:else}
      {#each sections as s (s.id)}
        <section data-group={s.id}>
          <h4>{s.label}</h4>
          <div class="grid">
            {#each s.items as u (u)}
              <button class="em" title={labels.get(u) ?? u} aria-label={labels.get(u) ?? u} onclick={() => pick(u)}>{u}</button>
            {/each}
          </div>
        </section>
      {:else}
        <p class="none">Loading…</p>
      {/each}
    {/if}
  </div>
</div>

<style>
  .picker {
    position: absolute; bottom: 46px; right: 0; width: 340px; height: 380px; display: flex; flex-direction: column; gap: 6px; padding: 8px;
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius-lg); box-shadow: var(--shadow-2); z-index: 10;
  }
  .find { display: flex; align-items: center; gap: 6px; height: 32px; padding: 0 8px; border-radius: var(--radius-md); background: var(--surface-2); border: 1px solid var(--border); color: var(--text-muted); }
  .find input { flex: 1; min-width: 0; border: 0; outline: none; background: transparent; color: var(--text); font-size: 13px; }
  .tabs { display: flex; justify-content: space-between; }
  .tab { width: 30px; height: 28px; border: 0; background: none; border-radius: 6px; color: var(--text-muted); display: flex; align-items: center; justify-content: center; }
  .tab:hover { background: var(--hover); color: var(--text); }
  .body { flex: 1; min-height: 0; overflow-y: auto; }
  section { content-visibility: auto; contain-intrinsic-size: auto 300px; }
  h4 { margin: 6px 2px 2px; font-size: 11px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.04em; color: var(--text-muted); }
  .grid { display: grid; grid-template-columns: repeat(9, 1fr); gap: 1px; }
  .em { border: 0; background: none; height: 34px; border-radius: 6px; font-size: 21px; padding: 0; line-height: 1; }
  .em:hover, .em:focus-visible { background: var(--hover); }
  .none { grid-column: 1 / -1; text-align: center; color: var(--text-muted); font-size: 13px; }
</style>
