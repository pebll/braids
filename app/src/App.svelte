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
  import { onMount } from 'svelte';
  import AccountPrompt from './components/AccountPrompt.svelte';
  import Braid from './components/Braid.svelte';
  import ChatList from './components/ChatList.svelte';
  import Conversation from './components/Conversation.svelte';
  import InfoPanel from './components/InfoPanel.svelte';
  import NewChat from './components/NewChat.svelte';
  import Overlays from './components/Overlays.svelte';
  import Settings from './components/Settings.svelte';
  import Sidebar from './components/Sidebar.svelte';
  import Welcome from './components/Welcome.svelte';
  import Wizard from './components/Wizard.svelte';
  import * as api from './lib/api';
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

  onMount(() => {
    const onResize = () => (ui.width = window.innerWidth);
    window.addEventListener('resize', onResize);
    api.setCloseToTray(settings.closeToTray);
    api.onFileDrop(
      (paths) => {
        if (store.current && paths.length) conversation?.addFiles(paths);
      },
      (hover) => (ui.dropHover = hover && !!store.current),
    );
    return () => window.removeEventListener('resize', onResize);
  });

  function moveChat(delta: number) {
    const list = store.chatList;
    if (!list.length) return;
    const keys = list.map((c) => `${c.accountId}\u0001${c.id}`);
    const i = keys.indexOf(store.current);
    const next = keys[Math.max(0, Math.min(keys.length - 1, i < 0 ? 0 : i + delta))];
    store.openChat(next);
  }

  function onKey(e: KeyboardEvent) {
    const mod = e.ctrlKey || e.metaKey;
    const modal = ui.settingsOpen || ui.newChatOpen || !!store.wizardSetupId || !!ui.confirmRequest || !!ui.lightbox;
    if (modal) {
      if (mod && e.key === ',') e.preventDefault();
      return;
    }
    if (mod && e.key.toLowerCase() === 'k') {
      e.preventDefault();
      if (ui.narrow) store.closeChat();
      ui.focusSearch++;
    } else if (mod && e.key.toLowerCase() === 'f' && store.current) {
      e.preventDefault();
      ui.chatSearchOpen = true;
    } else if (mod && e.key.toLowerCase() === 'i' && store.current) {
      e.preventDefault();
      ui.infoOpen = !ui.infoOpen;
    } else if (mod && e.key.toLowerCase() === 'b') {
      e.preventDefault();
      settings.sidebarCollapsed = !settings.sidebarCollapsed;
      settings.save();
    } else if (mod && e.key === ',') {
      e.preventDefault();
      ui.settingsSection = 'general';
      ui.settingsOpen = true;
    } else if (mod && e.key.toLowerCase() === 'r' && store.current) {
      e.preventDefault();
      const items = store.messages[store.current]?.items ?? [];
      const last = [...items].reverse().find((m) => !m.isOutgoing && !m.isDeleted);
      if (last) {
        ui.edit = null;
        ui.reply = last;
        ui.focusComposer++;
      }
    } else if (e.altKey && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) {
      e.preventDefault();
      moveChat(e.key === 'ArrowDown' ? 1 : -1);
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
    <div class="main">
      {#if !ui.narrow}
        <Sidebar />
      {/if}
      {#if !ui.narrow || !store.current}
        <ChatList />
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
{#if prompt && !store.wizardSetupId}<AccountPrompt {prompt} />{/if}
<Overlays />

<style>
  .frame { height: 100%; display: flex; flex-direction: column; }
  .main { flex: 1; min-height: 0; display: flex; position: relative; }
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
  /* narrow layout: chat list and conversation take the full width */
  @media (max-width: 899px) {
    .main :global(.chatlist) { width: 100%; border-right: 0; }
  }
</style>
