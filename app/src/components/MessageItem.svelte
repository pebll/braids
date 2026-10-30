<!--
  MessageItem.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  One message: bubble (own right, others left), sender name and avatar in
  groups, quote, attachment, reactions, hover actions.
-->
<script lang="ts">
  import { Check, CheckCheck, Copy, Download, FileText, LoaderCircle, MoreHorizontal, Pencil, Pin, PinOff, Reply, SmilePlus, Trash2, TriangleAlert } from '@lucide/svelte';
  import * as api from '../lib/api';
  import { isOnlyEmoji, renderText, senderColor, timeOfDay } from '../lib/format';
  import { fileMime, fileReady, store } from '../lib/store.svelte';
  import type { Account, Chat, Message } from '../lib/types';
  import { ui, type MenuItem } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';

  let {
    m,
    chat,
    account,
    first,
    last,
    onJump,
    onReact,
  }: {
    m: Message;
    chat: Chat;
    account: Account;
    first: boolean; // first of a group of consecutive messages from one sender
    last: boolean;
    onJump: (msgId: string) => void;
    onReact: (m: Message, anchor: HTMLElement) => void;
  } = $props();

  const QUICK = ['👍', '❤️', '😂', '😮'];
  const own = $derived(m.isOutgoing);
  const showName = $derived(chat.isGroup && !own && first);
  const showAvatar = $derived(chat.isGroup && !own);
  const mime = $derived(fileMime(m.file));
  const isImage = $derived(mime.startsWith('image/'));
  const isVideo = $derived(mime.startsWith('video/') && mime !== 'video/webm');
  const ready = $derived(fileReady(m.file));
  const emojiOnly = $derived(!m.file && !m.quoted && !m.isDeleted && isOnlyEmoji(m.text));
  const html = $derived(m.isDeleted ? '' : renderText(m.text, m.mentions, account.selfId));
  const reactions = $derived(Object.entries(m.reactions.counts).filter(([, n]) => n > 0));
  const mentionsMe = $derived(!own && (m.hasMention || Object.values(m.mentions).includes(account.selfId)));
  let moreBtn: HTMLButtonElement | undefined = $state();
  let reactBtn: HTMLButtonElement | undefined = $state();
  let requestedDownload = false;

  const editState = $derived.by(() => {
    const win = account.capabilities.editWindowSec;
    if (!own || m.isDeleted || !m.text) return { show: false, ok: false, reason: '' };
    if (win === 0) return { show: false, ok: false, reason: '' };
    if (win > 0 && Date.now() - m.time > win * 1000) {
      const span = win >= 3600 ? `${Math.round(win / 3600)} h` : `${Math.round(win / 60)} min`;
      return { show: true, ok: false, reason: `Edit window has expired (${span})` };
    }
    return { show: true, ok: true, reason: '' };
  });

  // images are fetched right away, other files on request
  $effect(() => {
    if ((isImage || isVideo) && m.file?.status === 'notDownloaded' && !requestedDownload) {
      requestedDownload = true;
      download();
    }
  });

  function download() {
    api.call('files.download', { accountId: m.accountId, chatId: m.chatId, msgId: m.id }).catch((e) => store.toastError(e));
  }

  function openFile() {
    if (!m.file) return;
    if (ready) {
      if (isImage) ui.lightbox = { src: api.fileUrl(m.file.path), name: m.file.name, path: m.file.path };
      else api.openPath(m.file.path).catch((e) => store.toastError(e));
    } else if (m.file.status !== 'downloading') {
      download();
    }
  }

  function react(emoji: string) {
    api.call('reactions.send', { accountId: m.accountId, chatId: m.chatId, msgId: m.id, emoji: m.reactions.own === emoji ? '' : emoji })
      .catch((e) => store.toastError(e));
  }

  function reply() {
    ui.edit = null;
    ui.reply = m;
    ui.focusComposer++;
  }

  function edit() {
    if (!editState.ok) return;
    ui.reply = null;
    ui.edit = m;
    ui.focusComposer++;
  }

  async function remove() {
    if (m.isDeleted) {
      await api.call('messages.delete', { accountId: m.accountId, chatId: m.chatId, msgId: m.id }).catch((e) => store.toastError(e));
      return;
    }
    const ok = await ui.confirm('Delete message?', 'The message is deleted for everyone in this chat.', 'Delete for everyone', true);
    if (ok) await api.call('messages.delete', { accountId: m.accountId, chatId: m.chatId, msgId: m.id }).catch((e) => store.toastError(e));
  }

  export function menuItems(): MenuItem[] {
    const items: MenuItem[] = [{ label: 'Reply', icon: Reply, run: reply }];
    if (editState.show) items.push({ label: 'Edit', icon: Pencil, run: edit, disabled: !editState.ok, hint: editState.reason });
    if (m.text) items.push({ label: 'Copy text', icon: Copy, run: () => navigator.clipboard.writeText(m.text).catch(() => {}) });
    if (m.file) {
      items.push({
        label: ready ? 'Open file' : 'Download',
        icon: Download,
        run: openFile,
      });
    }
    if (account.capabilities.pinMessage) {
      items.push({
        label: m.isPinned ? 'Unpin message' : 'Pin message',
        icon: m.isPinned ? PinOff : Pin,
        run: () => api.call('messages.pin', { accountId: m.accountId, chatId: m.chatId, msgId: m.id, isPinned: !m.isPinned }).catch((e) => store.toastError(e)),
      });
    }
    if (own || m.isDeleted) items.push({ label: m.isDeleted ? 'Remove' : 'Delete for everyone', icon: Trash2, danger: true, run: remove });
    return items;
  }

  function showMenu(e: MouseEvent) {
    e.preventDefault();
    ui.openMenu(e, menuItems());
  }

  function onTextClick(e: MouseEvent) {
    const a = (e.target as HTMLElement).closest('a[data-link]') as HTMLAnchorElement | null;
    if (a) {
      e.preventDefault();
      api.openUrl(a.href).catch(() => {});
    }
  }

  function reactionTitle(emoji: string): string {
    const names = Object.entries(m.reactions.senders)
      .filter(([, e]) => e === emoji)
      .map(([id]) => store.contacts[m.accountId]?.[id]?.name || id);
    if (m.reactions.own === emoji) names.unshift('You');
    return names.length ? names.join(', ') : `${m.reactions.counts[emoji]}`;
  }
