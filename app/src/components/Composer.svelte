<!--
  Composer.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Message input: reply / edit banner, attachments (picker, paste, drop),
  emoji, Enter to send. Drafts are kept per chat.
-->
<script lang="ts">
  import { ChartColumnBig, File as FileIcon, LoaderCircle, Mic, Paperclip, Pencil, Play, Reply, SendHorizontal, Smile, Trash2, X } from '@lucide/svelte';
  import { onDestroy, tick } from 'svelte';
  import * as api from '../lib/api';
  import { mediaSrc } from '../lib/media';
  import { editLastOwnMessage } from '../lib/actions';
  import { senderColor } from '../lib/format';
  import { settings } from '../lib/settings.svelte';
  import { splitKey, store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';
  import Avatar from './Avatar.svelte';
  import EmojiPicker from './EmojiPicker.svelte';
  import { loadEmoji, searchEmoji, type Emoji } from '../lib/emoji';

  let { chatKey }: { chatKey: string } = $props();


  let text = $state('');
  let attachments = $state<string[]>([]);
  let broken = $state<Record<string, boolean>>({}); // previews that could not be shown

  // photos and videos are previewed as small tiles, other files as chips
  function previewKind(path: string): 'image' | 'video' | '' {
    const ext = path.split('.').pop()?.toLowerCase() ?? '';
    if (broken[path]) return '';
    if (['jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp', 'avif', 'jfif'].includes(ext)) return 'image';
    if (['mp4', 'm4v', 'webm', 'mov', 'mkv', 'ogv'].includes(ext)) return 'video';
    return '';
  }
  let textarea: HTMLTextAreaElement | undefined = $state();
  let emojiOpen = $state(false);
  let sending = $state(false);
  let lastTypingSent = 0;
  let typingStopTimer: ReturnType<typeof setTimeout> | undefined;
  let loadedKey = '';

  const [accountId, chatId] = $derived(splitKey(chatKey));
  const canPoll = $derived.by(() => {
    const caps = store.accounts[accountId]?.capabilities;
    return !!caps?.polls && (!caps.pollsInGroupsOnly || !!store.chats[chatKey]?.isGroup);
  });
  const account = $derived(store.accounts[accountId]);

  // @-mentions: while typing "@na" after a space or at the start, the group's
  // members matching it are suggested; the pick is inserted as @Name or
  // @[Name With Spaces], which braidsd sends as a mention
  let mention = $state<{ start: number; query: string } | null>(null);
  let mentionIndex = $state(0);
  const suggestions = $derived.by(() => {
    if (!mention) return [];
    const q = mention.query.toLowerCase();
    return (store.members[chatKey] ?? [])
      .filter((c) => !c.isSelf && c.id !== account?.selfId && c.name && c.name.toLowerCase().includes(q))
      .sort((a, b) => Number(b.name.toLowerCase().startsWith(q)) - Number(a.name.toLowerCase().startsWith(q)) || a.name.localeCompare(b.name))
      .slice(0, 8);
  });

  function updateMention() {
    if (!textarea) return;
    const val = textarea.value;
    const caret = textarea.selectionStart ?? val.length;
    const m = /(^|\s)@([^\s@[\]]*)$/.exec(val.slice(0, caret));
    mention = m ? { start: caret - m[2].length - 1, query: m[2] } : null;
    mentionIndex = 0;
  }

  function insertMention(name: string) {
    if (!mention || !textarea) return;
    const caret = textarea.selectionStart ?? text.length;
    const token = /\s|[\]]/.test(name) ? `@[${name}] ` : `@${name} `;
    text = text.slice(0, mention.start) + token + text.slice(caret);
    const pos = mention.start + token.length;
    mention = null;
    tick().then(() => {
      textarea?.focus();
      textarea?.setSelectionRange(pos, pos);
      resize();
    });
  }
  let emojiQuery = $state<{ start: number; query: string } | null>(null);
  let emojiIndex = $state(0);
  let emojiAll = $state<Emoji[]>([]);
  const emojiSuggestions = $derived.by(() => (emojiQuery ? searchEmoji(emojiAll, emojiQuery.query, 8) : []));

  function updateEmoji() {
    if (!textarea) return;
    const val = textarea.value;
    const caret = textarea.selectionStart ?? val.length;
    const m = /(^|\s):([a-z][a-z0-9_+-]+)$/i.exec(val.slice(0, caret));
    emojiQuery = m ? { start: caret - m[2].length - 1, query: m[2] } : null;
    emojiIndex = 0;
    if (emojiQuery && !emojiAll.length) {
      loadEmoji().then((list) => (emojiAll = list));
    }
  }

  function insertSuggestedEmoji(unicode: string) {
    if (!emojiQuery || !textarea) return;
    const caret = textarea.selectionStart ?? text.length;
    text = text.slice(0, emojiQuery.start) + unicode + text.slice(caret);
    const pos = emojiQuery.start + unicode.length;
    emojiQuery = null;
    tick().then(() => {
      textarea?.focus();
      textarea?.setSelectionRange(pos, pos);
      resize();
    });
  }

  // load / save the draft when the chat changes
  $effect(() => {
    const key = chatKey;
    if (key !== loadedKey) {
      if (loadedKey) saveDraft(loadedKey);
      loadedKey = key;
      text = store.drafts[key] ?? '';
      attachments = [];
      emojiQuery = null;
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
    updateMention();
    updateEmoji();
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

  // ---- voice messages ----
  let recording = $state(false);
  let recStarting = $state(false);
  let recStart = 0;
  let recElapsed = $state(0);
  let recTimer: ReturnType<typeof setInterval> | undefined;
  let levels = $state<number[]>(Array(28).fill(0));
  let stopLevels: (() => void) | undefined;
  const recTime = $derived(`${Math.floor(recElapsed / 60)}:${String(recElapsed % 60).padStart(2, '0')}`);

  async function startRecording() {
    if (recording || recStarting) return;
    recStarting = true;
    try {
      levels = Array(28).fill(0);
      stopLevels = api.onVoiceLevel((v) => (levels = [...levels.slice(1), v]));
      await api.voiceStart();
      recording = true;
      recStart = Date.now();
      recElapsed = 0;
      recTimer = setInterval(() => (recElapsed = Math.floor((Date.now() - recStart) / 1000)), 250);
    } catch (e) {
      stopLevels?.();
      store.toastError(e);
    } finally {
      recStarting = false;
    }
  }

  async function stopRecording(send: boolean) {
    if (!recording) return;
    recording = false;
    clearInterval(recTimer);
    stopLevels?.();
    try {
      const clip = await api.voiceStop(!send);
      // a tap on the mic is not a message
      if (send && clip && clip.durationMs >= 700) {
        const quotedId = ui.reply?.id;
        await api.call('messages.send', { accountId, chatId, filePath: clip.path, isVoice: true, ...(quotedId ? { quotedId } : {}) });
        ui.reply = null;
      } else if (send && clip) {
        store.toast('Hold on a bit longer: the recording was too short.');
      }
    } catch (e) {
      store.toastError(e);
    } finally {
      textarea?.focus();
    }
  }

  // leaving the chat cancels a recording
  onDestroy(() => {
    if (recording) stopRecording(false);
  });

  function onRecordKey(e: KeyboardEvent) {
    if (!recording) return;
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      stopRecording(false);
    } else if (e.key === 'Enter') {
      e.preventDefault();
      stopRecording(true);
    }
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
    if (emojiSuggestions.length) {
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        emojiIndex = (emojiIndex + (e.key === 'ArrowDown' ? 1 : emojiSuggestions.length - 1)) % emojiSuggestions.length;
        return;
      }
      if ((e.key === 'Enter' && !e.shiftKey) || e.key === 'Tab') {
        e.preventDefault();
        insertSuggestedEmoji(emojiSuggestions[emojiIndex].unicode);
        return;
      }
      if (e.key === 'Escape') {
        e.preventDefault();
        emojiQuery = null;
        return;
      }
    }
    if (suggestions.length) {
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        mentionIndex = (mentionIndex + (e.key === 'ArrowDown' ? 1 : suggestions.length - 1)) % suggestions.length;
        return;
      }
      if ((e.key === 'Enter' && !e.shiftKey) || e.key === 'Tab') {
        e.preventDefault();
        insertMention(suggestions[mentionIndex].name);
        return;
      }
      if (e.key === 'Escape') {
        e.preventDefault();
        mention = null;
        return;
      }
    }
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
      {#each attachments as path, i (path + i)}
        {@const kind = previewKind(path)}
        {#if kind}
          <span class="thumb" title={baseName(path)}>
            {#if kind === 'image'}
              <img src={api.fileUrl(path)} alt={baseName(path)} onerror={() => (broken[path] = true)} />
            {:else}
              <!-- the first frame -->
              <video use:mediaSrc={{ path, fragment: '#t=0.1' }} preload="metadata" muted onerror={() => (broken[path] = true)}></video>
              <span class="play"><Play size={14} fill="currentColor" /></span>
            {/if}
            <button class="remove" aria-label="Remove attachment" onclick={() => attachments.splice(i, 1)}><X size={12} strokeWidth={2.5} /></button>
          </span>
        {:else}
          <span class="att">
            <FileIcon size={14} />
            <span class="att-name">{baseName(path)}</span>
            <button class="icon-btn sm" aria-label="Remove attachment" onclick={() => attachments.splice(i, 1)}><X size={14} /></button>
          </span>
        {/if}
      {/each}
    </div>
  {/if}

  {#if recording}
    <div class="row recording" role="group" aria-label="Recording a voice message">
      <button class="icon-btn lg" aria-label="Cancel recording" title="Cancel (Esc)" onclick={() => stopRecording(false)}><Trash2 size={20} strokeWidth={1.75} /></button>
      <div class="rec-box">
        <span class="rec-dot"></span>
        <span class="rec-time">{recTime}</span>
        <span class="rec-levels" aria-hidden="true">
          {#each levels as v}<span style:height={`${Math.max(8, Math.round(v * 100))}%`}></span>{/each}
        </span>
      </div>
      <button class="send" aria-label="Send voice message" title="Send (Enter)" onclick={() => stopRecording(true)}>
        <SendHorizontal size={19} strokeWidth={1.9} />
      </button>
    </div>
  {:else}
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
        onclick={() => { updateMention(); updateEmoji(); }}
        onblur={() => setTimeout(() => { if (document.activeElement !== textarea) { mention = null; emojiQuery = null; } }, 150)}
        onpaste={onPaste}
      ></textarea>
      {#if suggestions.length}
        <div class="mentions" role="listbox" aria-label="Mention">
          {#each suggestions as c, i (c.id)}
            <button role="option" aria-selected={i === mentionIndex} onmousedown={(e) => { e.preventDefault(); insertMention(c.name); }}>
              <Avatar name={c.name} id={c.id} size={24} owner={accountId} />
              <span>{c.name}</span>
            </button>
          {/each}
        </div>
      {/if}
      {#if emojiSuggestions.length}
        <div class="mentions emoji-suggest" role="listbox" aria-label="Emoji suggestions">
          {#each emojiSuggestions as e, i (e.unicode)}
            <button role="option" aria-selected={i === emojiIndex} onmousedown={(ev) => { ev.preventDefault(); insertSuggestedEmoji(e.unicode); }}>
              <span class="glyph">{e.unicode}</span>
              <span class="lbl">{e.label}</span>
            </button>
          {/each}
        </div>
      {/if}
      <button class="icon-btn sm" aria-label="Emoji" title="Emoji" onclick={() => (emojiOpen = !emojiOpen)}><Smile size={19} strokeWidth={1.75} /></button>
      {#if emojiOpen}
        <EmojiPicker onPick={insertEmoji} onClose={() => { emojiOpen = false; textarea?.focus(); }} />
      {/if}
    </div>
    {#if canPoll}
      <button class="icon-btn lg" aria-label="New poll" title="New poll" onclick={() => (ui.pollFor = chatKey)} disabled={!!ui.edit}><ChartColumnBig size={20} strokeWidth={1.75} /></button>
    {/if}
    {#if !text.trim() && !attachments.length && !ui.edit}
      <button class="send mic" aria-label="Record voice message" title="Record a voice message" onclick={startRecording} disabled={recStarting}>
        {#if recStarting}<LoaderCircle size={19} class="spin" />{:else}<Mic size={19} strokeWidth={1.9} />{/if}
      </button>
    {:else}
      <button class="send" aria-label={ui.edit ? 'Save edit' : 'Send'} title={ui.edit ? 'Save (Enter)' : 'Send (Enter)'} onclick={submit}
        disabled={sending || (!text.trim() && !attachments.length)}>
        <SendHorizontal size={19} strokeWidth={1.9} />
      </button>
    {/if}
  </div>
  {/if}
</div>

<svelte:window onkeydown={onRecordKey} />

<style>
  .composer { border-top: 1px solid var(--border); background: var(--bg-2); flex-shrink: 0; }
  .context { display: flex; align-items: center; gap: 10px; padding: 8px 12px 0; }
  .ctx-ico { color: var(--accent); display: flex; }
  .ctx-body { flex: 1; min-width: 0; border-left: 3px solid var(--c); padding-left: 8px; }
  .ctx-title { font-size: 12px; font-weight: 650; color: var(--c); }
  .ctx-text { font-size: 13px; color: var(--text-2); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .rec-box {
    flex: 1; min-width: 0; height: 40px; display: flex; align-items: center; gap: 10px; padding: 0 14px; border-radius: 12px;
    background: var(--surface-2); border: 1px solid var(--border-strong);
  }
  @keyframes blink { 0%, 100% { opacity: 0.3; } 40% { opacity: 1; } }
  .rec-dot { width: 10px; height: 10px; border-radius: 5px; background: var(--danger); animation: blink 1.2s infinite; flex-shrink: 0; }
  .rec-time { font-size: 13px; font-variant-numeric: tabular-nums; color: var(--text); min-width: 34px; }
  .rec-levels { flex: 1; height: 22px; display: flex; align-items: center; justify-content: flex-end; gap: 2px; overflow: hidden; }
  .rec-levels span { width: 3px; flex-shrink: 0; border-radius: 2px; background: var(--accent); transition: height 90ms linear; }
  .attachments { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; padding: 8px 12px 0 56px; }
  .thumb { position: relative; width: 64px; height: 64px; border-radius: 8px; overflow: hidden; background: var(--skeleton); border: 1px solid var(--border); flex-shrink: 0; }
  .thumb img, .thumb video { width: 100%; height: 100%; object-fit: cover; display: block; }
  .thumb .play {
    position: absolute; left: 50%; top: 50%; transform: translate(-50%, -50%); width: 26px; height: 26px; border-radius: 13px;
    display: flex; align-items: center; justify-content: center; background: rgb(0 0 0 / 0.55); color: #fff; pointer-events: none;
  }
  .thumb .remove {
    position: absolute; top: 3px; right: 3px; width: 20px; height: 20px; border-radius: 10px; border: 0; padding: 0;
    display: flex; align-items: center; justify-content: center; background: rgb(0 0 0 / 0.6); color: #fff;
  }
  .thumb .remove:hover { background: rgb(0 0 0 / 0.8); }
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
  .mentions {
    position: absolute; bottom: 46px; left: 0; width: 260px; max-height: 280px; overflow-y: auto; padding: 4px; display: flex; flex-direction: column;
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius-lg); box-shadow: var(--shadow-2); z-index: 10;
  }
  .mentions button { display: flex; align-items: center; gap: 8px; height: 34px; padding: 0 8px; border: 0; border-radius: 8px; background: none; text-align: left; font-size: 14px; color: var(--text); }
  .mentions button[aria-selected='true'], .mentions button:hover { background: var(--hover); }
  .emoji-suggest .glyph { width: 24px; text-align: center; font-size: 18px; line-height: 1; flex-shrink: 0; }
  .emoji-suggest .lbl { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
</style>
