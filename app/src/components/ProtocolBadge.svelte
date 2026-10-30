<!--
  ProtocolBadge.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Rounded tile in the account color with the protocol glyph (doc/design).
-->
<script lang="ts">
  import { protocolMeta } from '../lib/protocols';

  let { protocol, color, size = 18, ring = '' }: { protocol: string; color: string; size?: number; ring?: string } = $props();
  const meta = $derived(protocolMeta(protocol));
</script>

<span
  class="badge"
  title={meta.name}
  style:width="{size}px"
  style:height="{size}px"
  style:border-radius="{Math.round(size / 3)}px"
  style:background={color}
  style:box-shadow={ring ? `0 0 0 2px ${ring}` : 'none'}
>
  {#if meta.iconPath}
    <svg viewBox="0 0 24 24" width={Math.round(size * 0.62)} height={Math.round(size * 0.62)} aria-hidden="true"><path d={meta.iconPath} fill="#fff" /></svg>
  {:else}
    <span style:font-size="{Math.round(size * 0.6)}px">{meta.short}</span>
  {/if}
</span>

<style>
  .badge {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    color: #fff;
    font-weight: 700;
    line-height: 1;
  }
</style>
