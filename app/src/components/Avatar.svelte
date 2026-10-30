<!--
  Avatar.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.
-->
<script lang="ts">
  import { avatarColor, initials } from '../lib/format';
  import { store } from '../lib/store.svelte';
  import ProtocolBadge from './ProtocolBadge.svelte';

  let {
    name,
    id,
    size = 44,
    accountId = '',
    online = false,
    ring = 'var(--bg-1)',
    group = false,
  }: { name: string; id: string; size?: number; accountId?: string; online?: boolean; ring?: string; group?: boolean } = $props();

  const account = $derived(accountId ? store.accounts[accountId] : undefined);
  const badge = $derived(size >= 40 ? 18 : size >= 30 ? 14 : 12);

  // groups: a nearly square octagon (small corner cuts) with rounded corners
  function octagon(s: number): string {
    const c = s * 0.22; // corner cut
    const v = [[c, 0], [s - c, 0], [s, c], [s, s - c], [s - c, s], [c, s], [0, s - c], [0, c]];
    const r = 0.45; // rounding: part of each edge next to a corner
    const at = (a: number[], b: number[], t: number) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t];
    const f = (p: number[]) => `${p[0].toFixed(2)} ${p[1].toFixed(2)}`;
    let d = '';
    v.forEach((p, i) => {
      const prev = v[(i + 7) % 8];
      const next = v[(i + 1) % 8];
      // keep the rounding of the long edges as short as on the corner cuts
      const tPrev = Math.min(r, (r * c * Math.SQRT2) / Math.hypot(prev[0] - p[0], prev[1] - p[1]));
      const tNext = Math.min(r, (r * c * Math.SQRT2) / Math.hypot(next[0] - p[0], next[1] - p[1]));
      d += `${i === 0 ? 'M' : 'L'}${f(at(p, prev, tPrev))} Q${f(p)} ${f(at(p, next, tNext))} `;
    });
    return `path('${d}Z')`;
  }
</script>

<span class="avatar" style:width="{size}px" style:height="{size}px">
  <span
    class="circle"
    class:group
    style:clip-path={group ? octagon(size) : undefined}
    style:background={avatarColor(id || name)}
    style:font-size="{Math.round(size * (size >= 40 ? 0.39 : 0.37))}px"
    aria-hidden="true">{initials(name)}</span
  >
  {#if account}
    <span class="badge"><ProtocolBadge protocol={account.protocol} color={store.accountColor(account.accountId)} size={badge} {ring} /></span>
  {/if}
  {#if online}
    <span class="online" style:box-shadow="0 0 0 2px {ring}" title="Online"></span>
  {/if}
</span>

<style>
  .avatar { position: relative; flex-shrink: 0; display: block; }
  .circle {
    width: 100%; height: 100%; border-radius: 50%; color: #1c1917; font-weight: 600; display: flex;
    align-items: center; justify-content: center; letter-spacing: 0.01em; user-select: none;
  }
  /* groups: rounded octagon (clip-path set inline), people: circle */
  .circle.group { border-radius: 0; }
  .badge { position: absolute; right: -2px; bottom: -2px; display: flex; }
  .online { position: absolute; right: 0; top: 0; width: 10px; height: 10px; border-radius: 50%; background: var(--success); }
</style>
