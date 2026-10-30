<!--
  Sidebar.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Folders (all, favorites), accounts with their state, archive, add account and
  settings. 232 px, collapsible to 60 px; a drawer in the narrow layout.
-->
<script lang="ts">
  import { Archive, CircleAlert, Inbox, LoaderCircle, Plus, Settings, Star, WifiOff } from '@lucide/svelte';
  import { settings } from '../lib/settings.svelte';
  import { store, type Filter } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';
  import Logo from './Logo.svelte';
  import ProtocolBadge from './ProtocolBadge.svelte';

  let { drawer = false }: { drawer?: boolean } = $props();
  const collapsed = $derived(settings.sidebarCollapsed && !drawer);

  const syncing = $derived(store.accountList.filter((a) => a.state === 'connecting').length);
  const archivedCount = $derived(Object.values(store.chats).filter((c) => store.accounts[c.accountId] && store.category(c) === 'archived').length);

  function select(f: Filter) {
    store.filter = f;
    ui.drawerOpen = false;
  }

  function isActive(f: Filter): boolean {
    const cur = store.filter;
    return cur.kind === f.kind && (f.kind !== 'account' || (cur.kind === 'account' && cur.accountId === f.accountId));
  }

  function toggle() {
    settings.sidebarCollapsed = !settings.sidebarCollapsed;
    settings.save();
  }

  function stateTitle(state: string, error: string): string {
    if (state === 'error') return error || 'Error';
    if (state === 'connecting') return 'Connecting…';
    if (state === 'offline') return 'Offline';
    return 'Online';
  }
</script>

