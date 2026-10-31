<!--
  ForwardDialog.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Forward a message: pick a chat of any account, the message (text and
  attachment) is sent there as a new message, optionally with a "Forwarded
  from" line on top.
-->
<script lang="ts">
  import { LoaderCircle, Search, X } from '@lucide/svelte';
  import { tick } from 'svelte';
  import * as api from '../lib/api';
  import { chatKey, fileReady, store } from '../lib/store.svelte';
  import type { Chat, Message } from '../lib/types';
  import { ui } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';

  let { m }: { m: Message } = $props();

  let query = $state('');
  let withOrigin = $state(true);
  let busy = $state('');
  let input: HTMLInputElement | undefined = $state();

  const source = $derived(store.chats[chatKey(m.accountId, m.chatId)]);
  const origin = $derived.by(() => {
    const sender = m.isOutgoing ? 'me' : store.senderName(m);
    return source?.isGroup ? `Forwarded from ${sender} (in ${store.chatName(source)})` : `Forwarded from ${sender}`;
  });

  const chats = $derived.by(() => {
    const q = query.trim().toLowerCase();
    return Object.values(store.chats)
      .filter((c) => store.accounts[c.accountId] && store.category(c) !== 'archived')
      .filter((c) => !q || store.chatName(c).toLowerCase().includes(q))
      .sort((a, b) => b.lastActivity - a.lastActivity)
      .slice(0, 200);
  });

  $effect(() => {
    tick().then(() => input?.focus());
  });

  function close() {
    ui.forward = null;
  }

  // the attachment must be local to be sent again
  async function localFile(): Promise<string> {
    if (!m.file) return '';
    const current = () => store.messages[chatKey(m.accountId, m.chatId)]?.items.find((x) => x.id === m.id) ?? m;
    if (fileReady(current().file)) return current().file!.path;
    await api.call('files.download', { accountId: m.accountId, chatId: m.chatId, msgId: m.id });
    for (let i = 0; i < 600; i++) {
      await new Promise((r) => setTimeout(r, 100));
      const f = current().file;
      if (fileReady(f)) return f!.path;
      if (f?.status === 'failed') break;
    }
    throw new Error('The attachment could not be downloaded.');
  }

  async function send(c: Chat) {
    const key = chatKey(c.accountId, c.id);
    busy = key;
    try {
      const filePath = await localFile();
      const text = [withOrigin ? `_${origin}_` : '', m.text].filter(Boolean).join('\n');
      await api.call('messages.send', { accountId: c.accountId, chatId: c.id, ...(text ? { text } : {}), ...(filePath ? { filePath } : {}) });
      close();
      store.toast(`Forwarded to ${store.chatName(c)}.`, 'info', { label: 'Open', run: () => store.openChat(key) });
    } catch (e) {
      store.toastError(e);
    } finally {
      busy = '';
    }
  }
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && close()} />

<div class="scrim" role="presentation" onclick={(e) => e.target === e.currentTarget && close()}>
  <div class="dialog" role="dialog" aria-modal="true" aria-labelledby="forward-title">
    <div class="dialog-head">
      <h2 id="forward-title">Forward message</h2>
      <button class="icon-btn" aria-label="Close" onclick={close}><X size={18} /></button>
    </div>
    <div class="dialog-body">
      <div class="preview">
        {#if withOrigin}<em>{origin}</em>{/if}
        <span>{store.previewText(m) || m.file?.name || 'Attachment'}</span>
      </div>
      <label class="origin">
        <input type="checkbox" bind:checked={withOrigin} />
        Add “Forwarded from” line
      </label>
      <label class="search">
        <Search size={16} />
        <input bind:this={input} bind:value={query} placeholder="Search chats" aria-label="Search chats to forward to" />
      </label>
      <div class="list scroll">
        {#each chats as c (chatKey(c.accountId, c.id))}
          <button class="target" onclick={() => send(c)} disabled={!!busy}>
            <Avatar name={store.chatName(c)} id={c.id} size={36} accountId={c.accountId} group={c.isGroup} ring="var(--surface)" />
            <span class="cname">{store.chatName(c)}<small>{store.accountName(c.accountId)}</small></span>
            {#if busy === chatKey(c.accountId, c.id)}<LoaderCircle size={16} class="spin" />{/if}
          </button>
        {:else}
          <p class="empty">No matching chats.</p>
        {/each}
      </div>
    </div>
  </div>
</div>

<style>
  .preview {
    display: flex; flex-direction: column; gap: 2px; padding: 8px 10px; border-left: 3px solid var(--accent); border-radius: 6px;
    background: var(--surface-2); font-size: 13px; color: var(--text-2); max-height: 84px; overflow: hidden;
  }
  .preview em { color: var(--text-muted); }
  .origin { display: flex; align-items: center; gap: 8px; font-size: 13px; }
  .search { display: flex; align-items: center; gap: 8px; height: 38px; padding: 0 10px; border-radius: var(--radius-md); background: var(--surface-2); border: 1px solid var(--border); color: var(--text-muted); }
  .search input { flex: 1; border: 0; outline: none; background: transparent; color: var(--text); }
  .list { max-height: 320px; display: flex; flex-direction: column; gap: 1px; margin: 0 -8px; }
  .target { display: flex; align-items: center; gap: 10px; padding: 6px 8px; border: 0; background: none; border-radius: var(--radius-md); text-align: left; }
  .target:hover { background: var(--hover); }
  .cname { flex: 1; min-width: 0; display: flex; flex-direction: column; font-size: 14px; }
  .cname small { font-size: 12px; color: var(--text-muted); }
  .empty { color: var(--text-muted); font-size: 13px; text-align: center; }
</style>
