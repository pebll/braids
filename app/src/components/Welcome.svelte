<!--
  Welcome.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  First start: no accounts yet.
-->
<script lang="ts">
  import { ChevronRight, LoaderCircle, Lock, TriangleAlert } from '@lucide/svelte';
  import * as api from '../lib/api';
  import { protocolMeta } from '../lib/protocols';
  import { store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';
  import { availableProtocols } from '../lib/visible';
  import Braid from './Braid.svelte';
  import Logo from './Logo.svelte';
  import ProtocolBadge from './ProtocolBadge.svelte';

  let starting = $state('');

  async function choose(protocol: string) {
    if (starting) return;
    starting = protocol;
    try {
      store.wizardSetupId = await store.addAccount(protocol);
    } catch (e) {
      store.toastError(e);
    } finally {
      starting = '';
    }
  }
</script>

<div class="welcome">
  <div class="content">
    <Logo size={68} />
    <h1>Welcome to braids</h1>
    <p class="lead">Telegram, WhatsApp, Signal and Matrix — in one calm inbox.</p>
    <p class="privacy"><Lock size={13} />Everything stays on this computer. No account, no cloud, no telemetry.</p>

    <div class="label">Connect your first account</div>
    {#if !store.connected}
      <div class="waiting"><LoaderCircle size={16} class="spin" />Starting braids core…</div>
    {/if}
    <div class="grid">
      {#each availableProtocols() as p}
        {@const pm = protocolMeta(p)}
        <button class="card" onclick={() => choose(p)} disabled={!!starting}>
          <ProtocolBadge protocol={p} color={`var(--acct-${pm.color})`} size={40} />
          <span class="text">
            <b>{pm.name}</b>
            <span>{pm.description}</span>
            {#if pm.warning}<span class="warn"><TriangleAlert size={13} />{pm.warning}</span>{/if}
          </span>
          {#if starting === p}<LoaderCircle size={18} class="spin" />{:else}<ChevronRight size={18} />{/if}
        </button>
      {/each}
    </div>
    <p class="foot">
      Open source · AGPL v3 ·
      <a href="https://github.com/pebll/braids" onclick={(e) => { e.preventDefault(); api.openUrl('https://github.com/pebll/braids'); }}>Source code</a>
      · <button class="link-btn" onclick={() => { ui.settingsSection = 'general'; ui.settingsOpen = true; }}>Settings</button>
    </p>
  </div>
  <div class="braid" aria-hidden="true"><Braid width={Math.max(ui.width, 400)} height={80} faint /></div>
</div>

<style>
  .welcome { flex: 1; display: flex; flex-direction: column; background: var(--bg-1); overflow-y: auto; position: relative; min-height: 0; }
  .content { flex: 1; display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 48px 16px 120px; text-align: center; gap: 4px; }
  h1 { font-size: 30px; line-height: 38px; font-weight: 700; margin: 18px 0 6px; letter-spacing: -0.01em; }
  .lead { margin: 0; font-size: 16px; color: var(--text-2); }
  .privacy { margin: 8px 0 0; display: flex; align-items: center; gap: 6px; color: var(--text-2); font-size: 13px; }
  .label { margin-top: 36px; align-self: center; width: min(560px, 100%); text-align: left; font-size: 13px; font-weight: 600; color: var(--text-2); }
  .waiting { display: flex; align-items: center; gap: 8px; color: var(--text-muted); font-size: 13px; margin-top: 8px; }
  .grid { margin-top: 10px; width: min(560px, 100%); display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 10px; }
  .card {
    display: flex; gap: 14px; align-items: flex-start; padding: 16px; border-radius: var(--radius-lg); border: 1px solid var(--border);
    background: var(--surface); text-align: left; box-shadow: var(--shadow-1); color: var(--text-2);
  }
  .card:hover:not(:disabled) { border-color: var(--border-strong); box-shadow: var(--shadow-2); }
  .card :global(svg:last-child) { margin-top: 2px; }
  .text { flex: 1; display: flex; flex-direction: column; gap: 2px; font-size: 13px; color: var(--text-2); }
  .text b { font-size: 15px; color: var(--text); }
  .warn { display: flex; gap: 5px; align-items: flex-start; color: var(--warning); font-size: 12px; margin-top: 4px; line-height: 16px; }
  .warn :global(svg) { flex-shrink: 0; margin-top: 1px; }
  .foot { margin-top: 24px; font-size: 12px; color: var(--text-muted); }
  .braid { position: absolute; left: 0; right: 0; bottom: 30px; pointer-events: none; overflow: hidden; }
</style>
