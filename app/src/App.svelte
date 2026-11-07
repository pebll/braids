<!--
  App.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Layout (doc/design): sidebar 232 (60 collapsed) | chat list 330 |
  conversation | info panel 300 (column >= 1280 px, overlay below).
  Below 900 px a single pane with back button and the sidebar as a drawer.
-->
<script lang="ts">
  import { CircleAlert, LoaderCircle, MessagesSquare } from '@lucide/svelte';
  import { onMount, tick } from 'svelte';
  import AccountPrompt from './components/AccountPrompt.svelte';
  import Braid from './components/Braid.svelte';
  import ChatList from './components/ChatList.svelte';
  import Conversation from './components/Conversation.svelte';
  import ForwardDialog from './components/ForwardDialog.svelte';
  import MediaDialog from './components/MediaDialog.svelte';
  import InfoPanel from './components/InfoPanel.svelte';
  import NewChat from './components/NewChat.svelte';
  import Overlays from './components/Overlays.svelte';
  import Settings from './components/Settings.svelte';
  import PollDialog from './components/PollDialog.svelte';
  import ShortcutsDialog from './components/ShortcutsDialog.svelte';
  import Sidebar from './components/Sidebar.svelte';
  import Welcome from './components/Welcome.svelte';
  import Wizard from './components/Wizard.svelte';
  import { copyShortcut, editLastOwnMessage } from './lib/actions';
  import * as api from './lib/api';
  import { ACCOUNT_ACTIONS, actionFor, type Action } from './lib/keybinds.svelte';
  import { settings } from './lib/settings.svelte';
  import { store } from './lib/store.svelte';
  import { ui } from './lib/ui.svelte';

  let conversation: Conversation | undefined = $state();
  let everConnected = $state(false);

  // the wizard stays over the main layout while a new account syncs
  const welcome = $derived(store.synced && store.accountList.length === 0 && store.setups[store.wizardSetupId]?.done !== 'added');
  const prompt = $derived(Object.values(store.accountPrompts)[0]);

  $effect(() => {
    if (store.connected) everConnected = true;
  });

  $effect(() => {
    api.setBadge(store.badgeCount);
  });

  $effect(() => {
    const scale = settings.textSize / 100;
    const app = document.getElementById('app');
    if (api.isTauri) {
      app?.style.removeProperty('zoom');
      api.setWebviewZoom(scale).catch((e) => store.toastError(e));
    } else {
      app?.style.setProperty('zoom', String(scale));
    }
  });

  onMount(() => {
    let frame = 0;
    const syncWidth = () => {
      frame = 0;
      const width = window.visualViewport?.width ?? window.innerWidth;
      if (ui.width !== width) ui.width = width;
    };
    const onResize = () => {
      if (!frame) frame = requestAnimationFrame(syncWidth);
    };
    syncWidth();
    window.addEventListener('resize', onResize);
    window.visualViewport?.addEventListener('resize', onResize);
    const observer = new ResizeObserver(onResize);
    observer.observe(document.documentElement);
    api.setCloseToTray(settings.closeToTray);
    if (settings.checkUpdates) setTimeout(() => store.checkUpdates(), 20000);
    api.onFileDrop(
      (paths) => {
        if (store.current && paths.length) conversation?.addFiles(paths);
      },
      (hover) => (ui.dropHover = hover && !!store.current),
    );
    return () => {
      window.removeEventListener('resize', onResize);
      window.visualViewport?.removeEventListener('resize', onResize);
      observer.disconnect();
      if (frame) cancelAnimationFrame(frame);
    };
  });

  // chat list width
  const MIN_LIST = 240;
  const MAX_LIST = 560;
  function setListWidth(w: number) {
    settings.chatListWidth = Math.round(Math.min(MAX_LIST, Math.max(MIN_LIST, w)));
    settings.save();
  }

  function startResize(e: PointerEvent) {
    const el = e.currentTarget as HTMLElement;
    el.setPointerCapture(e.pointerId);
    const startX = e.clientX;
    const startW = settings.chatListWidth;
    document.body.classList.add('resizing');
    const move = (ev: PointerEvent) => {
      settings.chatListWidth = Math.round(Math.min(MAX_LIST, Math.max(MIN_LIST, startW + ev.clientX - startX)));
    };
    const up = () => {
      el.removeEventListener('pointermove', move);
      el.removeEventListener('pointerup', up);
      document.body.classList.remove('resizing');
      settings.save();
    };
    el.addEventListener('pointermove', move);
    el.addEventListener('pointerup', up);
  }

  // the chat list with the keyboard: shows it (narrow layout: instead of the
  // chat), then moves the highlight or focuses it
  async function toList(then: () => void) {
    if (ui.narrow && store.current) {
      ui.listCursor = store.current;
      store.closeChat();
      await tick();
    }
    then();
  }

  // next unread chat after the current one in list order (wrapping); none
  // left: no chat open (the "pick a chat" screen)
  function nextUnread() {
    const list = store.chatList;
    const keys = list.map((c) => `${c.accountId}\u0001${c.id}`);
    const i = keys.indexOf(store.current);
    for (let n = 1; n <= list.length; n++) {
      const j = (i + n) % list.length;
      if (list[j].isUnread && keys[j] !== store.current) {
        ui.listCursor = '';
        store.openChat(keys[j]);
        ui.focusComposer++;
        return;
      }
    }
    store.closeChat();
    goTo();
  }

  // the account of a protocol; with several, the next one after the shown one
  function goAccount(protocol: string): boolean {
    const accounts = store.accountList.filter((a) => a.protocol === protocol);
    if (!accounts.length) return false;
    const f = store.filter;
    const i = f.kind === 'account' ? accounts.findIndex((a) => a.accountId === f.accountId) : -1;
    store.filter = { kind: 'account', accountId: accounts[(i + 1) % accounts.length].accountId };
    return true;
  }

  // after a "go to": the whole folder (no search filter), the first chat
  // lightly highlighted
  function goTo() {
    store.search = '';
    ui.listCursor = '';
    toList(() => ui.list?.first());
  }

  function run(action: Action): boolean {
    switch (action) {
      case 'nextChat':
      case 'prevChat':
        toList(() => ui.list?.move(action === 'nextChat' ? 1 : -1));
        return true;
      case 'focusList':
        store.search = '';
        ui.listCursor = '';
        toList(() => ui.list?.focus());
        return true;
      case 'searchChats':
        if (ui.narrow) store.closeChat();
        ui.focusSearch++;
        return true;
      case 'searchInChat':
        if (!store.current) return false;
        ui.chatSearchOpen = true;
        return true;
      case 'nextUnread':
        // through all chats, not only the ones matching a search
        store.search = '';
        nextUnread();
        return true;
      case 'editLast':
        if (!store.current) return false;
        if (editLastOwnMessage()) ui.focusComposer++;
        return true;
      case 'toggleInfo':
        if (!store.current) return false;
        ui.infoOpen = !ui.infoOpen;
        return true;
      case 'textSizeIncrease':
        setTextSize(1);
        return true;
      case 'textSizeDecrease':
        setTextSize(-1);
        return true;
      case 'textSizeReset':
        setTextSize(0, 100);
        return true;
      case 'goAll':
        store.filter = { kind: 'all' };
        goTo();
        return true;
      case 'goUnread':
        store.filter = { kind: 'unread' };
        goTo();
        return true;
      case 'goFavorites':
        if (settings.favorites === 'top') return false;
        store.filter = { kind: 'favorites' };
        goTo();
        return true;
      case 'goMatrix':
      case 'goSignal':
      case 'goTelegram':
      case 'goWhatsApp':
        if (!goAccount(ACCOUNT_ACTIONS[action]!)) return false;
        goTo();
        return true;
      case 'settings':
        ui.settingsSection = 'general';
        ui.settingsOpen = true;
        return true;
      case 'shortcuts':
        ui.shortcuts = true;
        return true;
    }
  }

  const TEXT_SIZES: number[] = [70, 80, 90, 100, 110, 125, 150];
  function setTextSize(delta: number, exact?: number) {
    const current = TEXT_SIZES.indexOf(settings.textSize);
    const next = exact ?? TEXT_SIZES[Math.max(0, Math.min(TEXT_SIZES.length - 1, current < 0 ? 3 + delta : current + delta))];
    if (next !== settings.textSize) {
      settings.textSize = next;
      settings.save();
    }
    ui.showTextSize(settings.textSize);
  }

  function onKey(e: KeyboardEvent) {
    if (copyShortcut(e)) {
      e.preventDefault();
      return;
    }
    const action = actionFor(e);
    if (action === 'textSizeIncrease' || action === 'textSizeDecrease' || action === 'textSizeReset') {
      if (run(action)) e.preventDefault();
      return;
    }
    const modal = ui.settingsOpen || ui.newChatOpen || !!ui.forward || !!ui.pollFor || !!ui.media || ui.shortcuts || !!store.wizardSetupId || !!ui.confirmRequest || !!ui.lightbox || ui.recordingKeys;
    if (modal) {
      // no browser actions (print, new window, ...) behind a dialog either
      if (!ui.recordingKeys && actionFor(e)) e.preventDefault();
      return;
    }
    if (action && run(action)) {
      e.preventDefault();
    } else if (e.key === 'Escape' && ui.infoOpen && !ui.wide) {
      ui.infoOpen = false;
    }
  }
