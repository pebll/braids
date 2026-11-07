<!--
  Conversation.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Header, message history (paged back on scroll, day separators, "new
  messages" marker, grouping), typing indicator, in-chat search, composer.
  Only a window of the loaded messages is rendered (the newest WINDOW, more
  revealed while scrolling up), so long chats stay fast.
-->
<script lang="ts">
  import { ArrowDown, ChevronLeft, ChevronUp, EllipsisVertical, Lock, PanelRight, Pin, Search, Upload, X } from '@lucide/svelte';
  import { onDestroy, tick, untrack } from 'svelte';
  import { chatMenu } from '../lib/actions';
  import * as api from '../lib/api';
  import { dayLabel, isSameDay, lastSeen, startOfDay } from '../lib/format';
  import { hint } from '../lib/keybinds.svelte';
  import { settings } from '../lib/settings.svelte';
  import { splitKey, store } from '../lib/store.svelte';
  import type { Message } from '../lib/types';
  import { ui } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';
  import Composer from './Composer.svelte';
  import MessageItem from './MessageItem.svelte';

  let { chatKey }: { chatKey: string } = $props();

  const GROUP_GAP = 5 * 60 * 1000;
  const WINDOW = 150; // messages rendered when at the bottom
  const REVEAL = 60; // more rendered per step when scrolling up
  const [accountId, chatId] = $derived(splitKey(chatKey));
  const chat = $derived(store.chats[chatKey]);
  const account = $derived(store.accounts[accountId]);
  const list = $derived(store.messages[chatKey]);
  const items = $derived(list?.items ?? []);
  // first rendered message, by id so that it stays put when messages are
  // added or removed ('' or unknown: the start of the window at the bottom)
  let startId = $state('');
  const startIdx = $derived.by(() => {
    const i = startId ? items.findIndex((m) => m.id === startId) : -1;
    return i >= 0 ? i : Math.max(0, items.length - WINDOW);
  });
  // index ranges [from, to) of rendered items per day
  const days = $derived.by(() => {
    const out: { key: number; from: number; to: number }[] = [];
    for (let i = startIdx; i < items.length; i++) {
      const m = items[i];
      if (i === startIdx || !isSameDay(items[i - 1].time, m.time)) out.push({ key: startOfDay(m.time), from: i, to: i + 1 });
      else out[out.length - 1].to = i + 1;
    }
    return out;
  });
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
  let reactFor = $state<{ m: Message; x: number; y: number; above: boolean; emojis: string[] } | null>(null);
  let lastKey = '';
  let lastCount = 0;
  let lastNewestId = '';
  let openedAt = ''; // the chat just opened, until it is scrolled to its unread messages

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

  // scroll: to the latest message when a chat opens, stick to the bottom for
  // new messages while at the bottom
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
      startId = '';
      searchText = '';
      searchStatus = '';
      if (chat?.isGroup && !store.members[key]) {
        api.call('members.get', { accountId, chatId }).then((r) => { store.members[key] = r.members; }).catch(() => { store.members[key] = []; });
      }
    }
    if (opened) openedAt = key;
    tick().then(() => {
      if (!scroller) return;
      if (opened && list?.firstUnreadId) scrollToUnread();
      else if (opened || (appended && (wasAtBottom || items[count - 1]?.isOutgoing))) scrollToBottom();
      fillViewport();
    });
  });

  // an unread chat opens at the "New messages" line (once its messages are
  // there), a read one at the bottom
  $effect(() => {
    const first = list?.firstUnreadId;
    if (first && openedAt === chatKey) untrack(() => tick().then(scrollToUnread));
  });

  async function scrollToUnread() {
    const first = list?.firstUnreadId;
    if (!scroller || !first || openedAt !== chatKey) return;
    openedAt = '';
    const idx = items.findIndex((m) => m.id === first);
    if (idx >= 0 && idx < startIdx) {
      startId = items[Math.max(0, idx - 5)].id;
      await tick();
    }
    const marker = scroller.querySelector<HTMLElement>('[data-unread-marker]');
    if (!marker) return scrollToBottom();
    // the line near the top, with the message before it in sight
    scroller.scrollTop += marker.getBoundingClientRect().top - scroller.getBoundingClientRect().top - 64;
    atBottom = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 40;
  }

  function scrollToBottom() {
    if (scroller) scroller.scrollTop = scroller.scrollHeight;
    atBottom = true;
  }

  // renders older loaded messages above, keeping the view in place
  async function reveal(): Promise<boolean> {
    if (!scroller || startIdx === 0) return false;
    const fromBottom = scroller.scrollHeight - scroller.scrollTop;
    startId = items[Math.max(0, startIdx - REVEAL)].id;
    await tick();
    if (scroller) scroller.scrollTop = scroller.scrollHeight - fromBottom;
    return true;
  }

  // back at the bottom: render only the newest messages again
  function trimWindow() {
    if (atBottom && startId && items.length - startIdx > WINDOW) startId = '';
  }

  // older messages go on top; the view keeps its distance from the bottom
  // (stays at the bottom when it was there at the end: images may have
  // grown the content and moved the view while the page was loading)
  async function loadOlder() {
    if (!scroller || !list || list.loading || list.atStart) return;
    const key = chatKey;
    const fromBottom = scroller.scrollHeight - scroller.scrollTop;
    const n = await store.loadOlder(key);
    if (!n || !scroller || key !== chatKey) return;
    startId = items[0]?.id ?? ''; // render the new page
    await tick();
    if (atBottom) scrollToBottom();
    else scroller.scrollTop = scroller.scrollHeight - fromBottom;
  }

  // load more when the history does not fill the view yet
  async function fillViewport() {
    if (scroller && list?.loaded && !list.loading && scroller.scrollHeight <= scroller.clientHeight + 40) {
      if (!(await reveal())) {
        if (list.atStart) return;
        await loadOlder();
      }
      await tick();
      if (atBottom) scrollToBottom();
    }
  }

  function onScroll() {
    if (!scroller) return;
    atBottom = scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight < 60;
    // away from the bottom the window stays where it is (new messages do not
    // shift it)
    if (!atBottom && !startId && items.length) startId = items[startIdx].id;
    if (scroller.scrollTop < 300) {
      if (startIdx > 0) reveal();
      else loadOlder();
    }
    if (atBottom) {
      store.markCurrentRead();
      trimWindow();
    }
  }

  // stay at the bottom while content grows (images, reactions, edits) or
  // the view changes size (window shown again, composer grows)
  $effect(() => {
    if (!inner || !scroller) return;
    const observer = new ResizeObserver(() => {
      if (atBottom) scrollToBottom();
    });
    observer.observe(inner);
    observer.observe(scroller);
    return () => observer.disconnect();
  });

  // jump to a message, loading older pages until it is there
  async function jumpTo(msgId: string): Promise<boolean> {
    for (let i = 0; i < 25; i++) {
      // loaded but outside the rendered window: render from a bit before it
      const idx = items.findIndex((m) => m.id === msgId);
      if (idx >= 0 && idx < startIdx) {
        startId = items[Math.max(0, idx - 10)].id;
        await tick();
      }
      const el = scroller?.querySelector(`[data-msg-id="${CSS.escape(msgId)}"]`) as HTMLElement | null;
      if (el) {
        el.scrollIntoView({ block: 'center', behavior: 'smooth' });
        ui.highlight(msgId);
        return true;
      }
      if (!list || list.atStart) break;
      const n = await store.loadOlder(chatKey);
      if (items.length) startId = items[0].id;
      await tick();
      if (!n && list.atStart) break;
    }
    store.toast('The original message is not available.');
    return false;
  }

  // ---- pinned messages: a bar under the header; a click shows the pinned
  // message and moves on to the next older one ----
  let pinned = $state<Message[]>([]);
  let pinIndex = $state(0);
  const pin = $derived(pinned.length ? pinned[Math.min(pinIndex, pinned.length - 1)] : null);

  $effect(() => {
    const key = chatKey;
    untrack(() => {
      pinned = [];
      pinIndex = 0;
      api.call('media.list', { kind: 'pinned', accountId, chatId, limit: 50 })
        .then((r) => { if (key === chatKey) pinned = r.items as Message[]; })
        .catch(() => {});
    });
  });

  const pinListener = (m: Message) => {
    if (m.accountId !== accountId || m.chatId !== chatId) return;
    const i = pinned.findIndex((x) => x.id === m.id);
    if (m.isPinned && !m.isDeleted) {
      if (i >= 0) pinned[i] = m;
      else pinned = [...pinned, m].sort((a, b) => b.time - a.time);
    } else if (i >= 0) {
      pinned = pinned.filter((x) => x.id !== m.id);
    }
  };
  store.messageListeners.add(pinListener);
  onDestroy(() => store.messageListeners.delete(pinListener));

  async function showPin() {
    if (!pin) return;
    const id = pin.id;
    pinIndex = (pinIndex + 1) % pinned.length;
    await jumpTo(id);
  }

  function unpin(m: Message) {
    api.call('messages.pin', { accountId, chatId, msgId: m.id, isPinned: false }).catch((e) => store.toastError(e));
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

  // closing the search goes back to writing
  function closeSearch() {
    ui.chatSearchOpen = false;
    ui.focusComposer++;
  }

  // "Show in chat" from elsewhere (shared media)
  $effect(() => {
    const req = ui.jumpRequest;
    if (!req || req.chatKey !== chatKey || !list?.loaded) return;
    ui.jumpRequest = null;
    tick().then(() => jumpTo(req.msgId));
  });

  async function openReactions(m: Message, anchor: HTMLElement) {
    const r = anchor.getBoundingClientRect();
    const fallback = ['👍', '👎', '❤️', '🔥', '🎉', '😂', '😮', '😢', '😡', '🙏', '👏', '🤔', '💯', '👀', '✅', '🙌'];
    // below the button, or above it near the bottom of the window
    const above = r.bottom + 220 > window.innerHeight;
    reactFor = { m, x: r.left, y: above ? r.top - 6 : r.bottom + 6, above, emojis: fallback };
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

  function groupInfo(i: number): { first: boolean; last: boolean } {
    const m = items[i];
    const prev = items[i - 1];
    const next = items[i + 1];
    const day = !prev || !isSameDay(prev.time, m.time);
    const unreadHere = list?.firstUnreadId === m.id;
    const nextUnread = next && list?.firstUnreadId === next.id;
    const same = (a?: Message, b?: Message) => !!a && !!b && !a.system && !b.system && a.senderId === b.senderId && a.isOutgoing === b.isOutgoing && Math.abs(b.time - a.time) < GROUP_GAP && isSameDay(a.time, b.time);
    return { first: day || unreadHere || !same(prev, m), last: !!nextUnread || !same(m, next) };
  }
</script>

{#if chat && account}
  <main class="conv" aria-label="Conversation">
    <header>
      {#if ui.narrow}
        <button class="icon-btn" aria-label="Back to chats" onclick={() => store.closeChat()}><ChevronLeft size={22} /></button>
      {/if}
      <button class="who" onclick={() => (ui.infoOpen = !ui.infoOpen)} title="Chat info">
        <Avatar name={store.chatName(chat)} id={chat.id} size={38} {accountId} group={chat.isGroup} ring="var(--bg-2)" />
        <span class="who-text">
          <span class="who-name">{store.chatName(chat)}</span>
          <span class="who-sub">
            <span class="acct"><span class="acct-dot" style:background={store.accountColor(accountId)}></span>{store.accountName(accountId)}</span>
            {#if subtitle}<span>·</span><span class:typing={typing.length > 0}>{subtitle}</span>{/if}
          </span>
        </span>
      </button>
      <button class="icon-btn" class:active={ui.chatSearchOpen} aria-label="Search in chat" title={`Search in chat${hint('searchInChat')}`} onclick={() => (ui.chatSearchOpen ? closeSearch() : (ui.chatSearchOpen = true))}>
        <Search size={19} strokeWidth={1.75} />
      </button>
      <button class="icon-btn" class:active={ui.infoOpen} aria-label="Toggle info panel" title={`Chat info${hint('toggleInfo')}`} onclick={() => (ui.infoOpen = !ui.infoOpen)}>
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

    {#if pin}
      <div class="pin-bar">
        <button class="pin-main" onclick={showPin} title="Show the pinned message">
          <span class="pin-marks" aria-hidden="true">
            {#each pinned.slice(0, 4) as _, i}<span class:on={i === Math.min(pinIndex, pinned.length - 1) % 4}></span>{/each}
          </span>
          <Pin size={15} />
          <span class="pin-text">
            <b>{pinned.length > 1 ? `Pinned message ${Math.min(pinIndex, pinned.length - 1) + 1} of ${pinned.length}` : 'Pinned message'}</b>
            <span>{store.previewText(pin)}</span>
          </span>
        </button>
        {#if account.capabilities.pinMessage}
          <button class="icon-btn sm" aria-label="Unpin" title="Unpin" onclick={() => unpin(pin)}><X size={16} /></button>
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
          onkeydown={(e) => { if (e.key === 'Enter') findNext(); if (e.key === 'Escape') { e.preventDefault(); closeSearch(); } }}
        />
        {#if searchStatus}<span class="search-status">{searchStatus}</span>{/if}
        <button class="icon-btn sm" title="Find older (Enter)" aria-label="Find older" onclick={findNext}><ChevronUp size={17} /></button>
        <button class="icon-btn sm" aria-label="Close search" onclick={closeSearch}><X size={17} /></button>
      </div>
    {/if}

    <div class="messages scroll" bind:this={scroller} onscroll={onScroll}>
      <div class="inner" bind:this={inner}>
      {#if startIdx > 0}
        <div class="start"><span class="loading-dots"><span></span><span></span><span></span></span></div>
      {:else if list?.atStart}
        <div class="start">
          {#if chat.isGroup}<Lock size={13} />{/if}
          <span>Start of the conversation</span>
        </div>
      {:else if list?.loading}
        <div class="start"><span class="loading-dots"><span></span><span></span><span></span></span></div>
      {:else if list?.stalled}
        <div class="start"><button class="btn ghost" onclick={() => store.loadOlder(chatKey, true)}>Load older messages</button></div>
      {/if}

      {#each days as d (d.key)}
        <!-- one block per day, so its sticky label stays inside the day -->
        <div class="day-group">
          <div class="day"><span>{dayLabel(items[d.from].time)}</span></div>
          {#each items.slice(d.from, d.to) as m, j (m.id)}
            {@const g = groupInfo(d.from + j)}
            {#if list?.firstUnreadId === m.id}
              <div class="unread-marker" role="separator" data-unread-marker><span></span><b>New messages</b><span></span></div>
            {/if}
            {#if m.system}
              <!-- group events (joined, left, renamed): a line, not a bubble -->
              <div class="system-line" data-msg-id={m.id} title={new Date(m.time).toLocaleString()}>{store.systemLine(m)}</div>
            {:else}
              <MessageItem {m} {chat} {account} first={g.first} last={g.last} onJump={jumpTo} onReact={openReactions} />
            {/if}
          {/each}
        </div>
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
      <div class="react-pop" role="dialog" aria-label="Reactions"
        style:left="{Math.min(reactFor.x, ui.width - 300) / (api.isTauri ? 1 : settings.textSize / 100)}px"
        style:top="{reactFor.y / (api.isTauri ? 1 : settings.textSize / 100)}px" class:above={reactFor.above}>
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

  .pin-bar { display: flex; align-items: center; gap: 4px; padding: 4px 10px 4px 12px; border-bottom: 1px solid var(--border); background: var(--surface); }
  .pin-main { flex: 1; min-width: 0; display: flex; align-items: center; gap: 8px; border: 0; background: none; padding: 4px; border-radius: var(--radius-md); text-align: left; color: var(--accent); }
  .pin-main:hover { background: var(--hover); }
  .pin-marks { display: flex; flex-direction: column; gap: 2px; align-self: stretch; justify-content: center; }
  .pin-marks span { width: 2px; flex: 1; max-height: 10px; border-radius: 1px; background: var(--border-strong); }
  .pin-marks span.on { background: var(--accent); }
  .pin-text { flex: 1; min-width: 0; display: flex; flex-direction: column; line-height: 17px; }
  .pin-text b { font-size: 12px; font-weight: 600; color: var(--accent); }
  .pin-text span { font-size: 13px; color: var(--text); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .search-bar { display: flex; align-items: center; gap: 8px; padding: 6px 12px 6px 16px; border-bottom: 1px solid var(--border); color: var(--text-muted); }
  .search-bar input { flex: 1; border: 0; outline: none; background: transparent; font-size: 14px; color: var(--text); height: 30px; }
  .search-status { font-size: 12px; }

  .messages { flex: 1; min-height: 0; padding: 4px 16px 8px; }
  .inner { min-height: 100%; display: flex; flex-direction: column; justify-content: flex-end; }
  .start { display: flex; justify-content: center; align-items: center; gap: 6px; padding: 16px 0 8px; font-size: 12px; color: var(--text-muted); }
  .day-group { display: flex; flex-direction: column; }
  .day { display: flex; justify-content: center; padding: 10px 0 6px; position: sticky; top: 0; z-index: 2; pointer-events: none; }
  .day span { pointer-events: auto; }
  .day span { font-size: 12px; font-weight: 600; color: var(--text-muted); background: var(--bg-2); padding: 3px 10px; border-radius: 999px; border: 1px solid var(--border); }
  .system-line { text-align: center; font-size: 12px; color: var(--text-muted); padding: 6px 24px; overflow-wrap: anywhere; }
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
  .react-pop.above { transform: translateY(-100%); }
  .react-pop button { border: 0; background: none; height: 32px; border-radius: 6px; font-size: 18px; padding: 0; }
  .react-pop button:hover { background: var(--hover); }
  .react-pop button.mine { background: var(--accent-soft); }
</style>
