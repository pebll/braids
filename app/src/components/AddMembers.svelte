<!--
  AddMembers.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Add people to a group (Matrix: invite them): contacts of the account, or a
  user id typed in (@user:server).
-->
<script lang="ts">
  import { Check, LoaderCircle, Search, UserPlus, X } from '@lucide/svelte';
  import { tick } from 'svelte';
  import { splitKey, store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';

  let { chatKey }: { chatKey: string } = $props();

  const [accountId, chatId] = $derived(splitKey(chatKey));
  const chat = $derived(store.chats[chatKey]);
  const invites = $derived(store.accounts[accountId]?.protocol === 'Matrix');
  const memberIds = $derived(new Set((store.members[chatKey] ?? []).map((m) => m.id)));

  let query = $state('');
  let chosen = $state<string[]>([]);
  let busy = $state(false);
  let error = $state('');
  let input: HTMLInputElement | undefined = $state();

  $effect(() => {
    tick().then(() => input?.focus());
  });

  // people only (no groups), not in the group yet
  const contacts = $derived.by(() => {
    const q = query.trim().toLowerCase();
    return Object.values(store.contacts[accountId] ?? {})
      .filter((c) => store.isPerson(accountId, c) && !memberIds.has(c.id))
      .filter((c) => !q || c.name.toLowerCase().includes(q) || c.id.toLowerCase().includes(q) || c.phone.includes(q))
      .sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id))
      .slice(0, 300);
  });

  // a Matrix user id not in the contact list can be typed in
  const typed = $derived(query.trim());
  const direct = $derived(/^@[^:\s]+:\S+$/.test(typed) && !memberIds.has(typed) && !chosen.includes(typed) && !contacts.some((c) => c.id === typed));

  function toggle(id: string) {
    error = '';
    chosen = chosen.includes(id) ? chosen.filter((c) => c !== id) : [...chosen, id];
  }

  function addTyped() {
    toggle(typed);
    query = '';
  }

  function close() {
    ui.addMembersFor = '';
  }

  async function add() {
    if (!chosen.length || busy) return;
    busy = true;
    error = '';
    try {
      await store.editMembers(accountId, chatId, chosen, false);
      const who = chosen.length === 1 ? name(chosen[0]) : `${chosen.length} people`;
      // Matrix invites: members once they accept
      store.toast(invites ? `Invited ${who}. They show up in the member list once they accept.` : `${who} added`);
      close();
    } catch (e) {
      error = (e as Error).message;
    } finally {
      busy = false;
    }
  }

  function name(id: string): string {
    return store.contacts[accountId]?.[id]?.name || id;
  }
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && close()} />

<div class="scrim" role="presentation" onclick={(e) => e.target === e.currentTarget && close()}>
  <div class="dialog" role="dialog" aria-modal="true" aria-labelledby="addmembers-title">
    <div class="dialog-head">
      <h2 id="addmembers-title">{chat ? `Add people to ${store.chatName(chat)}` : 'Add people'}</h2>
      <button class="icon-btn" aria-label="Close" onclick={close}><X size={18} /></button>
    </div>
    <form class="dialog-body" onsubmit={(e) => { e.preventDefault(); if (direct) addTyped(); else add(); }}>
      {#if chosen.length}
        <div class="chosen" aria-label="People to add">
          {#each chosen as id (id)}
            <button type="button" class="chip" onclick={() => toggle(id)} title="Remove">{name(id)}<X size={12} /></button>
          {/each}
        </div>
      {/if}
      <label class="search">
        <Search size={16} />
        <input bind:this={input} bind:value={query} placeholder="Name or user id (@user:server)" aria-label="Search people" />
      </label>
      <div class="list scroll">
        {#if direct}
          <button type="button" class="contact" onclick={addTyped}>
            <Avatar name={typed} id={typed} size={32} />
            <span class="cname">Add {typed}</span>
          </button>
        {/if}
        {#each contacts as c (c.id)}
          {@const on = chosen.includes(c.id)}
          <button type="button" class="contact" role="checkbox" aria-checked={on} onclick={() => toggle(c.id)}>
            <Avatar name={c.name || c.id} id={c.id} size={32} owner={accountId} />
            <span class="cname">{c.name || c.id}<small>{c.phone || (c.name ? c.id : '')}</small></span>
            <span class="tick" class:on>{#if on}<Check size={14} />{/if}</span>
          </button>
        {:else}
          {#if !direct}<p class="empty">{query ? 'No matching contacts. Type a full user id to add someone else.' : 'No contacts to add.'}</p>{/if}
        {/each}
      </div>
      {#if error}<p class="error" role="alert">{error}</p>{/if}
      <button class="btn primary" type="submit" disabled={!chosen.length || busy}>
        {#if busy}<LoaderCircle size={16} class="spin" />Adding…{:else}<UserPlus size={16} />Add{#if chosen.length > 1} {chosen.length} people{/if}{/if}
      </button>
    </form>
  </div>
</div>

<style>
  .dialog-body { display: flex; flex-direction: column; gap: 10px; }
  .dialog-body > .btn { align-self: flex-end; }
  .search { display: flex; align-items: center; gap: 8px; height: 38px; padding: 0 10px; border-radius: var(--radius-md); background: var(--surface-2); border: 1px solid var(--border); color: var(--text-muted); }
  .search input { flex: 1; border: 0; outline: none; background: transparent; color: var(--text); }
  .list { max-height: 300px; display: flex; flex-direction: column; gap: 1px; margin: 0 -8px; }
  .contact { display: flex; align-items: center; gap: 10px; padding: 6px 8px; border: 0; background: none; border-radius: var(--radius-md); text-align: left; }
  .contact:hover { background: var(--hover); }
  .cname { flex: 1; min-width: 0; display: flex; flex-direction: column; font-size: 14px; }
  .cname small { font-size: 12px; color: var(--text-muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .empty { color: var(--text-muted); font-size: 13px; text-align: center; }
  .error { font-size: 13px; color: var(--danger); margin: 0; }
  .chosen { display: flex; flex-wrap: wrap; gap: 6px; }
  .chip { display: inline-flex; align-items: center; gap: 4px; height: 26px; padding: 0 8px 0 10px; border-radius: 999px; border: 0; background: var(--accent-soft); color: var(--accent); font-size: 13px; font-weight: 600; }
  .tick { width: 20px; height: 20px; border-radius: 10px; border: 1.5px solid var(--border-strong); display: flex; align-items: center; justify-content: center; flex-shrink: 0; }
  .tick.on { background: var(--accent); border-color: var(--accent); color: var(--on-accent); }
</style>
