<!--
  ChatList.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  The unified inbox: chats of all accounts (or the selected folder/account),
  pinned first, then by activity. Search by name, context menu per chat.
-->
<script lang="ts">
  import { AtSign, Menu as MenuIcon, Pin, Search, SquarePen, VolumeX, X } from '@lucide/svelte';
  import { chatMenu } from '../lib/actions';
  import { listTime } from '../lib/format';
  import { chatKey, store } from '../lib/store.svelte';
  import type { Chat } from '../lib/types';
  import { ui } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';
  import Braid from './Braid.svelte';

  let searchInput: HTMLInputElement | undefined = $state();
  let listEl: HTMLDivElement | undefined = $state();
  let cursor = $state(-1); // keyboard cursor, distinct from selection

  const chats = $derived(store.chatList);

  const title = $derived.by(() => {
    const f = store.filter;
    if (f.kind === 'archived') return 'Archived';
    if (f.kind === 'favorites') return 'Favorites';
    if (f.kind === 'account') return store.accountName(f.accountId);
    return 'All chats';
  });

  $effect(() => {
    if (ui.focusSearch) searchInput?.focus();
  });

  function open(c: Chat) {
    store.openChat(chatKey(c.accountId, c.id));
    ui.focusComposer++;
  }

  function preview(c: Chat): { prefix: string; prefixKind: string; text: string } {
    const key = chatKey(c.accountId, c.id);
    const typing = store.typingNames(key);
    if (typing.length) return { prefix: '', prefixKind: 'typing', text: c.isGroup ? `${typing[0].split(' ')[0]} is typing…` : 'typing…' };
    const draft = store.drafts[key];
    if (draft && key !== store.current) return { prefix: 'Draft:', prefixKind: 'draft', text: draft };
    const m = c.lastMessage;
    if (!m) return { prefix: '', prefixKind: '', text: '' };
    const prefix = m.isOutgoing ? 'You:' : c.isGroup ? `${store.senderName(m).split(' ')[0]}:` : '';
    return { prefix, prefixKind: 'sender', text: store.previewText(m) };
  }

  function onKey(e: KeyboardEvent) {
    if (!chats.length) return;
    if (e.key === 'ArrowDown' || e.key === 'j') {
      cursor = Math.min(chats.length - 1, cursor + 1);
    } else if (e.key === 'ArrowUp' || e.key === 'k') {
      cursor = Math.max(0, cursor - 1);
    } else if (e.key === 'Enter' && cursor >= 0) {
      open(chats[cursor]);
      return;
    } else {
      return;
    }
    e.preventDefault();
    listEl?.querySelectorAll<HTMLElement>('[role=option]')[cursor]?.scrollIntoView({ block: 'nearest' });
  }

  function onSearchKey(e: KeyboardEvent) {
    if (e.key === 'Escape') {
      store.search = '';
      searchInput?.blur();
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      cursor = 0;
      listEl?.focus();
    } else if (e.key === 'Enter' && chats.length) {
      open(chats[0]);
    }
  }
</script>

