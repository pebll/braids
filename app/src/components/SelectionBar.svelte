<!--
  SelectionBar.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Shown instead of the composer while messages of the chat are selected:
  how many, and what can be done with all of them (forward, copy, delete).
  Esc ends the selection.
-->
<script lang="ts">
  import { Copy, Forward, Trash2, X } from '@lucide/svelte';
  import { copySelection, deleteSelection, selectedMessages } from '../lib/actions';
  import { ui } from '../lib/ui.svelte';

  const messages = $derived(ui.selection ? selectedMessages() : []);
  const count = $derived(ui.selection?.ids.length ?? 0);
  const canDelete = $derived(messages.length > 0 && messages.every((m) => m.isOutgoing));

  function cancel() {
    ui.selection = null;
  }

  function onKey(e: KeyboardEvent) {
    if (e.key !== 'Escape' || ui.forward || ui.confirmRequest || ui.menu || ui.lightbox) return;
    e.preventDefault();
    cancel();
  }
</script>

<svelte:window onkeydown={onKey} />

<div class="selection-bar" role="toolbar" aria-label="Selected messages">
  <button class="icon-btn" aria-label="Cancel selection" title="Cancel (Esc)" onclick={cancel}><X size={18} /></button>
  <span class="count">{count} selected</span>
  <span class="grow"></span>
  <button class="btn" onclick={copySelection} disabled={!messages.length} title="Copy (Ctrl+C)"><Copy size={16} />Copy</button>
  <button class="btn" onclick={() => (ui.forward = messages)} disabled={!messages.length}><Forward size={16} />Forward</button>
  {#if canDelete}
    <button class="btn danger" onclick={deleteSelection}><Trash2 size={16} />Delete</button>
  {/if}
</div>

<style>
  .selection-bar {
    display: flex; align-items: center; gap: 8px; padding: 10px 16px; border-top: 1px solid var(--border);
    background: var(--bg-1); min-height: 60px;
  }
  .count { font-size: 14px; font-weight: 600; }
  .grow { flex: 1; }
  .btn { display: inline-flex; align-items: center; gap: 6px; white-space: nowrap; }
  .btn.danger { color: var(--danger); }
</style>
