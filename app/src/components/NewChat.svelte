<!--
  NewChat.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Start a chat with a contact of one of the accounts, or join a group by
  invite link / address.
-->
<script lang="ts">
  import { Link, LoaderCircle, Search, X } from '@lucide/svelte';
  import { tick } from 'svelte';
  import { chatKey, store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';
  import ProtocolBadge from './ProtocolBadge.svelte';

  let accountId = $state(store.filter.kind === 'account' ? store.filter.accountId : (store.accountList[0]?.accountId ?? ''));
  let query = $state('');
  let busy = $state('');
  let input: HTMLInputElement | undefined = $state();

  // join mode
  let mode = $state<'contact' | 'join'>('contact');
  let link = $state('');
  let joinError = $state('');
  let linkInput: HTMLInputElement | undefined = $state();
  const joinAccounts = $derived(store.accountList.filter((a) => a.capabilities.joinChat));

  // the messenger a link belongs to, '' when it does not tell
  function linkProtocol(l: string): string {
    const s = l.trim();
    if (/(^|\/\/)(t\.me|telegram\.me)\/|^tg:\/\/|^@/.test(s)) return 'Telegram';
    if (/chat\.whatsapp\.com\//.test(s)) return 'WhatsAppMd';
    if (/matrix\.to\/|^[#!][^:\s]+:\S+$/.test(s)) return 'Matrix';
    if (/dummy\.example\//.test(s)) return 'Dummy';
    return '';
  }

  // follow the link: pick an account of its messenger
  $effect(() => {
    const protocol = linkProtocol(link);
    if (mode !== 'join' || !protocol) return;
    const current = store.accounts[accountId];
    if (current?.protocol === protocol) return;
    const match = joinAccounts.find((a) => a.protocol === protocol);
    if (match) accountId = match.accountId;
  });

  $effect(() => {
    if (mode === 'join') tick().then(() => linkInput?.focus());
  });

  async function join() {
    if (!link.trim() || !accountId) return;
    joinError = '';
    busy = 'join';
    try {
      await store.joinChat(accountId, link.trim());
      ui.newChatOpen = false;
    } catch (e) {
      joinError = (e as Error).message;
    } finally {
      busy = '';
    }
  }

  const contacts = $derived.by(() => {
    const q = query.trim().toLowerCase();
    return Object.values(store.contacts[accountId] ?? {})
      .filter((c) => !c.isSelf && (c.name || c.id))
      .filter((c) => !q || c.name.toLowerCase().includes(q) || c.id.toLowerCase().includes(q) || c.phone.includes(q))
      .sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id))
      .slice(0, 300);
  });

  // matrix user ids / phone numbers not in the contact list can be typed directly
  const direct = $derived(/^@[^:\s]+:\S+$/.test(query.trim()) || /^\+\d{6,}$/.test(query.trim().replace(/\s/g, '')));

  $effect(() => {
    tick().then(() => input?.focus());
  });

  async function start(userId: string) {
    const existing = store.chats[chatKey(accountId, userId)];
    if (existing) {
      store.openChat(chatKey(accountId, userId));
      ui.newChatOpen = false;
      return;
    }
    busy = userId;
    try {
      await store.createChat(accountId, userId);
      ui.newChatOpen = false;
    } catch (e) {
      store.toastError(e);
    } finally {
      busy = '';
    }
  }
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && (ui.newChatOpen = false)} />

<div class="scrim" role="presentation" onclick={(e) => e.target === e.currentTarget && (ui.newChatOpen = false)}>
  <div class="dialog" role="dialog" aria-modal="true" aria-labelledby="newchat-title">
    <div class="dialog-head">
      <h2 id="newchat-title">{mode === 'join' ? 'Join a group' : 'New chat'}</h2>
      <button class="icon-btn" aria-label="Close" onclick={() => (ui.newChatOpen = false)}><X size={18} /></button>
    </div>
    <div class="dialog-body">
      <div class="modes" role="tablist">
        <button role="tab" aria-selected={mode === 'contact'} onclick={() => (mode = 'contact')}>Message a contact</button>
        <button role="tab" aria-selected={mode === 'join'} onclick={() => (mode = 'join')} disabled={!joinAccounts.length}
          title={joinAccounts.length ? undefined : 'None of your accounts can join groups by link'}>Join a group</button>
      </div>
      {#if mode === 'join'}
        <div class="accounts">
          {#each joinAccounts as a (a.accountId)}
            <button class="acct" aria-pressed={a.accountId === accountId} onclick={() => (accountId = a.accountId)}>
              <ProtocolBadge protocol={a.protocol} color={store.accountColor(a.accountId)} size={16} />{store.accountName(a.accountId)}
            </button>
          {/each}
        </div>
        <form class="join" onsubmit={(e) => { e.preventDefault(); join(); }}>
          <label class="search">
            <Link size={16} />
            <input bind:this={linkInput} bind:value={link} placeholder="Invite link or address" aria-label="Invite link or address" oninput={() => (joinError = '')} />
          </label>
          <p class="hint">Telegram: https://t.me/+… or @name · WhatsApp: https://chat.whatsapp.com/… · Matrix: #room:server or a matrix.to link. Signal invite links are not supported yet.</p>
          {#if joinError}<p class="error" role="alert">{joinError}</p>{/if}
          <button class="btn primary" type="submit" disabled={!link.trim() || !accountId || !joinAccounts.some((a) => a.accountId === accountId) || busy === 'join'}>
            {#if busy === 'join'}<LoaderCircle size={16} class="spin" />Joining…{:else}Join{/if}
          </button>
        </form>
      {:else}
      <div class="accounts">
        {#each store.accountList as a (a.accountId)}
          <button class="acct" aria-pressed={a.accountId === accountId} onclick={() => (accountId = a.accountId)}>
            <ProtocolBadge protocol={a.protocol} color={store.accountColor(a.accountId)} size={16} />{store.accountName(a.accountId)}
          </button>
        {/each}
      </div>
      <label class="search">
        <Search size={16} />
        <input bind:this={input} bind:value={query} placeholder="Name, number or user id" aria-label="Search contacts" />
      </label>
      <div class="list scroll">
        {#if direct}
          <button class="contact" onclick={() => start(query.trim().replace(/\s/g, ''))}>
            <Avatar name={query.trim()} id={query.trim()} size={36} />
            <span class="cname">Message {query.trim()}</span>
            {#if busy}<LoaderCircle size={16} class="spin" />{/if}
          </button>
        {/if}
        {#each contacts as c (c.id)}
          <button class="contact" onclick={() => start(c.id)} disabled={!!busy}>
            <Avatar name={c.name || c.id} id={c.id} size={36} owner={accountId} online={store.isOnline(accountId, c.id)} ring="var(--surface)" />
            <span class="cname">{c.name || c.id}<small>{c.phone || (c.name ? c.id : '')}</small></span>
            {#if busy === c.id}<LoaderCircle size={16} class="spin" />{/if}
          </button>
        {:else}
          {#if !direct}<p class="empty">{query ? 'No matching contacts.' : 'No contacts for this account yet.'}</p>{/if}
        {/each}
      </div>
      {/if}
    </div>
  </div>
</div>

<style>
  .accounts { display: flex; gap: 6px; flex-wrap: wrap; }
  .acct { display: inline-flex; align-items: center; gap: 6px; height: 30px; padding: 0 10px; border-radius: 999px; border: 1px solid var(--border); background: var(--surface); font-size: 13px; }
  .acct[aria-pressed='true'] { border-color: var(--accent); background: var(--accent-soft); font-weight: 600; }
  .search { display: flex; align-items: center; gap: 8px; height: 38px; padding: 0 10px; border-radius: var(--radius-md); background: var(--surface-2); border: 1px solid var(--border); color: var(--text-muted); }
  .search input { flex: 1; border: 0; outline: none; background: transparent; color: var(--text); }
  .list { max-height: 360px; display: flex; flex-direction: column; gap: 1px; margin: 0 -8px; }
  .contact { display: flex; align-items: center; gap: 10px; padding: 6px 8px; border: 0; background: none; border-radius: var(--radius-md); text-align: left; }
  .contact:hover { background: var(--hover); }
  .cname { flex: 1; min-width: 0; display: flex; flex-direction: column; font-size: 14px; }
  .cname small { font-size: 12px; color: var(--text-muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .empty { color: var(--text-muted); font-size: 13px; text-align: center; }
  .modes { display: flex; gap: 4px; padding: 3px; border-radius: var(--radius-md); background: var(--surface-2); }
  .modes button { flex: 1; height: 30px; border: 0; border-radius: calc(var(--radius-md) - 2px); background: none; font-size: 13px; color: var(--text-2); }
  .modes button[aria-selected='true'] { background: var(--surface); color: var(--text); font-weight: 600; box-shadow: var(--shadow-1); }
  .join { display: flex; flex-direction: column; gap: 10px; }
  .join .btn { align-self: flex-end; }
  .hint { font-size: 12px; color: var(--text-muted); margin: 0; }
  .error { font-size: 13px; color: var(--danger); margin: 0; }
</style>