</script>

<div
  class="msg"
  class:own
  class:first
  class:last
  class:highlight={ui.highlightId === m.id}
  data-msg-id={m.id}
  oncontextmenu={showMenu}
  role="article"
  aria-label={`${store.senderName(m)}, ${timeOfDay(m.time)}`}
>
  {#if showAvatar}
    {#if last}
      <Avatar name={store.senderName(m)} id={m.senderId} size={30} owner={m.accountId} />
    {:else}
      <span class="avatar-space"></span>
    {/if}
  {/if}

  <div class="col">
    <div
      class="bubble"
      class:emoji={emojiOnly}
      class:mention={mentionsMe}
      class:deleted={m.isDeleted}
      class:has-image={(isImage || isVideo) && ready}
    >
      {#if showName}
        <div class="sender" style:color={senderColor(m.senderId)}>{store.senderName(m)}</div>
      {/if}

      {#if m.quoted}
        <button class="quote" onclick={() => onJump(m.quoted!.id)} style:--q={senderColor(m.quoted.senderId)}>
          <span class="q-name">{m.quoted.senderId === account.selfId ? 'You' : m.quoted.senderName || m.quoted.senderId}</span>
          <span class="q-text">{store.quotedText(m)}</span>
        </button>
      {/if}

      {#if m.file && !m.isDeleted}
        {#if isImage && ready}
          <button class="image" onclick={openFile} aria-label="Open image {m.file.name}">
            <img src={api.fileUrl(m.file.path)} alt={m.file.name} loading="lazy" />
          </button>
        {:else if isVideo && ready}
          <!-- svelte-ignore a11y_media_has_caption -->
          <video class="video" src={api.fileUrl(m.file.path)} controls preload="metadata"></video>
        {:else}
          <button class="file" onclick={openFile} title={ready ? 'Open' : 'Download'}>
            <span class="file-ico">
              {#if m.file.status === 'downloading'}<LoaderCircle size={20} class="spin" />
              {:else if m.file.status === 'failed'}<TriangleAlert size={20} />
              {:else if ready}<FileText size={20} />
              {:else}<Download size={20} />{/if}
            </span>
            <span class="file-meta">
              <span class="file-name">{m.file.name || 'File'}</span>
              <span class="file-status">
                {ready ? 'Open' : m.file.status === 'downloading' ? 'Downloading…' : m.file.status === 'failed' ? 'Download failed — retry' : 'Download'}
              </span>
            </span>
          </button>
        {/if}
      {/if}

      {#if m.isDeleted}
        <span class="text deleted-text">Message deleted</span>
      {:else if m.text}
        <!-- eslint-disable-next-line svelte/no-at-html-tags: renderText escapes all input -->
        <span class="text msg-text" onclick={onTextClick} role="presentation">{@html html}</span>
      {/if}

      <span class="meta">
        {#if m.isPinned}<Pin size={11} />{/if}
        {#if m.isEdited && !m.isDeleted}<span>edited</span>{/if}
        <span>{timeOfDay(m.time)}</span>
        {#if own && !m.isDeleted}
          <span class="ticks" class:read={m.isRead} title={m.isRead ? 'Read' : 'Sent'} aria-label={m.isRead ? 'Read' : 'Sent'}>
            {#if m.isRead}<CheckCheck size={14} />{:else}<Check size={14} />{/if}
          </span>
        {/if}
      </span>
    </div>

    {#if reactions.length}
      <div class="reactions">
        {#each reactions as [emoji, n]}
          <button class="chip" class:mine={m.reactions.own === emoji} title={reactionTitle(emoji)} onclick={() => react(emoji)}>
            <span>{emoji}</span><span class="n">{n}</span>
          </button>
        {/each}
      </div>
    {/if}

    {#if !m.isDeleted}
      <div class="actions" role="toolbar" aria-label="Message actions">
        {#each QUICK as emoji}
          <button class="quick" class:mine={m.reactions.own === emoji} onclick={() => react(emoji)} title="React {emoji}">{emoji}</button>
        {/each}
        <button class="icon-btn sm" bind:this={reactBtn} onclick={() => onReact(m, reactBtn!)} title="More reactions" aria-label="More reactions"><SmilePlus size={17} /></button>
        <button class="icon-btn sm" onclick={reply} title="Reply" aria-label="Reply"><Reply size={17} /></button>
        {#if editState.show}
          <button class="icon-btn sm" onclick={edit} disabled={!editState.ok} title={editState.ok ? 'Edit' : editState.reason} aria-label="Edit"><Pencil size={16} /></button>
        {/if}
        <button class="icon-btn sm" bind:this={moreBtn} title="More" aria-label="More"
          onclick={() => { const r = moreBtn!.getBoundingClientRect(); ui.openMenu({ x: r.left, y: r.bottom + 4 }, menuItems()); }}>
          <MoreHorizontal size={17} />
        </button>
      </div>
    {/if}
  </div>
</div>

<style>
  .msg { display: flex; gap: 8px; align-items: flex-end; justify-content: flex-start; margin-top: 2px; position: relative; padding: 2px 4px; border-radius: 10px; }
  .msg.first { margin-top: 10px; }
  .msg.own { justify-content: flex-end; }
  .msg.highlight { background: var(--cursor); transition: background var(--duration-slow) var(--ease); }
  .avatar-space { width: 30px; flex-shrink: 0; }
  .col { max-width: min(440px, 78%); min-width: 0; display: flex; flex-direction: column; align-items: flex-start; position: relative; }
  .own .col { align-items: flex-end; }

  .bubble {
    background: var(--other-bubble); color: var(--other-text); padding: 7px 11px; font-size: 14px; line-height: 20px; max-width: 100%;
    overflow-wrap: anywhere; border-radius: 6px 16px 16px 6px; user-select: text; -webkit-user-select: text;
  }
  /* tail corner (bottom, sender side) and inner corners of a group are 6 px */
  .first .bubble { border-top-left-radius: 16px; }
  .own .bubble { background: var(--own-bubble); color: var(--own-text); border-radius: 16px 6px 6px 16px; }
  .own.first .bubble { border-top-right-radius: 16px; }
  .bubble.mention { background: var(--mention); }
  .bubble.deleted { background: transparent; border: 1px dashed var(--border-strong); color: var(--text-muted); }
  .bubble.emoji { background: transparent; padding: 0 2px; font-size: 34px; line-height: 40px; }
  .bubble.emoji .meta { font-size: 11px; line-height: 14px; }
  .bubble.has-image { padding: 4px 4px 7px; }
  .bubble.has-image .text, .bubble.has-image .sender, .bubble.has-image .quote { margin-left: 7px; margin-right: 7px; }

  .sender { font-size: 13px; font-weight: 650; margin-bottom: 2px; }
  .deleted-text { font-style: italic; }

  .quote {
    display: flex; flex-direction: column; text-align: left; width: 100%; border: 0; border-left: 3px solid var(--q); border-radius: 6px;
    background: rgba(0, 0, 0, 0.05); padding: 5px 8px; margin-bottom: 6px; min-width: 0;
  }
  :global(.t-dark) .quote { background: rgba(255, 255, 255, 0.06); }
  .q-name { font-size: 12px; font-weight: 650; color: var(--q); }
  .q-text { font-size: 13px; color: var(--text-2); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 360px; }

  .image { border: 0; padding: 0; background: none; display: block; margin-bottom: 4px; }
  .video { display: block; max-width: 100%; max-height: 360px; border-radius: 12px; margin-bottom: 4px; background: #000; }
  .image img { display: block; max-width: 100%; max-height: 320px; border-radius: 12px; min-width: 120px; object-fit: cover; }

  .file {
    display: flex; align-items: center; gap: 10px; border: 1px solid var(--border); background: var(--surface); border-radius: 10px;
    padding: 8px 12px 8px 8px; margin-bottom: 4px; min-width: 220px; max-width: 100%; text-align: left;
  }
  .file-ico { width: 38px; height: 38px; border-radius: 8px; background: var(--accent-soft); color: var(--accent); display: flex; align-items: center; justify-content: center; flex-shrink: 0; }
  .file-meta { display: flex; flex-direction: column; min-width: 0; }
  .file-name { font-size: 14px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: var(--text); }
  .file-status { font-size: 12px; color: var(--text-muted); }

  .meta {
    display: inline-flex; align-items: center; gap: 4px; font-size: 11px; color: var(--text-muted); float: right;
    margin: 6px 0 -4px 10px; line-height: 14px; user-select: none;
  }
  .own .meta { color: var(--own-meta); }
  .ticks { display: flex; }
  .ticks.read { color: var(--accent); }

  .reactions { display: flex; gap: 4px; flex-wrap: wrap; margin-top: 4px; }
  .chip {
    display: inline-flex; align-items: center; gap: 4px; height: 24px; padding: 0 8px; border-radius: 999px; background: var(--surface);
    border: 1px solid var(--border); font-size: 13px; color: var(--text);
  }
  .chip.mine { background: var(--accent-soft); border-color: var(--accent); }
  .chip .n { font-size: 12px; font-weight: 600; color: var(--text-2); }

  .actions {
    position: absolute; top: -34px; left: 0; display: none; align-items: center; gap: 2px; padding: 3px 4px; background: var(--surface);
    border: 1px solid var(--border); border-radius: 12px; box-shadow: var(--shadow-2); z-index: 5; white-space: nowrap;
  }
  .own .actions { left: auto; right: 0; }
  .msg:hover .actions, .actions:focus-within { display: flex; }
  .quick { width: 28px; height: 28px; border: 0; background: none; border-radius: 8px; font-size: 16px; padding: 0; }
  .quick:hover { background: var(--hover); }
  .quick.mine { background: var(--accent-soft); }
</style>
