<!--
  Composer.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Message input: reply / edit banner, attachments (picker, paste, drop),
  emoji, Enter to send. Drafts are kept per chat.
-->
<script lang="ts">
  import { File as FileIcon, Paperclip, Pencil, Reply, SendHorizontal, Smile, X } from '@lucide/svelte';
  import { tick } from 'svelte';
  import * as api from '../lib/api';
  import { editLastOwnMessage } from '../lib/actions';
  import { senderColor } from '../lib/format';
  import { settings } from '../lib/settings.svelte';
  import { splitKey, store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';

  let { chatKey }: { chatKey: string } = $props();

  const EMOJI = ['😀', '😂', '🙂', '😉', '😍', '🥰', '😘', '😎', '🤔', '😅', '😭', '😢', '😮', '😡', '🙄', '😴', '👍', '👎', '👏', '🙏', '💪', '👋', '🤝', '✌️', '❤️', '🔥', '🎉', '✨', '⭐', '✅', '❌', '👀', '💯', '🤷', '🤦', '🙌', '☕', '🍕', '🍻', '🚀'];

  let text = $state('');
  let attachments = $state<string[]>([]);
  let textarea: HTMLTextAreaElement | undefined = $state();
  let emojiOpen = $state(false);
  let sending = $state(false);
  let lastTypingSent = 0;
  let typingStopTimer: ReturnType<typeof setTimeout> | undefined;
  let loadedKey = '';

  const [accountId, chatId] = $derived(splitKey(chatKey));
  const account = $derived(store.accounts[accountId]);

  // load / save the draft when the chat changes
  $effect(() => {
    const key = chatKey;
    if (key !== loadedKey) {
      if (loadedKey) saveDraft(loadedKey);
      loadedKey = key;
      text = store.drafts[key] ?? '';
      attachments = [];
      ui.reply = null;
      ui.edit = null;
      tick().then(resize);
    }
  });

  $effect(() => {
    if (ui.focusComposer) tick().then(() => textarea?.focus());
  });

  // entering edit mode puts the message text into the composer
  $effect(() => {
    const m = ui.edit;
    if (m) {
      text = m.text;
      tick().then(() => {
        resize();
        textarea?.focus();
        textarea?.setSelectionRange(text.length, text.length);
      });
    }
  });

  function saveDraft(key: string) {
    const t = ui.edit ? '' : text.trim();
    if (t) store.drafts[key] = text;
    else delete store.drafts[key];
  }

  function resize() {
    if (!textarea) return;
    textarea.style.height = 'auto';
    textarea.style.height = `${Math.min(textarea.scrollHeight, 200)}px`;
  }

  function sendTyping(isTyping: boolean) {
    api.call('typing.send', { accountId, chatId, isTyping }).catch(() => {});
  }

  function onInput() {
    resize();
    if (ui.edit) return;
    const now = Date.now();
    if (text && now - lastTypingSent > 4000) {
      lastTypingSent = now;
      sendTyping(true);
    }
    clearTimeout(typingStopTimer);
    typingStopTimer = setTimeout(() => {
      if (lastTypingSent) {
        lastTypingSent = 0;
        sendTyping(false);
      }
    }, 5000);
  }

  async function submit() {
    const body = text.trim();
    if (sending || (!body && !attachments.length)) return;
    sending = true;
    try {
      if (ui.edit) {
        if (body && body !== ui.edit.text) {
          await api.call('messages.edit', { accountId, chatId, msgId: ui.edit.id, text: body });
        }
        ui.edit = null;
      } else {
        const quotedId = ui.reply?.id;
        if (attachments.length) {
          // the text goes with the first file
          for (let i = 0; i < attachments.length; i++) {
            await api.call('messages.send', { accountId, chatId, filePath: attachments[i], ...(i === 0 && body ? { text: body } : {}), ...(i === 0 && quotedId ? { quotedId } : {}) });
          }
        } else {
          await api.call('messages.send', { accountId, chatId, text: body, ...(quotedId ? { quotedId } : {}) });
        }
        ui.reply = null;
      }
      text = '';
      attachments = [];
      delete store.drafts[chatKey];
      clearTimeout(typingStopTimer);
      if (lastTypingSent) {
        lastTypingSent = 0;
        sendTyping(false);
      }
      await tick();
      resize();
    } catch (e) {
      store.toastError(e);
    } finally {
      sending = false;
      textarea?.focus();
    }
  }

  function onKeydown(e: KeyboardEvent) {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing && (settings.enterSends || e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      submit();
    } else if (e.key === 'Escape') {
      if (emojiOpen) emojiOpen = false;
      else if (ui.edit) {
        ui.edit = null;
        text = '';
        tick().then(resize);
      } else if (ui.reply) ui.reply = null;
      else textarea?.blur();
    } else if (e.key === 'ArrowUp' && !text && !attachments.length) {
      if (editLastOwnMessage()) e.preventDefault();
    }
  }

  async function onPaste(e: ClipboardEvent) {
    const files = Array.from(e.clipboardData?.files ?? []);
    if (!files.length) return;
    e.preventDefault();
    try {
      for (const f of files) {
        const ext = f.type.split('/')[1] ?? 'bin';
        attachments.push(await api.saveTempFile(f.name || `pasted.${ext}`, f));
      }
    } catch (err) {
      store.toastError(err);
    }
  }

  async function pick() {
    try {
      attachments.push(...(await api.pickFiles()));
      textarea?.focus();
    } catch (e) {
      store.toastError(e);
    }
  }

  export function addFiles(paths: string[]) {
    attachments.push(...paths);
    textarea?.focus();
  }

  function insertEmoji(emoji: string) {
    const el = textarea;
    if (!el) return;
    const start = el.selectionStart ?? text.length;
    const end = el.selectionEnd ?? text.length;
    text = text.slice(0, start) + emoji + text.slice(end);
    emojiOpen = false;
    tick().then(() => {
      el.focus();
      el.setSelectionRange(start + emoji.length, start + emoji.length);
      resize();
    });
  }

  function baseName(path: string): string {
    return path.split(/[\\/]/).pop() ?? path;
  }
</script>

<div class="composer">
  {#if ui.reply || ui.edit}
    {@const m = (ui.edit ?? ui.reply)!}
    <div class="context">
      <span class="ctx-ico">{#if ui.edit}<Pencil size={18} />{:else}<Reply size={18} />{/if}</span>
      <div class="ctx-body" style:--c={ui.edit ? 'var(--accent)' : senderColor(m.senderId)}>
        <div class="ctx-title">{ui.edit ? 'Editing message' : `Replying to ${store.senderName(m)}`}</div>
        <div class="ctx-text">{store.previewText(m)}</div>
      </div>
      <button class="icon-btn sm" aria-label={ui.edit ? 'Cancel edit' : 'Cancel reply'} title="Cancel (Esc)"
        onclick={() => { if (ui.edit) { ui.edit = null; text = ''; } ui.reply = null; textarea?.focus(); }}>
        <X size={17} />
      </button>
    </div>
  {/if}

  {#if attachments.length}
    <div class="attachments">
      {#each attachments as path, i}
        <span class="att">
          <FileIcon size={14} />
          <span class="att-name">{baseName(path)}</span>
          <button class="icon-btn sm" aria-label="Remove attachment" onclick={() => attachments.splice(i, 1)}><X size={14} /></button>
        </span>
      {/each}
    </div>
  {/if}

  <div class="row">
    <button class="icon-btn lg" aria-label="Attach file" title="Attach file" onclick={pick} disabled={!!ui.edit}><Paperclip size={20} strokeWidth={1.75} /></button>
    <div class="box">
      <textarea
        bind:this={textarea}
        bind:value={text}
        rows="1"
        placeholder={account?.state === 'online' ? 'Message' : 'Message (sent when the account is back online)'}
        aria-label="Message"
        oninput={onInput}
        onkeydown={onKeydown}
        onpaste={onPaste}
      ></textarea>
      <button class="icon-btn sm" aria-label="Emoji" title="Emoji" onclick={() => (emojiOpen = !emojiOpen)}><Smile size={19} strokeWidth={1.75} /></button>
      {#if emojiOpen}
        <div class="emoji-pop" role="dialog" aria-label="Emoji">
          {#each EMOJI as e}
            <button onclick={() => insertEmoji(e)}>{e}</button>
          {/each}
        </div>
      {/if}
    </div>
    <button class="send" aria-label={ui.edit ? 'Save edit' : 'Send'} title={ui.edit ? 'Save (Enter)' : 'Send (Enter)'} onclick={submit}
      disabled={sending || (!text.trim() && !attachments.length)}>
      <SendHorizontal size={19} strokeWidth={1.9} />
    </button>
  </div>
</div>

<style>
  .composer { border-top: 1px solid var(--border); background: var(--bg-2); flex-shrink: 0; }
  .context { display: flex; align-items: center; gap: 10px; padding: 8px 12px 0; }
  .ctx-ico { color: var(--accent); display: flex; }
  .ctx-body { flex: 1; min-width: 0; border-left: 3px solid var(--c); padding-left: 8px; }
  .ctx-title { font-size: 12px; font-weight: 650; color: var(--c); }
  .ctx-text { font-size: 13px; color: var(--text-2); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .attachments { display: flex; flex-wrap: wrap; gap: 6px; padding: 8px 12px 0 56px; }
  .att { display: inline-flex; align-items: center; gap: 6px; height: 30px; padding: 0 2px 0 10px; border-radius: 8px; background: var(--surface-2); border: 1px solid var(--border); font-size: 13px; max-width: 260px; }
  .att-name { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .row { display: flex; align-items: flex-end; gap: 6px; padding: 10px 12px 12px; }
  .box {
    flex: 1; min-width: 0; min-height: 40px; display: flex; align-items: flex-end; gap: 6px; padding: 0 5px 0 14px; border-radius: 12px;
    background: var(--surface-2); border: 1px solid var(--border-strong); position: relative;
  }
  .box:focus-within { border-color: var(--accent); }
  .box .icon-btn { margin-bottom: 5px; }
  textarea {
    flex: 1; min-width: 0; border: 0; outline: none; background: transparent; resize: none; font-size: 14px; line-height: 20px;
    padding: 9px 0; max-height: 200px; color: var(--text);
  }
  textarea::placeholder { color: var(--text-muted); }
  .send {
    width: 40px; height: 40px; border-radius: 10px; border: 0; background: var(--accent); color: var(--on-accent);
    display: flex; align-items: center; justify-content: center; flex-shrink: 0;
  }
  .send:hover:not(:disabled) { background: var(--accent-hover); }
  .send:disabled { background: var(--border-strong); color: var(--surface); }
  .emoji-pop {
    position: absolute; bottom: 46px; right: 0; width: 296px; padding: 8px; display: grid; grid-template-columns: repeat(8, 1fr); gap: 2px;
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius-lg); box-shadow: var(--shadow-2); z-index: 10;
  }
  .emoji-pop button { border: 0; background: none; height: 32px; border-radius: 6px; font-size: 18px; padding: 0; }
  .emoji-pop button:hover { background: var(--hover); }
</style>
