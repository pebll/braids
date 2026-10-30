<!--
  InfoPanel.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Chat info: identity, actions (pin, archive, leave), members, shared media.
  A 300 px column at >= 1280 px, an overlay sheet below.
-->
<script lang="ts">
  import { Archive, ArchiveRestore, LogOut, Pin, PinOff, Search, Trash2, X } from '@lucide/svelte';
  import * as api from '../lib/api';
  import { protocolMeta } from '../lib/protocols';
  import { splitKey, store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';
  import ProtocolBadge from './ProtocolBadge.svelte';

  let { chatKey, overlay = false }: { chatKey: string; overlay?: boolean } = $props();

  const [accountId, chatId] = $derived(splitKey(chatKey));
  const chat = $derived(store.chats[chatKey]);
  const account = $derived(store.accounts[accountId]);
  const membersLoaded = $derived(store.members[chatKey] !== undefined);
  const members = $derived(store.members[chatKey] ?? []);
  const images = $derived(
    (store.messages[chatKey]?.items ?? []).filter((m) => m.file?.status === 'downloaded' && m.file.mimeType.startsWith('image/')).slice(-9).reverse(),
  );
  const contact = $derived(chat && !chat.isGroup ? store.contacts[accountId]?.[chatId] : undefined);

  async function leave() {
    if (!chat) return;
    const name = store.chatName(chat);
    const ok = await ui.confirm(
      chat.isGroup ? `Leave “${name}”?` : `Delete chat with ${name}?`,
      chat.isGroup ? 'You will stop receiving messages from this group.' : 'The chat and its history are removed from this account.',
      chat.isGroup ? 'Leave' : 'Delete',
      true,
    );
    if (ok) await api.call('chats.delete', { accountId, chatId }).catch((e) => store.toastError(e));
  }
</script>

{#if chat && account}
  <aside class="info" class:overlay aria-label="Chat info">
    <div class="head">
      <h3>{chat.isGroup ? 'Group info' : 'Contact info'}</h3>
      <button class="icon-btn" aria-label="Close info" onclick={() => (ui.infoOpen = false)}><X size={19} /></button>
    </div>
    <div class="body scroll">
      <div class="identity">
        <Avatar name={store.chatName(chat)} id={chat.id} size={76} {accountId} ring="var(--bg-1)" />
        <div class="name">{store.chatName(chat)}</div>
        <div class="sub">
          <ProtocolBadge protocol={account.protocol} color={store.accountColor(accountId)} size={14} />
          {protocolMeta(account.protocol).name}{#if store.accountName(accountId) !== protocolMeta(account.protocol).name} · {store.accountName(accountId)}{/if}{#if chat.isGroup && members.length} · {members.length} members{/if}
        </div>
        {#if contact?.phone}<div class="sub">{contact.phone}</div>{/if}
        {#if !chat.isGroup}<div class="sub mono">{chat.id}</div>{/if}
      </div>

      <div class="actions">
        <button onclick={() => (ui.chatSearchOpen = true)}><Search size={18} />Search</button>
        {#if account.capabilities.pinChat}
          <button onclick={() => api.call('chats.pin', { accountId, chatId, isPinned: !chat.isPinned }).catch((e) => store.toastError(e))}>
            {#if chat.isPinned}<PinOff size={18} />Unpin{:else}<Pin size={18} />Pin{/if}
          </button>
        {/if}
        {#if account.capabilities.archiveChat}
          <button onclick={() => api.call('chats.archive', { accountId, chatId, isArchived: !chat.isArchived }).catch((e) => store.toastError(e))}>
            {#if chat.isArchived}<ArchiveRestore size={18} />Unarchive{:else}<Archive size={18} />Archive{/if}
          </button>
        {/if}
        <button class="danger" onclick={leave}>
          {#if chat.isGroup}<LogOut size={18} />Leave{:else}<Trash2 size={18} />Delete{/if}
        </button>
      </div>

      {#if chat.isMuted}
        <div class="banner info">Notifications for this chat are muted (change it in the {protocolMeta(account.protocol).name} app).</div>
      {/if}

      {#if chat.isGroup}
        <div class="section">Members{#if members.length} · {members.length}{/if}</div>
        {#if !membersLoaded}
          <div class="muted">Loading members…</div>
        {:else if !members.length}
          <div class="muted">The member list is not available.</div>
        {/if}
        <div class="members">
          {#each members.slice(0, 200) as member (member.id)}
            <div class="member">
              <Avatar name={member.name || member.id} id={member.id} size={30} online={store.isOnline(accountId, member.id)} ring="var(--bg-1)" />
              <span class="member-name">{member.isSelf ? 'You' : member.name || member.id}</span>
            </div>
          {/each}
          {#if members.length > 200}<div class="muted">and {members.length - 200} more</div>{/if}
        </div>
      {/if}

      {#if images.length}
        <div class="section">Shared media</div>
        <div class="media">
          {#each images as m (m.id)}
            <button onclick={() => (ui.lightbox = { src: api.fileUrl(m.file!.path), name: m.file!.name, path: m.file!.path })} aria-label="Open {m.file!.name}">
              <img src={api.fileUrl(m.file!.path)} alt={m.file!.name} loading="lazy" />
            </button>
          {/each}
        </div>
      {/if}
    </div>
  </aside>
{/if}

<style>
  .info { width: 300px; flex-shrink: 0; background: var(--bg-1); border-left: 1px solid var(--border); display: flex; flex-direction: column; min-height: 0; }
  .info.overlay {
    position: absolute; right: 12px; top: 12px; bottom: 12px; z-index: 40; border: 1px solid var(--border); border-radius: var(--radius-xl);
    box-shadow: var(--shadow-3); overflow: hidden; max-width: calc(100% - 24px);
  }
  .head { height: 56px; display: flex; align-items: center; padding: 0 12px 0 16px; border-bottom: 1px solid var(--border); flex-shrink: 0; }
  h3 { margin: 0; font-size: 15px; font-weight: 650; flex: 1; }
  .body { flex: 1; padding: 16px; display: flex; flex-direction: column; gap: 14px; }
  .identity { display: flex; flex-direction: column; align-items: center; gap: 4px; text-align: center; padding-top: 4px; }
  .name { font-size: 18px; font-weight: 650; margin-top: 8px; overflow-wrap: anywhere; }
  .sub { font-size: 12px; color: var(--text-muted); display: flex; align-items: center; gap: 6px; justify-content: center; flex-wrap: wrap; }
  .sub.mono { font-family: var(--mono); font-size: 11px; overflow-wrap: anywhere; }
  .actions { display: grid; grid-template-columns: repeat(auto-fit, minmax(76px, 1fr)); gap: 8px; }
  .actions button {
    display: flex; flex-direction: column; align-items: center; gap: 4px; padding: 10px 4px; border-radius: var(--radius-lg); border: 1px solid var(--border);
    background: var(--surface); font-size: 13px; color: var(--text-2);
  }
  .actions button:hover { background: var(--surface-2); }
  .actions button.danger { color: var(--danger); }
  .section { font-size: 11px; font-weight: 600; letter-spacing: 0.06em; text-transform: uppercase; color: var(--text-muted); margin-top: 4px; }
  .muted { font-size: 13px; color: var(--text-muted); }
  .members { display: flex; flex-direction: column; gap: 2px; margin-top: -6px; }
  .member { display: flex; align-items: center; gap: 10px; padding: 5px 0; }
  .member-name { font-size: 14px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .media { display: grid; grid-template-columns: repeat(3, 1fr); gap: 4px; margin-top: -6px; }
  .media button { border: 0; padding: 0; aspect-ratio: 1; border-radius: 6px; overflow: hidden; background: var(--skeleton); }
  .media img { width: 100%; height: 100%; object-fit: cover; display: block; }
</style>