<nav class="sidebar" class:collapsed aria-label="Folders and accounts">
  <!-- the logo is the sidebar toggle (not in the narrow-layout drawer) -->
  {#if drawer}
    <div class="brand"><Logo size={22} /><span class="name">braids</span></div>
  {:else}
    <button class="brand logo-toggle" title={collapsed ? 'Expand sidebar (Ctrl+B)' : 'Collapse sidebar (Ctrl+B)'} aria-label="Toggle sidebar" onclick={toggle}>
      <Logo size={22} />
      {#if !collapsed}<span class="name">braids</span>{/if}
    </button>
  {/if}

  <div class="items">
    {#each [{ f: { kind: 'all' } as Filter, label: 'All chats', icon: Inbox }, { f: { kind: 'favorites' } as Filter, label: 'Favorites', icon: Star }] as item}
      {@const n = store.unreadCount(item.f)}
      <button class="item" class:active={isActive(item.f)} onclick={() => select(item.f)} title={collapsed ? item.label : undefined}>
        <span class="ico"><item.icon size={18} strokeWidth={1.75} /></span>
        {#if !collapsed}<span class="label">{item.label}</span>{/if}
        {#if n > 0}<span class="count" class:corner={collapsed}>{n}</span>{/if}
      </button>
    {/each}
  </div>

  {#if store.accountList.length}
    {#if !collapsed}<div class="section">Accounts</div>{:else}<div class="sep"></div>{/if}
    <div class="items">
      {#each store.accountList as account (account.accountId)}
        {@const f = { kind: 'account', accountId: account.accountId } as Filter}
        {@const n = store.unreadCount(f)}
        <button
          class="item"
          class:active={isActive(f)}
          class:dim={account.state === 'offline'}
          onclick={() => select(f)}
          title={`${store.accountName(account.accountId)} — ${stateTitle(account.state, account.error)}`}
        >
          <span class="ico"><ProtocolBadge protocol={account.protocol} color={store.accountColor(account.accountId)} size={18} /></span>
          {#if !collapsed}<span class="label">{store.accountName(account.accountId)}</span>{/if}
          {#if !collapsed}
            {#if account.state === 'connecting'}
              <span class="state" aria-label="Connecting"><LoaderCircle size={14} class="spin" /></span>
            {:else if account.state === 'offline'}
              <span class="state" aria-label="Offline"><WifiOff size={14} /></span>
            {:else if account.state === 'error'}
              <span class="state warn" aria-label="Error"><CircleAlert size={15} /></span>
            {/if}
          {/if}
          {#if n > 0}<span class="count" class:corner={collapsed}>{n}</span>{/if}
        </button>
      {/each}
    </div>
  {/if}

  <div class="sep"></div>
  <div class="items">
    <button class="item" class:active={isActive({ kind: 'archived' })} onclick={() => select({ kind: 'archived' })} title={collapsed ? 'Archived' : undefined}>
      <span class="ico"><Archive size={18} strokeWidth={1.75} /></span>
      {#if !collapsed}<span class="label">Archived</span>{/if}
      {#if archivedCount > 0 && !collapsed}<span class="count muted">{archivedCount}</span>{/if}
    </button>
  </div>

  <div class="grow"></div>

  {#if !collapsed && (syncing > 0 || !store.connected)}
    <div class="status">
      <LoaderCircle size={13} class="spin" />
      <span>{!store.connected ? 'Connecting to braids core…' : `${syncing} account${syncing > 1 ? 's' : ''} connecting…`}</span>
    </div>
  {/if}

  <div class="foot">
    <button class="add" onclick={() => { ui.drawerOpen = false; store.wizardSetupId = 'new'; }} title="Add account">
      <Plus size={18} strokeWidth={1.75} />
      {#if !collapsed}Add account{/if}
    </button>
    {#if !collapsed}
      <button class="icon-btn" aria-label="Settings" title="Settings (Ctrl+,)" onclick={() => { ui.drawerOpen = false; ui.settingsSection = 'general'; ui.settingsOpen = true; }}>
        <Settings size={18} strokeWidth={1.75} />
      </button>
    {/if}
  </div>
  {#if collapsed}
    <button class="item" title="Settings" onclick={() => { ui.settingsSection = 'general'; ui.settingsOpen = true; }}>
      <span class="ico"><Settings size={18} strokeWidth={1.75} /></span>
    </button>
  {/if}
</nav>

<style>
  .sidebar {
    width: 232px; flex-shrink: 0; background: var(--bg-0); border-right: 1px solid var(--border); display: flex;
    flex-direction: column; padding: 12px 8px 8px; min-height: 0; overflow-y: auto; overflow-x: hidden;
  }
  .sidebar.collapsed { width: 60px; align-items: stretch; }
  .brand { display: flex; align-items: center; gap: 8px; padding: 4px 6px; margin-bottom: 6px; height: 34px; }
  .logo-toggle { border: 0; background: transparent; border-radius: var(--radius-md); color: var(--text); text-align: left; width: 100%; }
  .logo-toggle:hover { background: var(--hover); }
  .collapsed .brand { justify-content: center; padding: 0; }
  .name { font-size: 16px; font-weight: 650; letter-spacing: -0.01em; flex: 1; }
  .items { display: flex; flex-direction: column; gap: 2px; }
  .item {
    position: relative; display: flex; align-items: center; gap: 10px; height: 34px; padding: 0 10px; border: 0; border-radius: var(--radius-md);
    background: transparent; width: 100%; text-align: left; color: var(--text); font-size: 14px; font-weight: 500;
  }
  .collapsed .item { justify-content: center; padding: 0; }
  .item:hover { background: var(--hover); }
  .item.active { background: var(--selected); font-weight: 600; }
  .item.dim { color: var(--text-muted); }
  .item.dim .ico { opacity: 0.55; }
  .ico { color: var(--text-2); display: flex; }
  .label { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .state { color: var(--text-muted); display: flex; }
  .state.warn { color: var(--warning); }
  .count.corner { position: absolute; top: 1px; right: 4px; min-width: 16px; height: 16px; font-size: 10px; line-height: 16px; padding: 0 4px; }
  .section { font-size: 11px; font-weight: 600; letter-spacing: 0.06em; text-transform: uppercase; color: var(--text-muted); padding: 14px 10px 6px; }
  .sep { height: 1px; background: var(--border); margin: 10px 6px; }
  .grow { flex: 1; }
  .status { display: flex; align-items: center; gap: 6px; padding: 6px 10px; font-size: 12px; color: var(--text-muted); }
  .foot { display: flex; gap: 4px; padding-top: 4px; border-top: 1px solid var(--border); }
  .add {
    flex: 1; display: flex; align-items: center; justify-content: flex-start; gap: 8px; height: 36px; border: 0; background: transparent; border-radius: var(--radius-md);
    padding: 0 10px; font-size: 14px; color: var(--text-2); font-weight: 500;
  }
  .collapsed .add { justify-content: center; padding: 0; }
  .add:hover { background: var(--hover); color: var(--text); }
</style>
