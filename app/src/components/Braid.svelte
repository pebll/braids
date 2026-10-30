<!--
  Braid.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  The braid motif: a horizontal plait of three strands that slides (static
  with prefers-reduced-motion). Only for welcome, empty states and syncing.
-->
<script lang="ts">
  let { height = 40, width = 320, animated = true, faint = false }: { height?: number; width?: number; animated?: boolean; faint?: boolean } = $props();

  const strands = [0, 1, 2];
  const period = $derived(Math.round(height * 2.6));
  const count = $derived(Math.ceil(width / period) + 2);

  // each strand is a sine wave shifted by a third of the period
  function strand(i: number): string {
    const amp = height / 2 - 5;
    const mid = height / 2;
    const off = (i * period) / 3;
    let d = '';
    for (let x = -period; x <= count * period; x += 3) {
      const y = mid + amp * Math.sin(((x + off) / period) * 2 * Math.PI);
      d += `${d ? 'L' : 'M'}${x.toFixed(1)} ${y.toFixed(1)}`;
    }
    return d;
  }
</script>

<svg class="braid" class:animated class:faint {width} {height} viewBox="0 0 {width} {height}" aria-hidden="true">
  <g class="slide" style:--period="{period}px">
    {#each strands as i}
      <path d={strand(i)} class="s{i}" fill="none" stroke-width={Math.max(3, height / 10)} stroke-linecap="round" />
    {/each}
  </g>
</svg>

<style>
  .braid { display: block; overflow: hidden; }
  .s0 { stroke: var(--accent); }
  .s1 { stroke: var(--border-strong); }
  .s2 { stroke: color-mix(in srgb, var(--accent) 45%, var(--border)); }
  .faint .s0, .faint .s2 { opacity: 0.35; }
  .faint .s1 { opacity: 0.6; }
  .animated .slide { animation: slide var(--duration-braid) linear infinite; }
  @keyframes slide { to { transform: translateX(calc(-1 * var(--period))); } }
  @media (prefers-reduced-motion: reduce) { .animated .slide { animation: none; } }
</style>
