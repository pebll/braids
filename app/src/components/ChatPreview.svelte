<!--
  ChatPreview.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Hover preview of a chat in the list (#40): its unread messages (up to five),
  else the last ones, in full, next to the row. Only reads the history
  (messages.get): nothing is marked as read, the chat stays closed.
-->
<script lang="ts">
  import { LoaderCircle } from '@lucide/svelte';
  import * as api from '../lib/api';
  import { renderText, timeOfDay, listTime, isSameDay } from '../lib/format';
  import { chatKey, store } from '../lib/store.svelte';
  import type { Chat, Message } from '../lib/types';

  let { chat, top, left }: { chat: Chat; top: number; left: number } = $props();

  const UNREAD_SHOWN = 5;
  const READ_SHOWN = 3;

  let messages = $state<Message[] | null>(null);
  let el: HTMLDivElement | undefined = $state();
  let y = $state(0);

  $effect(() => {
    const c = chat;
    const key = chatKey(c.accountId, c.id);
    messages = null;
    api.call('messages.get', { accountId: c.accountId, chatId: c.id, limit: 20 })
      .then((r) => {
        if (key !== chatKey(chat.accountId, chat.id)) return;
        const items = (r.messages as Message[]).filter((m) => !m.isDeleted);
        const unread = c.isUnread && !store.eventsOnly(c) ? Math.min(store.chatUnread(c), UNREAD_SHOWN) : 0;
        messages = items.slice(-(unread || READ_SHOWN));
      })
      .catch(() => (messages = []));
  });

  // kept inside the window
  $effect(() => {
    void messages;
    const h = el?.offsetHeight ?? 0;
    y = Math.max(8, Math.min(top, window.innerHeight - h - 8));
  });

  function when(m: Message): string {
    return isSameDay(m.time, Date.now()) ? timeOfDay(m.time) : `${listTime(m.time)} ${timeOfDay(m.time)}`;
  }

  function plainOnly(m: Message): boolean {
    return !!m.system || !!m.file || !!m.poll;
  }
</script>

<div class="chat-preview" bind:this={el} style:top="{y}px" style:left="{left}px" role="tooltip" aria-label="Preview of {store.chatName(chat)}">
  <div class="head">
    <span class="name">{store.chatName(chat)}</span>
    {#if chat.isUnread && !store.eventsOnly(chat)}<span class="unread">{store.chatUnread(chat)} unread</span>{/if}
  </div>
  {#if messages === null}
    <div class="muted"><LoaderCircle size={14} class="spin" /></div>
  {:else if !messages.length}
    <div class="muted">No messages yet.</div>
  {:else}
    {#each messages as m (m.id)}
      {#if m.system}
        <div class="system">{store.systemLine(m)}</div>
      {:else}
        <div class="msg">
          <div class="meta">
            {#if chat.isGroup || m.isOutgoing}<span class="sender">{store.senderName(m)}</span>{/if}
            <span class="time">{when(m)}</span>
          </div>
          {#if plainOnly(m)}
            <div class="text">{store.previewText(m)}</div>
          {:else}
            <div class="text">{@html renderText(m.text, m.mentions, store.accounts[m.accountId]?.selfId ?? '')}</div>
          {/if}
        </div>
      {/if}
    {/each}
  {/if}
  <div class="hint">Click to open · not marked as read</div>
</div>

<style>
  .chat-preview {
    position: fixed; z-index: 60; width: 340px; max-height: min(460px, calc(100vh - 16px)); overflow: hidden;
    display: flex; flex-direction: column; gap: 8px; padding: 12px 14px 10px;
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius-lg, 12px); box-shadow: var(--shadow-2, var(--shadow-1));
    pointer-events: none; animation: fade var(--duration-fast) var(--ease);
  }
  .head { display: flex; align-items: baseline; gap: 8px; }
  .name { font-weight: 650; font-size: 14px; flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .unread { font-size: 12px; color: var(--accent); font-weight: 600; }
  .msg { display: flex; flex-direction: column; gap: 2px; }
  .meta { display: flex; gap: 6px; font-size: 12px; color: var(--text-muted); }
  .sender { font-weight: 600; color: var(--text-2); }
  .text {
    font-size: 13.5px; line-height: 1.4; color: var(--text); overflow-wrap: anywhere;
    display: -webkit-box; -webkit-line-clamp: 8; line-clamp: 8; -webkit-box-orient: vertical; overflow: hidden;
  }
  .text :global(pre) { margin: 2px 0; font-size: 12px; white-space: pre-wrap; }
  .system { font-size: 12px; color: var(--text-muted); text-align: center; }
  .muted { font-size: 13px; color: var(--text-muted); display: flex; }
  .hint { font-size: 11px; color: var(--text-muted); border-top: 1px solid var(--border); padding-top: 6px; }
</style>
