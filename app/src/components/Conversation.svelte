<!--
  Conversation.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Header, message history (paged back on scroll, day separators, "new
  messages" marker, grouping), typing indicator, in-chat search, composer.
-->
<script lang="ts">
  import { ArrowDown, ChevronLeft, ChevronUp, EllipsisVertical, Lock, PanelRight, Search, Upload, X } from '@lucide/svelte';
  import { tick } from 'svelte';
  import { chatMenu } from '../lib/actions';
  import * as api from '../lib/api';
  import { dayLabel, isSameDay, lastSeen } from '../lib/format';
  import { splitKey, store } from '../lib/store.svelte';
  import type { Message } from '../lib/types';
  import { ui } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';
  import Composer from './Composer.svelte';
  import MessageItem from './MessageItem.svelte';

  let { chatKey }: { chatKey: string } = $props();

  const GROUP_GAP = 5 * 60 * 1000;
  const [accountId, chatId] = $derived(splitKey(chatKey));
  const chat = $derived(store.chats[chatKey]);
  const account = $derived(store.accounts[accountId]);
  const list = $derived(store.messages[chatKey]);
  const items = $derived(list?.items ?? []);
  const typing = $derived(store.typingNames(chatKey));
  const members = $derived(store.members[chatKey]);

  let scroller: HTMLDivElement | undefined = $state();
  let inner: HTMLDivElement | undefined = $state();
  let composer: Composer | undefined = $state();
  let atBottom = $state(true);
  let searchText = $state('');
  let searchFrom = $state('');
  let searchStatus = $state('');
  let searchInput: HTMLInputElement | undefined = $state();
  let reactFor = $state<{ m: Message; x: number; y: number; emojis: string[] } | null>(null);
  let lastKey = '';
  let lastCount = 0;
  let lastNewestId = '';

  const subtitle = $derived.by(() => {
    if (!chat) return '';
    if (typing.length) return chat.isGroup ? `${typing.join(', ')} typing…` : 'typing…';
    if (chat.isGroup) {
      if (!members) return 'Group';
      const online = members.filter((c) => !c.isSelf && store.isOnline(accountId, c.id)).length;
      return `${members.length} members${online ? `, ${online} online` : ''}`;
    }
    const p = store.presence[`${accountId}\u0001${chatId}`];
    if (p?.isOnline) return 'online';
    return p ? lastSeen(p.timeSeen) : '';
  });

  // scroll: to the bottom (or the first unread) when a chat opens, stick to
  // the bottom for new messages while at the bottom
  $effect.pre(() => {
    const key = chatKey;
    const count = items.length;
    const newest = items[count - 1]?.id ?? '';
    const opened = key !== lastKey;
    const appended = !opened && newest !== lastNewestId && count >= lastCount;
    const wasAtBottom = atBottom;
    lastKey = key;
    lastCount = count;
    lastNewestId = newest;
    if (opened) {
      searchText = '';
      searchStatus = '';
      if (chat?.isGroup && !store.members[key]) {
        api.call('members.get', { accountId, chatId }).then((r) => { store.members[key] = r.members; }).catch(() => { store.members[key] = []; });
      }
    }
    tick().then(() => {
      if (!scroller) return;
      if (opened || (appended && (wasAtBottom || items[count - 1]?.isOutgoing))) {
        const unread = opened && list?.firstUnreadId ? scroller.querySelector('[data-unread-marker]') : null;
        if (unread) (unread as HTMLElement).scrollIntoView({ block: 'center' });
        else scrollToBottom();
      }
      fillViewport();
    });
  });

  function scrollToBottom() {
    if (scroller) scroller.scrollTop = scroller.scrollHeight;
    atBottom = true;
  }

  async function loadOlder() {
    if (!scroller || !list || list.loading || list.atStart) return;
    const prevHeight = scroller.scrollHeight;
    const prevTop = scroller.scrollTop;
    const n = await store.loadOlder(chatKey);
    if (!n || !scroller) return;
    await tick();
    scroller.scrollTop = prevTop + (scroller.scrollHeight - prevHeight);
  }

  // load more when the history does not fill the view yet
  async function fillViewport() {
    if (scroller && list?.loaded && !list.atStart && !list.loading && scroller.scrollHeight <= scroller.clientHeight + 40) {
      await loadOlder();
      await tick();
      if (atBottom) scrollToBottom();
    }
  }

  function onScroll() {
    if (!scroller) return;
    atBottom = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 60;
    if (scroller.scrollTop < 300) loadOlder();
    if (atBottom) store.markCurrentRead();
  }

  // stay at the bottom while content grows (images, reactions, edits)
  $effect(() => {
    if (!inner) return;
    const observer = new ResizeObserver(() => {
      if (atBottom) scrollToBottom();
    });
    observer.observe(inner);
    return () => observer.disconnect();
  });

  // jump to a message, loading older pages until it is there
  async function jumpTo(msgId: string): Promise<boolean> {
    for (let i = 0; i < 25; i++) {
      const el = scroller?.querySelector(`[data-msg-id="${CSS.escape(msgId)}"]`) as HTMLElement | null;
      if (el) {
        el.scrollIntoView({ block: 'center', behavior: 'smooth' });
        ui.highlight(msgId);
        return true;
      }
      if (!list || list.atStart) break;
      const n = await store.loadOlder(chatKey);
      await tick();
      if (!n && list.atStart) break;
    }
    store.toast('The original message is not available.');
    return false;
  }

  async function findNext() {
    const text = searchText.trim();
    if (!text) return;
    searchStatus = 'Searching…';
    try {
      const r = await api.call('messages.find', { accountId, chatId, text, ...(searchFrom ? { fromMsgId: searchFrom } : {}) });
      if (r.found) {
        searchFrom = r.msgId;
        searchStatus = '';
        await jumpTo(r.msgId);
      } else {
        searchStatus = searchFrom ? 'No more results' : 'No results';
        searchFrom = '';
      }
    } catch (e) {
      searchStatus = '';
      store.toastError(e);
    }
  }

  $effect(() => {
    if (ui.chatSearchOpen) tick().then(() => searchInput?.focus());
  });

  async function openReactions(m: Message, anchor: HTMLElement) {
    const r = anchor.getBoundingClientRect();
    const fallback = ['👍', '👎', '❤️', '🔥', '🎉', '😂', '😮', '😢', '😡', '🙏', '👏', '🤔', '💯', '👀', '✅', '🙌'];
    reactFor = { m, x: r.left, y: r.bottom + 6, emojis: fallback };
    try {
      const res = await api.call('reactions.available', { accountId, chatId, msgId: m.id });
      if (reactFor?.m.id === m.id && res.emojis?.length) reactFor.emojis = res.emojis;
    } catch {
      // keep the default set
    }
  }

  function sendReaction(emoji: string) {
    const m = reactFor?.m;
    reactFor = null;
    if (!m) return;
    api.call('reactions.send', { accountId, chatId, msgId: m.id, emoji: m.reactions.own === emoji ? '' : emoji }).catch((e) => store.toastError(e));
  }

  export function addFiles(paths: string[]) {
    composer?.addFiles(paths);
  }

  function groupInfo(i: number): { first: boolean; last: boolean; day: boolean } {
    const m = items[i];
    const prev = items[i - 1];
    const next = items[i + 1];
    const day = !prev || !isSameDay(prev.time, m.time);
    const unreadHere = list?.firstUnreadId === m.id;
    const nextUnread = next && list?.firstUnreadId === next.id;
    const same = (a?: Message, b?: Message) => !!a && !!b && a.senderId === b.senderId && a.isOutgoing === b.isOutgoing && Math.abs(b.time - a.time) < GROUP_GAP && isSameDay(a.time, b.time);
    return { first: day || unreadHere || !same(prev, m), last: !!nextUnread || !same(m, next), day };
  }
</script>

{#if chat && account}
  <main class="conv" aria-label="Conversation">
    <header>
      {#if ui.narrow}
        <button class="icon-btn" aria-label="Back to chats" onclick={() => store.closeChat()}><ChevronLeft size={22} /></button>
      {/if}
      <button class="who" onclick={() => (ui.infoOpen = !ui.infoOpen)} title="Chat info">
        <Avatar name={store.chatName(chat)} id={chat.id} size={38} {accountId} ring="var(--bg-2)" />
        <span class="who-text">
          <span class="who-name">{store.chatName(chat)}</span>
          <span class="who-sub">
            <span class="acct"><span class="acct-dot" style:background={store.accountColor(accountId)}></span>{store.accountName(accountId)}</span>
            {#if subtitle}<span>·</span><span class:typing={typing.length > 0}>{subtitle}</span>{/if}
          </span>
        </span>
      </button>
      <button class="icon-btn" class:active={ui.chatSearchOpen} aria-label="Search in chat" title="Search in chat (Ctrl+F)" onclick={() => (ui.chatSearchOpen = !ui.chatSearchOpen)}>
        <Search size={19} strokeWidth={1.75} />
      </button>
      <button class="icon-btn" class:active={ui.infoOpen} aria-label="Toggle info panel" title="Chat info (Ctrl+I)" onclick={() => (ui.infoOpen = !ui.infoOpen)}>
        <PanelRight size={19} strokeWidth={1.75} />
      </button>
      <button class="icon-btn" aria-label="More" title="More" onclick={(e) => { const r = (e.currentTarget as HTMLElement).getBoundingClientRect(); ui.openMenu({ x: r.right - 210, y: r.bottom + 4 }, chatMenu(chat)); }}>
        <EllipsisVertical size={19} strokeWidth={1.75} />
      </button>
    </header>

    {#if account.state === 'error' || account.state === 'offline'}
      <div class="account-banner" class:error={account.state === 'error'}>
        {account.state === 'error' ? account.error || 'This account has a problem.' : `${store.accountName(accountId)} is offline — messages are sent when it reconnects.`}
        {#if account.state === 'error'}
          <button class="link-btn" onclick={() => { ui.settingsSection = 'accounts'; ui.settingsAccount = accountId; ui.settingsOpen = true; }}>Account settings</button>
        {/if}
      </div>
    {/if}

    {#if ui.chatSearchOpen}
      <div class="search-bar">
        <Search size={16} />
        <input
          bind:this={searchInput}
          bind:value={searchText}
          placeholder="Search in this chat"
          aria-label="Search in this chat"
          oninput={() => { searchFrom = ''; searchStatus = ''; }}
          onkeydown={(e) => { if (e.key === 'Enter') findNext(); if (e.key === 'Escape') ui.chatSearchOpen = false; }}
        />
        {#if searchStatus}<span class="search-status">{searchStatus}</span>{/if}
        <button class="icon-btn sm" title="Find older (Enter)" aria-label="Find older" onclick={findNext}><ChevronUp size={17} /></button>
        <button class="icon-btn sm" aria-label="Close search" onclick={() => (ui.chatSearchOpen = false)}><X size={17} /></button>
      </div>
    {/if}

    <div class="messages scroll" bind:this={scroller} onscroll={onScroll}>
      <div class="inner" bind:this={inner}>
      {#if list?.atStart}
        <div class="start">
          {#if chat.isGroup}<Lock size={13} />{/if}
          <span>Start of the conversation</span>
        </div>
      {:else if list?.loading}
        <div class="start"><span class="loading-dots"><span></span><span></span><span></span></span></div>
      {/if}

      {#each items as m, i (m.id)}
        {@const g = groupInfo(i)}
        {#if g.day}
          <div class="day"><span>{dayLabel(m.time)}</span></div>
        {/if}
        {#if list?.firstUnreadId === m.id}
          <div class="unread-marker" role="separator" data-unread-marker><span></span><b>New messages</b><span></span></div>
        {/if}
        <MessageItem {m} {chat} {account} first={g.first} last={g.last} onJump={jumpTo} onReact={openReactions} />
      {/each}

      {#if typing.length}
        <div class="typing-line">
          <span class="typing-dots"><span></span><span></span><span></span></span>
          {chat.isGroup ? `${typing.join(', ')} ${typing.length > 1 ? 'are' : 'is'} typing…` : `${store.chatName(chat)} is typing…`}
        </div>
      {/if}
      </div>
    </div>

    {#if !atBottom}
      <button class="to-bottom" aria-label="Scroll to latest" onclick={scrollToBottom}><ArrowDown size={18} /></button>
    {/if}

    <Composer bind:this={composer} {chatKey} />

    {#if ui.dropHover}
      <div class="drop">
        <Upload size={28} />
        <b>Drop to attach</b>
        <span>to {store.chatName(chat)}</span>
      </div>
    {/if}

    {#if reactFor}
      <div class="react-scrim" role="presentation" onclick={() => (reactFor = null)}></div>
      <div class="react-pop" role="dialog" aria-label="Reactions" style:left="{Math.min(reactFor.x, ui.width - 300)}px" style:top="{reactFor.y}px">
        {#each reactFor.emojis as e}
          <button class:mine={reactFor.m.reactions.own === e} onclick={() => sendReaction(e)}>{e}</button>
        {/each}
      </div>
    {/if}
  </main>
{/if}

<style>
  .conv { flex: 1; min-width: 0; display: flex; flex-direction: column; background: var(--bg-2); position: relative; }
  header { height: 60px; flex-shrink: 0; display: flex; align-items: center; gap: 4px; padding: 0 12px 0 12px; border-bottom: 1px solid var(--border); }
  .who { flex: 1; min-width: 0; display: flex; align-items: center; gap: 10px; border: 0; background: none; padding: 4px; text-align: left; border-radius: var(--radius-md); }
  .who-text { flex: 1; min-width: 0; display: flex; flex-direction: column; }
  .who-name { font-size: 15px; font-weight: 650; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .who-sub { font-size: 12px; color: var(--text-muted); display: flex; gap: 6px; align-items: center; white-space: nowrap; overflow: hidden; line-height: 16px; }
  .acct { display: inline-flex; align-items: center; gap: 4px; color: var(--text-2); font-weight: 600; }
  .acct-dot { width: 7px; height: 7px; border-radius: 50%; }
  .typing { color: var(--accent); font-style: italic; }

  .account-banner { padding: 8px 16px; font-size: 13px; background: var(--warning-soft); color: var(--text); display: flex; gap: 10px; align-items: center; }
  .account-banner.error { background: var(--danger-soft); }

  .search-bar { display: flex; align-items: center; gap: 8px; padding: 6px 12px 6px 16px; border-bottom: 1px solid var(--border); color: var(--text-muted); }
  .search-bar input { flex: 1; border: 0; outline: none; background: transparent; font-size: 14px; color: var(--text); height: 30px; }
  .search-status { font-size: 12px; }

  .messages { flex: 1; min-height: 0; padding: 4px 16px 8px; }
  .inner { min-height: 100%; display: flex; flex-direction: column; justify-content: flex-end; }
  .start { display: flex; justify-content: center; align-items: center; gap: 6px; padding: 16px 0 8px; font-size: 12px; color: var(--text-muted); }
  .day { display: flex; justify-content: center; padding: 10px 0 6px; position: sticky; top: 0; z-index: 2; }
  .day span { font-size: 12px; font-weight: 600; color: var(--text-muted); background: var(--bg-2); padding: 3px 10px; border-radius: 999px; border: 1px solid var(--border); }
  .unread-marker { display: flex; align-items: center; gap: 10px; padding: 8px 0; }
  .unread-marker span { flex: 1; height: 1px; background: var(--accent); opacity: 0.6; }
  .unread-marker b { font-size: 12px; font-weight: 700; color: var(--accent); }

  .typing-line { display: flex; align-items: center; gap: 8px; padding: 8px 4px 4px 4px; font-size: 12px; color: var(--text-muted); }
  .typing-dots, .loading-dots { display: flex; gap: 3px; padding: 8px 10px; background: var(--other-bubble); border-radius: 12px; }
  .typing-dots span, .loading-dots span { width: 6px; height: 6px; border-radius: 50%; background: var(--text-muted); animation: blink 1.2s infinite; }
  .typing-dots span:nth-child(2), .loading-dots span:nth-child(2) { animation-delay: 0.2s; }
  .typing-dots span:nth-child(3), .loading-dots span:nth-child(3) { animation-delay: 0.4s; }
  @keyframes blink { 0%, 100% { opacity: 0.3; } 40% { opacity: 1; } }

  .to-bottom {
    position: absolute; right: 20px; bottom: 84px; width: 38px; height: 38px; border-radius: 50%; border: 1px solid var(--border);
    background: var(--surface); color: var(--text-2); box-shadow: var(--shadow-2); display: flex; align-items: center; justify-content: center; z-index: 6;
  }

  .drop {
    position: absolute; inset: 8px; border: 2px dashed var(--accent); border-radius: var(--radius-xl); background: var(--bg-2);
    display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 6px; color: var(--accent); z-index: 20; opacity: 0.96;
  }
  .drop span { color: var(--text-2); font-size: 13px; }

  .react-scrim { position: fixed; inset: 0; z-index: 30; }
  .react-pop {
    position: fixed; z-index: 31; width: 288px; max-height: 240px; overflow-y: auto; padding: 8px; display: grid; grid-template-columns: repeat(8, 1fr); gap: 2px;
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius-lg); box-shadow: var(--shadow-2);
  }
  .react-pop button { border: 0; background: none; height: 32px; border-radius: 6px; font-size: 18px; padding: 0; }
  .react-pop button:hover { background: var(--hover); }
  .react-pop button.mine { background: var(--accent-soft); }
</style>
