<!--
  Overlays.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Context menu, confirmation dialog, image lightbox, toasts.
-->
<script lang="ts">
  import { ExternalLink, X } from '@lucide/svelte';
  import { tick } from 'svelte';
  import * as api from '../lib/api';
  import { store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';

  let menuEl: HTMLDivElement | undefined = $state();
  let confirmBtn: HTMLButtonElement | undefined = $state();
  let menuPos = $state({ x: 0, y: 0 });

  // keep the menu inside the window
  $effect(() => {
    const m = ui.menu;
    if (!m) return;
    menuPos = { x: m.x, y: m.y };
    tick().then(() => {
      if (!menuEl) return;
      const r = menuEl.getBoundingClientRect();
      menuPos = {
        x: Math.max(8, Math.min(m.x, window.innerWidth - r.width - 8)),
        y: Math.max(8, m.y + r.height > window.innerHeight - 8 ? m.y - r.height : m.y),
      };
      menuEl.querySelector<HTMLButtonElement>('button:not(:disabled)')?.focus();
    });
  });

  $effect(() => {
    if (ui.confirmRequest) tick().then(() => confirmBtn?.focus());
  });

  function answer(ok: boolean) {
    ui.confirmRequest?.resolve(ok);
    ui.confirmRequest = null;
  }

  function onKey(e: KeyboardEvent) {
    if (e.key !== 'Escape') return;
    if (ui.menu) ui.menu = null;
    else if (ui.confirmRequest) answer(false);
    else if (ui.lightbox) ui.lightbox = null;
  }

  function onMenuKey(e: KeyboardEvent) {
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    e.preventDefault();
    const buttons = Array.from(menuEl?.querySelectorAll<HTMLButtonElement>('button:not(:disabled)') ?? []);
    const i = buttons.indexOf(document.activeElement as HTMLButtonElement);
    buttons[(i + (e.key === 'ArrowDown' ? 1 : buttons.length - 1)) % buttons.length]?.focus();
  }
</script>

<svelte:window onkeydown={onKey} />

{#if ui.menu}
  <div class="menu-scrim" role="presentation" onclick={() => (ui.menu = null)} oncontextmenu={(e) => { e.preventDefault(); ui.menu = null; }}></div>
  <div class="menu" role="menu" tabindex="-1" bind:this={menuEl} style:left="{menuPos.x}px" style:top="{menuPos.y}px" onkeydown={onMenuKey}>
    {#each ui.menu.items as item}
      <button role="menuitem" class:danger={item.danger} disabled={item.disabled} title={item.hint} onclick={() => { ui.menu = null; item.run(); }}>
        {#if item.icon}<item.icon size={16} />{/if}
        <span>{item.label}</span>
        {#if item.hint}<span class="menu-hint">{item.hint}</span>{/if}
      </button>
    {/each}
  </div>
{/if}

{#if ui.confirmRequest}
  {@const c = ui.confirmRequest}
  <div class="scrim" role="presentation" onclick={(e) => e.target === e.currentTarget && answer(false)}>
    <div class="dialog confirm" role="alertdialog" aria-modal="true" aria-labelledby="confirm-title">
      <div class="dialog-head"><h2 id="confirm-title">{c.title}</h2></div>
      <div class="dialog-body"><p class="sub">{c.body}</p></div>
      <div class="dialog-foot">
        <button class="btn ghost" onclick={() => answer(false)}>Cancel</button>
        <button class="btn" class:danger={c.danger} class:primary={!c.danger} bind:this={confirmBtn} onclick={() => answer(true)}>{c.confirm}</button>
      </div>
    </div>
  </div>
{/if}

{#if ui.lightbox}
  {@const lb = ui.lightbox}
  <div class="lightbox" role="dialog" aria-modal="true" aria-label={lb.name}>
    <div class="lb-bar">
      <span class="lb-name">{lb.name}</span>
      <button class="icon-btn" aria-label="Open with default app" title="Open with default app" onclick={() => api.openPath(lb.path).catch((e) => store.toastError(e))}><ExternalLink size={19} /></button>
      <button class="icon-btn" aria-label="Close" onclick={() => (ui.lightbox = null)}><X size={20} /></button>
    </div>
    <div class="lb-img" role="presentation" onclick={(e) => e.target === e.currentTarget && (ui.lightbox = null)}>
      <img src={lb.src} alt={lb.name} />
    </div>
  </div>
{/if}

{#if store.toasts.length}
  <div class="toasts" role="status" aria-live="polite">
    {#each store.toasts as t (t.id)}
      <div class="toast" class:error={t.kind === 'error'}>
        <span>{t.text}</span>
        {#if t.action}<button onclick={() => { t.action!.run(); store.dismissToast(t.id); }}>{t.action.label}</button>{/if}
        <button aria-label="Dismiss" onclick={() => store.dismissToast(t.id)}><X size={14} /></button>
      </div>
    {/each}
  </div>
{/if}

<style>
  .menu-scrim { position: fixed; inset: 0; z-index: 79; }
  .confirm { width: 400px; }
  .lightbox { position: fixed; inset: 0; z-index: 70; background: rgba(15, 14, 13, 0.92); display: flex; flex-direction: column; animation: fade var(--duration-fast) var(--ease); }
  .lb-bar { display: flex; align-items: center; gap: 4px; padding: 10px 12px 10px 20px; color: #f2efeb; }
  .lb-bar .icon-btn { color: #f2efeb; }
  .lb-bar .icon-btn:hover { background: rgba(255, 255, 255, 0.1); }
  .lb-name { flex: 1; font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .lb-img { flex: 1; min-height: 0; display: flex; align-items: center; justify-content: center; padding: 0 24px 24px; }
  .lb-img img { max-width: 100%; max-height: 100%; object-fit: contain; border-radius: 6px; }
</style>