<section class="chatlist" aria-label="Chat list">
  <div class="head">
    <div class="title-row">
      {#if ui.narrow}
        <button class="icon-btn" aria-label="Open sidebar" onclick={() => (ui.drawerOpen = true)}><MenuIcon size={20} strokeWidth={1.75} /></button>
      {/if}
      <h2>{title}</h2>
      <button class="icon-btn" aria-label="New chat" title="New chat" onclick={() => (ui.newChatOpen = true)} disabled={!store.accountList.length}>
        <SquarePen size={19} strokeWidth={1.75} />
      </button>
    </div>
    <label class="search">
      <Search size={16} strokeWidth={1.75} />
      <input
        bind:this={searchInput}
        bind:value={store.search}
        placeholder="Search chats"
        aria-label="Search chats"
        onkeydown={onSearchKey}
      />
      {#if store.search}
        <button class="icon-btn sm" aria-label="Clear search" onclick={() => (store.search = '')}><X size={15} /></button>
      {:else}
        <kbd>Ctrl K</kbd>
      {/if}
    </label>
  </div>

  <div
    class="list scroll"
    role="listbox"
    aria-label="Chats"
    tabindex="0"
    bind:this={listEl}
    onkeydown={onKey}
    onblur={() => (cursor = -1)}
  >
    {#each chats as c, i (chatKey(c.accountId, c.id))}
      {@const key = chatKey(c.accountId, c.id)}
      {@const p = preview(c)}
      {@const selected = key === store.current}
      <div
        class="row"
        class:selected
        class:cursor={i === cursor}
        class:unread={c.isUnread}
        class:favorite={store.category(c) === 'favorite'}
        title={store.category(c) === 'favorite' ? 'Favorite' : undefined}
        role="option"
        aria-selected={selected}
        tabindex="-1"
        onclick={() => open(c)}
        onkeydown={(e) => e.key === 'Enter' && open(c)}
        oncontextmenu={(e) => { e.preventDefault(); ui.openMenu(e, chatMenu(c)); }}
      >
        <Avatar name={store.chatName(c)} id={c.id} accountId={c.accountId} group={c.isGroup} ring={selected ? 'var(--selected)' : 'var(--bg-1)'}
          online={!c.isGroup && store.isOnline(c.accountId, c.id)} />
        <div class="body">
          <div class="line1">
            <span class="name">{store.chatName(c)}</span>
            {#if c.isMuted}<span class="flag" title="Muted"><VolumeX size={14} /></span>{/if}
            <span class="grow"></span>
            <span class="time">{listTime(c.lastActivity)}</span>
          </div>
          <div class="line2">
            <span class="preview">
              {#if p.prefixKind === 'typing'}
                <span class="typing">{p.text}</span>
              {:else}
                {#if p.prefix}<span class="prefix" class:draft={p.prefixKind === 'draft'}>{p.prefix}</span>{/if}
                {p.text}
              {/if}
            </span>
            {#if c.isPinned}<span class="flag" title="Pinned"><Pin size={14} /></span>{/if}
            {#if c.isUnreadMention}<span class="mention-dot" title="Mentioned"><AtSign size={12} strokeWidth={2.5} /></span>{/if}
            {#if c.isUnread}<span class="dot" class:muted={c.isMuted} title="Unread"></span>{/if}
          </div>
        </div>
      </div>
    {:else}
      <div class="empty">
        {#if !store.synced}
          {#each [0, 1, 2, 3, 4] as i}
            <div class="skeleton" style:opacity={1 - i * 0.18}><span class="sk-av"></span><span class="sk-lines"><span></span><span></span></span></div>
          {/each}
        {:else if store.search}
          <p>No chats match “{store.search}”.</p>
        {:else if store.filter.kind === 'archived'}
          <p>No archived chats.</p>
        {:else if store.filter.kind === 'favorites'}
          <p><b>No favorites yet.</b><br />Right-click a chat and choose “Add to favorites”.</p>
        {:else if store.accountList.length}
          <Braid width={200} height={28} faint />
          <p><b>No chats yet.</b><br />They appear here as the accounts sync.</p>
        {/if}
      </div>
    {/each}
  </div>
</section>

<style>
  .chatlist { width: 330px; flex-shrink: 0; background: var(--bg-1); border-right: 1px solid var(--border); display: flex; flex-direction: column; min-height: 0; }
  .head { padding: 12px 12px 8px; display: flex; flex-direction: column; gap: 10px; }
  .title-row { display: flex; align-items: center; gap: 6px; }
  h2 { margin: 0; font-size: 18px; font-weight: 650; flex: 1; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .search {
    display: flex; align-items: center; gap: 8px; height: 34px; padding: 0 4px 0 10px; border-radius: var(--radius-md);
    background: var(--surface-2); border: 1px solid var(--border); color: var(--text-muted);
  }
  .search:focus-within { border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); }
  .search input { flex: 1; min-width: 0; border: 0; outline: none; background: transparent; font-size: 14px; color: var(--text); }
  .search input::placeholder { color: var(--text-muted); }
  kbd { font-family: var(--mono); font-size: 10px; color: var(--text-muted); border: 1px solid var(--border); border-radius: 4px; padding: 0 4px; margin-right: 4px; }
  .list { flex: 1; padding: 0 6px 8px; display: flex; flex-direction: column; gap: 1px; outline: none; min-height: 0; }
  .row { display: flex; gap: 12px; align-items: center; padding: 9px 12px; border-radius: 10px; cursor: default; flex-shrink: 0; }
  .row:hover { background: var(--hover); }
  .row.selected { background: var(--selected); }
  .row.favorite { box-shadow: inset 0 0 0 1.5px var(--favorite); }
  .row.cursor { box-shadow: inset 0 0 0 2px var(--focus); }
  .body { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .line1, .line2 { display: flex; align-items: center; gap: 5px; }
  .name { font-size: 14px; font-weight: 500; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-width: 0; }
  .unread .name { font-weight: 650; }
  .grow { flex: 1; }
  .time { font-size: 12px; color: var(--text-muted); flex-shrink: 0; }
  .unread .time { color: var(--accent); }
  .preview { font-size: 13px; color: var(--text-muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; flex: 1; min-width: 0; }
  .unread .preview { color: var(--text); }
  .prefix { color: var(--text-2); font-weight: 500; }
  .unread .prefix { font-weight: 600; }
  .prefix.draft { color: var(--danger); font-weight: 600; }
  .typing { color: var(--accent); font-style: italic; }
  .flag { color: var(--text-muted); display: flex; flex-shrink: 0; }
  .dot.muted { background: var(--border-strong); }
  .mention-dot { width: 18px; height: 18px; border-radius: 50%; background: var(--accent); color: var(--on-accent); display: flex; align-items: center; justify-content: center; flex-shrink: 0; }
  .empty { padding: 24px 16px; color: var(--text-muted); font-size: 13px; text-align: center; display: flex; flex-direction: column; align-items: center; gap: 10px; }
  .empty p { margin: 0; }
  .empty b { color: var(--text-2); }
  .skeleton { display: flex; gap: 12px; align-items: center; width: 100%; padding: 9px 6px; }
  .sk-av { width: 44px; height: 44px; border-radius: 50%; background: var(--skeleton); flex-shrink: 0; }
  .sk-lines { flex: 1; display: flex; flex-direction: column; gap: 8px; }
  .sk-lines span { height: 10px; border-radius: 5px; background: var(--skeleton); }
  .sk-lines span:first-child { width: 60%; }
</style>