</script>

<svelte:window onkeydown={onKey} />

{#if store.fatal}
  <div class="fatal">
    <CircleAlert size={28} />
    <h1>braids cannot start</h1>
    <p>{store.fatal}</p>
  </div>
{:else if welcome}
  <div class="frame">
    {#if everConnected && !store.connected}<div class="conn">Reconnecting to braids core…</div>{/if}
    <Welcome />
  </div>
{:else}
  <div class="frame">
    {#if everConnected && !store.connected}
      <div class="conn"><LoaderCircle size={14} class="spin" />Reconnecting to braids core…</div>
    {/if}
    <div class="main" style:--chatlist-w="{settings.chatListWidth}px">
      {#if !ui.narrow}
        <Sidebar />
      {/if}
      {#if !ui.narrow || !store.current}
        <ChatList />
      {/if}
      {#if !ui.narrow}
        <!-- drag to make the chat list wider or narrower; double-click resets
             (a focusable separator is the ARIA window splitter pattern) -->
        <!-- svelte-ignore a11y_no_noninteractive_tabindex, a11y_no_noninteractive_element_interactions -->
        <div class="splitter" role="separator" aria-orientation="vertical" aria-label="Resize the chat list" tabindex="0"
          aria-valuemin={MIN_LIST} aria-valuemax={MAX_LIST} aria-valuenow={settings.chatListWidth}
          onpointerdown={startResize} ondblclick={() => setListWidth(330)}
          onkeydown={(e) => { if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') { e.preventDefault(); setListWidth(settings.chatListWidth + (e.key === 'ArrowLeft' ? -20 : 20)); } }}></div>
      {/if}
      {#if store.current && store.currentChat}
        <Conversation bind:this={conversation} chatKey={store.current} />
        {#if ui.infoOpen}
          <InfoPanel chatKey={store.current} overlay={!ui.wide} />
        {/if}
      {:else if !ui.narrow}
        <div class="no-chat">
          {#if !store.synced}
            <LoaderCircle size={22} class="spin" />
          {:else}
            <MessagesSquare size={40} strokeWidth={1.25} />
            <p><b>Pick a chat</b><br />Messages from all your accounts land in one inbox.</p>
            <Braid width={260} height={32} animated={false} faint />
          {/if}
        </div>
      {/if}
    </div>
    {#if ui.narrow && ui.drawerOpen}
      <div class="drawer-scrim" role="presentation" onclick={() => (ui.drawerOpen = false)}></div>
      <div class="drawer"><Sidebar drawer /></div>
    {/if}
  </div>
{/if}

{#if store.wizardSetupId}<Wizard />{/if}
{#if ui.settingsOpen}<Settings />{/if}
{#if ui.newChatOpen}<NewChat />{/if}
{#if ui.forward}<ForwardDialog messages={ui.forward} />{/if}
{#if ui.shortcuts}<ShortcutsDialog />{/if}
{#if ui.pollFor}<PollDialog chatKey={ui.pollFor} />{/if}
{#if ui.media}<MediaDialog scope={ui.media.chatKey} />{/if}
{#if prompt && !store.wizardSetupId}<AccountPrompt {prompt} />{/if}
<Overlays />
{#if ui.textSizeNotice}
  <div class="text-size-notice" role="status" aria-live="polite">Text size: {ui.textSizeNotice} %</div>
{/if}

<style>
  .frame { height: 100%; display: flex; flex-direction: column; }
  .main { flex: 1; min-height: 0; display: flex; position: relative; }
  /* a thin hit area over the chat list's border */
  .splitter { width: 7px; margin: 0 -4px 0 -3px; flex-shrink: 0; cursor: col-resize; z-index: 5; position: relative; outline: none; }
  .splitter::after { content: ''; position: absolute; top: 0; bottom: 0; left: 3px; width: 1px; background: transparent; transition: background var(--duration-fast); }
  .splitter:hover::after, .splitter:focus-visible::after, :global(body.resizing) .splitter::after { background: var(--accent); width: 2px; }
  :global(body.resizing) { cursor: col-resize; user-select: none; }
  .conn { display: flex; align-items: center; justify-content: center; gap: 8px; padding: 6px; font-size: 12px; background: var(--warning-soft); color: var(--text); }
  .no-chat { flex: 1; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 12px; color: var(--text-muted); background: var(--bg-2); text-align: center; }
  .no-chat p { margin: 0; font-size: 13px; }
  .no-chat b { color: var(--text-2); font-size: 15px; }
  .drawer-scrim { position: fixed; inset: 0; background: var(--overlay); z-index: 45; }
  .drawer { position: fixed; left: 0; top: 0; bottom: 0; z-index: 46; display: flex; box-shadow: var(--shadow-3); animation: slide-in var(--duration-base) var(--ease); }
  @keyframes slide-in { from { transform: translateX(-100%); } }
  .fatal { height: 100%; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 8px; color: var(--danger); padding: 24px; text-align: center; }
  .fatal h1 { color: var(--text); font-size: 20px; margin: 0; }
  .fatal p { color: var(--text-2); margin: 0; max-width: 480px; }
  .text-size-notice {
    position: fixed; top: 72px; right: 20px; z-index: 100; padding: 10px 14px;
    border-radius: var(--radius-lg); background: var(--text); color: var(--bg-1);
    box-shadow: var(--shadow-2); font-size: 13px; font-weight: 600;
    animation: pop var(--duration-base) var(--ease);
  }
  /* narrow layout: chat list and conversation take the full width */
  @media (max-width: 899px) {
    .main :global(.chatlist) { width: 100%; border-right: 0; }
  }
</style>
