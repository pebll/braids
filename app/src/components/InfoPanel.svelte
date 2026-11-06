<!--
  InfoPanel.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Chat info: identity, actions (favorite, archive, leave), members, shared media.
  A 300 px column at >= 1280 px, an overlay sheet below.
-->
<script lang="ts">
  import { Archive, ArchiveRestore, Bell, BellOff, ChevronDown, ChevronUp, Images, LogOut, Search, Star, StarOff, Trash2, X } from '@lucide/svelte';
  import { openLink, personMenu, viewPicture } from '../lib/actions';
  import * as api from '../lib/api';
  import { linkify } from '../lib/format';
  import { protocolMeta } from '../lib/protocols';
  import { fileMime, fileReady, galleryItems, splitKey, store } from '../lib/store.svelte';
  import type { Message } from '../lib/types';
  import { openLightbox, ui } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';
  import ProtocolBadge from './ProtocolBadge.svelte';

  let { chatKey, overlay = false }: { chatKey: string; overlay?: boolean } = $props();

  const [accountId, chatId] = $derived(splitKey(chatKey));
  const chat = $derived(store.chats[chatKey]);
  const account = $derived(store.accounts[accountId]);
  const membersLoaded = $derived(store.members[chatKey] !== undefined);
  const members = $derived(store.members[chatKey] ?? []);
  // the newest shared images (also older than the loaded history), again
  // when images are added to the loaded history
  let shared = $state<Message[]>([]);
  const loadedImages = $derived((store.messages[chatKey]?.items ?? []).filter((m) => fileReady(m.file) && fileMime(m.file).startsWith('image/')).length);
  $effect(() => {
    const key = chatKey;
    void loadedImages;
    const [a, c] = splitKey(key);
    api.call('media.list', { accountId: a, chatId: c, kind: 'media', limit: 30 })
      .then((r) => {
        if (key === chatKey) shared = (r.items as Message[]).filter((m) => fileReady(m.file) && fileMime(m.file).startsWith('image/')).slice(0, 9);
      })
      .catch(() => {});
  });
  const images = $derived(shared);
  const contact = $derived(chat && !chat.isGroup ? store.contacts[accountId]?.[chatId] : undefined);
  const description = $derived(chat ? store.description(accountId, chatId).trim() : '');
  const longDescription = $derived(description.length > 220 || description.split('\n').length > 4);
  let descriptionOpen = $state(false);
  const MEMBERS_SHOWN = 5;
  let membersOpen = $state(false);
  $effect(() => {
    void chatId;
    descriptionOpen = false;
    membersOpen = false;
  });

  function onDescriptionClick(e: MouseEvent) {
    const a = (e.target as HTMLElement).closest('a[data-link]') as HTMLAnchorElement | null;
    if (a) {
      e.preventDefault();
      openLink(a.href);
    }
  }

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
        {#if store.avatar(accountId, chat.id)}
          <button class="pic" aria-label="View profile picture" title="View profile picture" onclick={() => viewPicture(accountId, chat.id, store.chatName(chat))}>
            <Avatar name={store.chatName(chat)} id={chat.id} size={76} {accountId} group={chat.isGroup} ring="var(--bg-1)" />
          </button>
        {:else}
          <Avatar name={store.chatName(chat)} id={chat.id} size={76} {accountId} group={chat.isGroup} ring="var(--bg-1)" />
        {/if}
        <div class="name">{store.chatName(chat)}</div>
        <div class="sub">
          <ProtocolBadge protocol={account.protocol} color={store.accountColor(accountId)} size={14} />
          {protocolMeta(account.protocol).name}{#if store.accountName(accountId) !== protocolMeta(account.protocol).name} · {store.accountName(accountId)}{/if}{#if chat.isGroup && members.length} · {members.length} members{/if}
        </div>
        {#if contact?.phone}<div class="sub">{contact.phone}</div>{/if}
        {#if !chat.isGroup}<div class="sub mono">{chat.id}</div>{/if}
      </div>

      {#if description}
        <div class="description">
          <div class="desc-label">{chat.isGroup ? 'Description' : 'About'}</div>
          <!-- eslint-disable-next-line svelte/no-at-html-tags: linkify escapes all input -->
          <p class:clamped={longDescription && !descriptionOpen} onclick={onDescriptionClick} role="presentation">{@html linkify(description)}</p>
          {#if longDescription}
            <button class="more" onclick={() => (descriptionOpen = !descriptionOpen)}>{descriptionOpen ? 'Show less' : 'Show more'}</button>
          {/if}
        </div>
      {/if}

      <div class="actions">
        <button onclick={() => (ui.chatSearchOpen = true)}><Search size={18} />Search</button>
        <button onclick={() => (ui.media = { chatKey })}><Images size={18} />Media</button>
        <button class:on={store.category(chat) === 'favorite'}
          onclick={() => store.setCategory(chat, store.category(chat) === 'favorite' ? 'default' : 'favorite')}>
          {#if store.category(chat) === 'favorite'}<StarOff size={18} />Unfavorite{:else}<Star size={18} />Favorite{/if}
        </button>
        <button class:on={chat.isMuted} onclick={() => store.setMuted(chat, !chat.isMuted)}>
          {#if chat.isMuted}<Bell size={18} />Unmute{:else}<BellOff size={18} />Mute{/if}
        </button>
        <button onclick={() => store.setCategory(chat, store.category(chat) === 'archived' ? 'default' : 'archived')}>
          {#if store.category(chat) === 'archived'}<ArchiveRestore size={18} />Unarchive{:else}<Archive size={18} />Archive{/if}
        </button>
        <button class="danger" onclick={leave}>
          {#if chat.isGroup}<LogOut size={18} />Leave{:else}<Trash2 size={18} />Delete{/if}
        </button>
      </div>

      {#if chat.isGroup}
        <div class="section">Members{#if members.length} · {members.length}{/if}</div>
        {#if !membersLoaded}
          <div class="muted">Loading members…</div>
        {:else if !members.length}
          <div class="muted">The member list is not available.</div>
        {/if}
        <div class="members">
          {#each members.slice(0, membersOpen ? 200 : MEMBERS_SHOWN) as member (member.id)}
            {#if member.isSelf}
              <div class="member">
                <Avatar name={member.name || member.id} id={member.id} size={30} owner={accountId} online={store.isOnline(accountId, member.id)} ring="var(--bg-1)" />
                <span class="member-name">You</span>
              </div>
            {:else}
              <button class="member" onclick={(e) => ui.openMenu(e, personMenu(accountId, member.id, member.name || member.id))}>
                <Avatar name={member.name || member.id} id={member.id} size={30} owner={accountId} online={store.isOnline(accountId, member.id)} ring="var(--bg-1)" />
                <span class="member-name">{member.name || member.id}</span>
              </button>
            {/if}
          {/each}
          {#if membersOpen && members.length > 200}<div class="muted">and {members.length - 200} more</div>{/if}
        </div>
        {#if members.length > MEMBERS_SHOWN}
          <button class="more members-toggle" onclick={() => (membersOpen = !membersOpen)} aria-expanded={membersOpen}>
            {#if membersOpen}<ChevronUp size={15} />Show less{:else}<ChevronDown size={15} />Show all {members.length}{/if}
          </button>
        {/if}
      {/if}

      {#if images.length}
        <div class="section media-head">Shared media<button class="link" onclick={() => (ui.media = { chatKey })}>Show all</button></div>
        <div class="media">
          {#each images as m (m.id)}
            <button onclick={() => openLightbox(galleryItems(images), m.file!.path)} aria-label="Open {m.file!.name}">
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
  .description { display: flex; flex-direction: column; gap: 4px; }
  .desc-label { font-size: 11px; font-weight: 600; letter-spacing: 0.06em; text-transform: uppercase; color: var(--text-muted); }
  .description p { margin: 0; font-size: 13px; line-height: 19px; color: var(--text); overflow-wrap: anywhere; user-select: text; -webkit-user-select: text; }
  .description p.clamped { display: -webkit-box; -webkit-line-clamp: 4; line-clamp: 4; -webkit-box-orient: vertical; overflow: hidden; }
  .description :global(a) { color: var(--accent); }
  .more { align-self: flex-start; border: 0; background: none; padding: 0; font-size: 12px; font-weight: 600; color: var(--accent); }
  .members-toggle { display: inline-flex; align-items: center; gap: 4px; margin-top: -4px; }
  .actions { display: grid; grid-template-columns: repeat(auto-fit, minmax(76px, 1fr)); gap: 8px; }
  .actions button {
    display: flex; flex-direction: column; align-items: center; gap: 4px; padding: 10px 4px; border-radius: var(--radius-lg); border: 1px solid var(--border);
    background: var(--surface); font-size: 13px; color: var(--text-2);
  }
  .actions button:hover { background: var(--surface-2); }
  .actions button.danger { color: var(--danger); }
  .actions button.on { color: var(--accent); border-color: var(--accent); background: var(--accent-soft); }
  .section { font-size: 11px; font-weight: 600; letter-spacing: 0.06em; text-transform: uppercase; color: var(--text-muted); margin-top: 4px; }
  .muted { font-size: 13px; color: var(--text-muted); }
  .members { display: flex; flex-direction: column; gap: 2px; margin-top: -6px; }
  .member { display: flex; align-items: center; gap: 10px; padding: 5px 6px; margin: 0 -6px; width: calc(100% + 12px); border: 0; background: none; border-radius: var(--radius-md); text-align: left; color: var(--text); font-family: inherit; }
  button.member:hover { background: var(--hover); }
  .pic { border: 0; background: none; padding: 0; border-radius: 50%; cursor: zoom-in; }
  .member-name { font-size: 14px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .media-head { display: flex; align-items: center; justify-content: space-between; }
  .media-head .link { border: 0; background: none; padding: 0; font-size: 12px; font-weight: 600; letter-spacing: 0; text-transform: none; color: var(--accent); }
  .media { display: grid; grid-template-columns: repeat(3, 1fr); gap: 4px; margin-top: -6px; }
  .media button { border: 0; padding: 0; aspect-ratio: 1; border-radius: 6px; overflow: hidden; background: var(--skeleton); }
  .media img { width: 100%; height: 100%; object-fit: cover; display: block; }
</style>
