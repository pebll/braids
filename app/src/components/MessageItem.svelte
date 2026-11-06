<!--
  MessageItem.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  One message: bubble (own right, others left), sender name and avatar in
  groups, quote, attachment, reactions, hover actions.
-->
<script lang="ts">
  import { Check, CheckCheck, Clock, Copy, Download, FileText, Forward, Image, LoaderCircle, Pencil, Pin, PinOff, Play, Reply, RotateCw, SmilePlus, Trash2, TriangleAlert } from '@lucide/svelte';
  import { copyText, openLink, personMenu } from '../lib/actions';
  import * as api from '../lib/api';
  import { mediaSrc } from '../lib/media';
  import { firstLink, isOnlyEmoji, renderText, senderColor, timeOfDay } from '../lib/format';
  import { settings } from '../lib/settings.svelte';
  import { chatKey, fileMime, fileReady, galleryItems, store } from '../lib/store.svelte';
  import type { Account, Chat, Message } from '../lib/types';
  import { openLightbox, ui, type MenuItem } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';
  import LinkPreview from './LinkPreview.svelte';
  import Sticker from './Sticker.svelte';
  import VoicePlayer from './VoicePlayer.svelte';

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

  const own = $derived(m.isOutgoing);
  const showName = $derived(chat.isGroup && !own && first);
  const showAvatar = $derived(chat.isGroup && !own);
  const mime = $derived(fileMime(m.file));
  const isImage = $derived(mime.startsWith('image/'));
  const isVideo = $derived(mime.startsWith('video/'));
  const isAudio = $derived(mime.startsWith('audio/'));
  const isSticker = $derived(m.file?.kind === 'sticker' && !m.isDeleted);
  const isGif = $derived(m.file?.kind === 'gif');
  // a sticker's text is its emoji (or a placeholder): shown as the sticker's label only
  const stickerEmoji = $derived(isSticker && (isOnlyEmoji(m.text) || /^\[sticker\]$/i.test(m.text.trim())) ? m.text.trim() : '');
  let videoFailed = $state(false); // no preview: shown as a file to open externally
  const ready = $derived(fileReady(m.file));
  const emojiOnly = $derived(!m.file && !m.quoted && !m.isDeleted && isOnlyEmoji(m.text));
  const showText = $derived(!!m.text && !(isSticker && stickerEmoji));
  const previewUrl = $derived(settings.linkPreviews && !m.isDeleted && !m.file && !m.pending ? firstLink(m.text) : '');
  const html = $derived(m.isDeleted ? '' : renderText(m.text, m.mentions, account.selfId));
  const reactions = $derived(Object.entries(m.reactions.counts).filter(([, n]) => n > 0));
  const mentionsMe = $derived(!own && (m.hasMention || Object.values(m.mentions).includes(account.selfId)));
  let bubbleHeight = $state(0);
  let reactBtn: HTMLButtonElement | undefined = $state();
  let requestedDownload = false;
  let saveWhenReady = $state(false);

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

  // images, videos and voice messages are fetched right away, other files on request
  $effect(() => {
    if ((isImage || isVideo || isAudio || isSticker) && m.file?.status === 'notDownloaded' && !requestedDownload) {
      requestedDownload = true;
      download();
    }
  });

  function download() {
    api.call('files.download', { accountId: m.accountId, chatId: m.chatId, msgId: m.id }).catch((e) => store.toastError(e));
  }

  // "Download file": a copy where the user picks (fetched first if needed)
  async function saveFile() {
    if (!m.file) return;
    if (!ready) {
      saveWhenReady = true;
      if (m.file.status !== 'downloading') download();
      return;
    }
    try {
      const saved = await api.saveFileAs(m.file.path, m.file.name);
      if (saved) store.toast(`Saved to ${saved}`, 'info', { label: 'Open', run: () => api.openPath(saved).catch((e) => store.toastError(e)) });
    } catch (e) {
      store.toastError(e);
    }
  }

  $effect(() => {
    if (saveWhenReady && ready) {
      saveWhenReady = false;
      saveFile();
    } else if (saveWhenReady && m.file?.status === 'failed') {
      saveWhenReady = false;
      store.toast('The file could not be downloaded.', 'error');
    }
  });

  function openFile() {
    if (!m.file) return;
    if (ready) {
      if (isImage || (isVideo && !videoFailed)) {
        // switch between the loaded images and videos of the chat
        openLightbox(galleryItems(store.messages[chatKey(m.accountId, m.chatId)]?.items ?? []), m.file.path);
      }
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

  // ---- outbox: not yet sent ----
  const pending = $derived(m.pending ?? '');

  function resend() {
    api.call('messages.resend', { accountId: m.accountId, chatId: m.chatId, msgId: m.id }).catch((e) => store.toastError(e));
  }

  function discard() {
    api.call('messages.delete', { accountId: m.accountId, chatId: m.chatId, msgId: m.id }).catch((e) => store.toastError(e));
  }

  async function remove() {
    if (m.isDeleted) {
      await api.call('messages.delete', { accountId: m.accountId, chatId: m.chatId, msgId: m.id }).catch((e) => store.toastError(e));
      return;
    }
    const ok = await ui.confirm('Delete message?', 'The message is deleted for everyone in this chat.', 'Delete for everyone', true);
    if (ok) await api.call('messages.delete', { accountId: m.accountId, chatId: m.chatId, msgId: m.id }).catch((e) => store.toastError(e));
  }

  function pinItem(): MenuItem {
    return {
      label: m.isPinned ? 'Unpin message' : 'Pin message',
      icon: m.isPinned ? PinOff : Pin,
      run: () => api.call('messages.pin', { accountId: m.accountId, chatId: m.chatId, msgId: m.id, isPinned: !m.isPinned }).catch((e) => store.toastError(e)),
    };
  }

  // actions after react and reply, in the order the hover bar adds them
  function extraItems(): MenuItem[] {
    const items: MenuItem[] = [];
    if (m.text) items.push({ label: 'Copy text', icon: Copy, run: () => copyText(m.text, 'Copied') });
    if (m.file && isImage && !isSticker && ready) items.push({ label: 'Copy image', icon: Image, run: () => api.copyImage(m.file!.path).then(() => store.toast('Copied')).catch((e) => store.toastError(e)) });
    if (m.file) items.push({ label: 'Download file', icon: Download, run: saveFile });
    items.push({ label: 'Forward…', icon: Forward, run: () => (ui.forward = m) });
    if (account.capabilities.pinMessage) items.push(pinItem());
    if (editState.ok) items.push({ label: 'Edit', icon: Pencil, run: edit });
    if (own) items.push({ label: 'Delete for everyone', icon: Trash2, danger: true, run: remove });
    return items;
  }

  // hover bar sized to the bubble: a one-line message gets react and reply
  // side by side, two lines the same stacked, each further line one more
  // action (line: 20 px, bubble padding: 14 px)
  const lines = $derived(Math.max(1, Math.round((bubbleHeight - 14) / 20)));
  const barItems = $derived(lines <= 2 ? [] : extraItems().slice(0, lines - 2));

  // right-click menu: everything
  export function menuItems(): MenuItem[] {
    if (m.isDeleted) return [{ label: 'Remove', icon: Trash2, danger: true, run: remove }];
    if (pending) {
      const items: MenuItem[] = [];
      if (pending === 'failed') items.push({ label: 'Send again', icon: RotateCw, run: resend });
      if (m.text) items.push({ label: 'Copy text', icon: Copy, run: () => copyText(m.text, 'Copied') });
      items.push({ label: pending === 'failed' ? 'Delete' : 'Cancel sending', icon: Trash2, danger: true, run: discard });
      return items;
    }
    const items: MenuItem[] = [{ label: 'Reply', icon: Reply, run: reply }];
    if (editState.show) items.push({ label: 'Edit', icon: Pencil, run: edit, disabled: !editState.ok, hint: editState.reason });
    if (m.text) items.push({ label: 'Copy text', icon: Copy, run: () => copyText(m.text, 'Copied') });
    if (m.file && isImage && !isSticker && ready) items.push({ label: 'Copy image', icon: Image, run: () => api.copyImage(m.file!.path).then(() => store.toast('Copied')).catch((e) => store.toastError(e)) });
    if (m.file) items.push({ label: 'Download file', icon: Download, run: saveFile });
    items.push({ label: 'Forward…', icon: Forward, run: () => (ui.forward = m) });
    if (account.capabilities.pinMessage) items.push(pinItem());
    if (own) items.push({ label: 'Delete for everyone', icon: Trash2, danger: true, run: remove });
    return items;
  }

  // a click on the avatar or name of a sender in a group
  function openPerson(e: MouseEvent) {
    e.stopPropagation();
    const items = personMenu(m.accountId, m.senderId, store.senderName(m));
    if (items.length) ui.openMenu(e, items);
  }

  function showMenu(e: MouseEvent) {
    e.preventDefault();
    ui.openMenu(e, menuItems());
  }

  function onTextClick(e: MouseEvent) {
    const a = (e.target as HTMLElement).closest('a[data-link]') as HTMLAnchorElement | null;
    if (a) {
      e.preventDefault();
      openLink(a.href);
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
  class:pending={!!pending}
  class:failed={pending === 'failed'}
  data-msg-id={m.id}
  oncontextmenu={showMenu}
  onmouseenter={() => (ui.hoverMessage = m)}
  onmouseleave={() => ui.hoverMessage === m && (ui.hoverMessage = null)}
  role="article"
  aria-label={`${store.senderName(m)}, ${timeOfDay(m.time)}`}
>
  {#if showAvatar}
    {#if last}
      <button class="who" aria-label={`${store.senderName(m)}: options`} onclick={(e) => openPerson(e)}>
        <Avatar name={store.senderName(m)} id={m.senderId} size={30} owner={m.accountId} />
      </button>
    {:else}
      <span class="avatar-space"></span>
    {/if}
  {/if}

  <div class="col">
    <div
      class="bubble"
      bind:clientHeight={bubbleHeight}
      class:emoji={emojiOnly}
      class:mention={mentionsMe}
      class:deleted={m.isDeleted}
      class:has-image={(isImage || isVideo) && ready}
      class:sticker={isSticker}
    >
      {#if showName}
        <button class="sender" style:color={senderColor(m.senderId)} onclick={(e) => openPerson(e)}>{store.senderName(m)}</button>
      {/if}

      {#if m.quoted}
        <button class="quote" onclick={() => onJump(m.quoted!.id)} style:--q={senderColor(m.quoted.senderId)}>
          <span class="q-name">{m.quoted.senderId === account.selfId ? 'You' : m.quoted.senderName || m.quoted.senderId}</span>
          <span class="q-text">{store.quotedText(m)}</span>
        </button>
      {/if}

      {#if m.file && !m.isDeleted}
        {#if isSticker}
          <Sticker file={m.file} emoji={stickerEmoji} />
        {:else if isGif && isVideo && ready && !videoFailed}
          <!-- a GIF (a video without sound): plays muted and looping in the chat -->
          <button class="image gif" onclick={openFile} aria-label="Open GIF {m.file.name}">
            <!-- svelte-ignore a11y_media_has_caption -->
            <video use:mediaSrc={{ path: m.file.path, type: m.file.mimeType, lazy: true }} autoplay loop muted playsinline onerror={() => (videoFailed = true)}></video>
            <span class="gif-badge">GIF</span>
          </button>
        {:else if isImage && ready}
          <button class="image" class:gif={isGif} onclick={openFile} aria-label="Open image {m.file.name}">
            <img src={api.fileUrl(m.file.path)} alt={m.file.name} loading="lazy" />
            {#if isGif}<span class="gif-badge">GIF</span>{/if}
          </button>
        {:else if isAudio}
          <VoicePlayer path={ready ? m.file.path : ''} id={m.id} status={m.file.status} onDownload={download} />
        {:else if isVideo && ready && !videoFailed}
          <!-- the first frame; plays in the viewer -->
          <button class="image video-thumb" onclick={openFile} aria-label="Play video {m.file.name}">
            <!-- svelte-ignore a11y_media_has_caption -->
            <video use:mediaSrc={{ path: m.file.path, type: m.file.mimeType, fragment: '#t=0.1', lazy: true }} preload="metadata" muted onerror={() => (videoFailed = true)}></video>
            <span class="play-mark"><Play size={22} fill="currentColor" /></span>
          </button>
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
      {:else if showText}
        <!-- eslint-disable-next-line svelte/no-at-html-tags: renderText escapes all input -->
        <span class="text msg-text" onclick={onTextClick} role="presentation">{@html html}</span>
      {/if}
      {#if previewUrl}<LinkPreview url={previewUrl} />{/if}

      <span class="meta">
        {#if m.isPinned}<Pin size={11} />{/if}
        {#if m.isEdited && !m.isDeleted}<span>edited</span>{/if}
        <span>{timeOfDay(m.time)}</span>
        {#if pending === 'sending'}
          <span class="ticks" title="Sending…" aria-label="Sending"><LoaderCircle size={13} class="spin" /></span>
        {:else if pending === 'waiting'}
          <span class="ticks" title="Waiting for the connection; sent when the account is back online" aria-label="Waiting for the connection"><Clock size={13} /></span>
        {:else if pending === 'failed'}
          <span class="ticks failed-mark" title="Not sent" aria-label="Not sent"><TriangleAlert size={13} /></span>
        {:else if own && !m.isDeleted}
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

    {#if pending === 'failed'}
      <div class="not-sent" role="alert">
        <TriangleAlert size={13} />Not sent
        <button class="link-btn" onclick={resend}>Send again</button>
        <button class="link-btn" onclick={discard}>Delete</button>
      </div>
    {/if}

    {#if !m.isDeleted && !pending}
      <div class="actions" class:row={lines <= 1} role="toolbar" aria-label="Message actions" aria-orientation={lines <= 1 ? 'horizontal' : 'vertical'}>
        <button class="icon-btn sm" bind:this={reactBtn} onclick={() => onReact(m, reactBtn!)} title="React" aria-label="React"><SmilePlus size={17} /></button>
        <button class="icon-btn sm" onclick={reply} title="Reply" aria-label="Reply"><Reply size={17} /></button>
        {#each barItems as item (item.label)}
          <button class="icon-btn sm" class:danger={item.danger} onclick={item.run} title={item.label} aria-label={item.label}><item.icon size={16} /></button>
        {/each}
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

  .sender { font-size: 13px; font-weight: 650; margin-bottom: 2px; display: block; border: 0; background: none; padding: 0; text-align: left; font-family: inherit; cursor: pointer; }
  .sender:hover { text-decoration: underline; }
  .who { border: 0; background: none; padding: 0; border-radius: 50%; cursor: pointer; align-self: flex-end; }
  .deleted-text { font-style: italic; }

  .quote {
    display: flex; flex-direction: column; text-align: left; width: 100%; border: 0; border-left: 3px solid var(--q); border-radius: 6px;
    background: rgba(0, 0, 0, 0.05); padding: 5px 8px; margin-bottom: 6px; min-width: 0;
  }
  :global(.t-dark) .quote { background: rgba(255, 255, 255, 0.06); }
  .q-name { font-size: 12px; font-weight: 650; color: var(--q); }
  .q-text { font-size: 13px; color: var(--text-2); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 360px; }

  .image { border: 0; padding: 0; background: none; display: block; margin-bottom: 4px; }
  .video-thumb { position: relative; }
  .video-thumb video { display: block; max-width: 100%; max-height: 360px; min-width: 160px; min-height: 100px; border-radius: 12px; background: #000; }
  .play-mark {
    position: absolute; left: 50%; top: 50%; transform: translate(-50%, -50%); width: 48px; height: 48px; border-radius: 24px;
    display: flex; align-items: center; justify-content: center; background: rgb(0 0 0 / 0.55); color: #fff; pointer-events: none;
  }
  .video-thumb:hover .play-mark { background: rgb(0 0 0 / 0.7); }
  .image img { display: block; max-width: 100%; max-height: 320px; border-radius: 12px; min-width: 120px; object-fit: cover; }
  .gif { position: relative; }
  .gif video { display: block; max-width: 100%; max-height: 320px; min-width: 120px; border-radius: 12px; background: #000; }
  .gif-badge {
    position: absolute; left: 8px; top: 8px; padding: 1px 6px; border-radius: 6px; font-size: 11px; font-weight: 700; letter-spacing: 0.04em;
    background: rgb(0 0 0 / 0.55); color: #fff; pointer-events: none;
  }
  /* stickers stand on their own, like big emoji */
  .bubble.sticker { background: transparent; box-shadow: none; padding: 0; }
  .bubble.sticker .meta { align-self: flex-end; padding: 1px 6px; border-radius: 8px; background: color-mix(in srgb, var(--surface) 80%, transparent); }

  .file {
    display: flex; align-items: center; gap: 10px; border: 1px solid var(--border); background: var(--surface); border-radius: 10px;
    padding: 8px 12px 8px 8px; margin-bottom: 4px; min-width: 220px; max-width: 100%; text-align: left;
  }
  .file-ico { width: 38px; height: 38px; border-radius: 8px; background: var(--accent-soft); color: var(--accent); display: flex; align-items: center; justify-content: center; flex-shrink: 0; }
  .file-meta { display: flex; flex-direction: column; min-width: 0; }
  .file-name { font-size: 14px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: var(--text); }
  .file-status { font-size: 12px; color: var(--text-muted); }

  /* margins instead of gap: WebKitGTK leaves gap out of the float's width,
     the ticks then stick out into the bubble padding */
  .meta {
    display: inline-flex; align-items: center; font-size: 11px; color: var(--text-muted); float: right; white-space: nowrap;
    margin: 6px 0 -4px 10px; line-height: 14px; user-select: none;
  }
  .meta > :global(* + *) { margin-left: 4px; }
  .own .meta { color: var(--own-meta); }
  .ticks { display: flex; flex-shrink: 0; }
  .ticks.read { color: var(--accent); }

  .reactions { display: flex; gap: 4px; flex-wrap: wrap; margin-top: 4px; }
  .chip {
    display: inline-flex; align-items: center; gap: 4px; height: 24px; padding: 0 8px; border-radius: 999px; background: var(--surface);
    border: 1px solid var(--border); font-size: 13px; color: var(--text);
  }
  .chip.mine { background: var(--accent-soft); border-color: var(--accent); }
  .chip .n { font-size: 12px; font-weight: 600; color: var(--text-2); }

  /* hover bar: vertical, beside the bubble (right of others' messages, left of own) */
  .actions {
    position: absolute; bottom: 0; left: calc(100% + 6px); display: none; flex-direction: column; align-items: center; gap: 2px; padding: 3px;
    background: var(--surface); border: 1px solid var(--border); border-radius: 12px; box-shadow: var(--shadow-2); z-index: 5;
  }
  .own .actions { left: auto; right: calc(100% + 6px); }
  .msg:hover .actions, .actions:focus-within { display: flex; }
  .actions.row { flex-direction: row; }
  .actions .danger:hover { color: var(--danger); }
  .msg.pending .bubble { opacity: 0.6; }
  .msg.failed .bubble { opacity: 0.75; box-shadow: inset 0 0 0 1px var(--danger); }
  .failed-mark { color: var(--danger); }
  .not-sent { display: flex; align-items: center; gap: 6px; justify-content: flex-end; font-size: 12px; color: var(--danger); margin-top: 3px; }
  .link-btn { border: 0; background: none; padding: 0 2px; color: var(--accent); font-size: 12px; font-weight: 600; cursor: pointer; font-family: inherit; }
  .link-btn:hover { text-decoration: underline; }
</style>
