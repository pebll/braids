<!--
  Lightbox.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Image viewer: zoom (wheel towards the cursor, + / - / 0, double-click),
  pan by dragging, switch between the images of the chat or list (← → or
  j k, also while zoomed), Esc closes.
-->
<script lang="ts">
  import { ChevronLeft, ChevronRight, Copy, Download, ExternalLink, MessageSquareText, X, ZoomIn, ZoomOut } from '@lucide/svelte';
  import * as api from '../lib/api';
  import { mediaSrc } from '../lib/media';
  import { chatKey, store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';

  const MIN = 1;
  const MAX = 8;

  const lb = $derived(ui.lightbox!);
  const item = $derived(lb.items[lb.index]);
  const count = $derived(lb.items.length);
  // profile pictures: the small one until the full-size one has arrived
  const path = $derived((item?.fullAvatar && store.fullAvatars[item.fullAvatar]) || item?.path);

  let scale = $state(1);
  let x = $state(0);
  let y = $state(0);
  let stage: HTMLDivElement | undefined = $state();
  let drag: { px: number; py: number; x: number; y: number; moved: boolean } | null = null;
  let videoFailed = $state(false);
  const isVideo = $derived(!!item?.video);

  // a new image starts fitted
  $effect(() => {
    void item?.path;
    scale = 1;
    x = 0;
    y = 0;
    videoFailed = false;
  });

  function close() {
    ui.lightbox = null;
  }

  function go(delta: number) {
    if (count < 2 || !ui.lightbox) return;
    ui.lightbox.index = (lb.index + delta + count) % count;
  }

  // zoom to s, keeping the point (cx, cy) (relative to the stage center) in place
  function zoomTo(s: number, cx = 0, cy = 0) {
    const next = Math.min(MAX, Math.max(MIN, s));
    const k = next / scale;
    x = cx - (cx - x) * k;
    y = cy - (cy - y) * k;
    scale = next;
    if (scale === 1) {
      x = 0;
      y = 0;
    }
  }

  function center(e: { clientX: number; clientY: number }): [number, number] {
    const r = stage!.getBoundingClientRect();
    return [e.clientX - (r.left + r.width / 2), e.clientY - (r.top + r.height / 2)];
  }

  function onWheel(e: WheelEvent) {
    if (isVideo) return;
    e.preventDefault();
    const [cx, cy] = center(e);
    zoomTo(scale * Math.exp(-e.deltaY * 0.0015), cx, cy);
  }

  function onDblClick(e: MouseEvent) {
    if (isVideo) return;
    const [cx, cy] = center(e);
    zoomTo(scale > 1 ? 1 : 2.5, cx, cy);
  }

  function onPointerDown(e: PointerEvent) {
    if (e.button !== 0 || (isVideo && e.target !== stage)) return;
    drag = { px: e.clientX, py: e.clientY, x, y, moved: false };
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
  }

  function onPointerMove(e: PointerEvent) {
    if (!drag) return;
    const dx = e.clientX - drag.px;
    const dy = e.clientY - drag.py;
    if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
    if (scale > 1) {
      x = drag.x + dx;
      y = drag.y + dy;
    }
  }

  function onPointerUp(e: PointerEvent) {
    const d = drag;
    drag = null;
    // a click on the backdrop (not a drag, not on the image) closes
    if (d && !d.moved && scale === 1 && e.target === stage) close();
  }

  async function save() {
    try {
      const saved = await api.saveFileAs(path, item.name);
      if (saved) store.toast(`Saved to ${saved}`, 'info', { label: 'Open', run: () => api.openPath(saved).catch((e) => store.toastError(e)) });
    } catch (e) {
      store.toastError(e);
    }
  }

  function copy() {
    api.copyImage(path).then(() => store.toast('Image copied')).catch((e) => store.toastError(e));
  }

  // closes the viewer (and the media dialog) and scrolls the chat to the message
  function showInChat() {
    const msg = item.msg;
    if (!msg) return;
    const key = chatKey(msg.accountId, msg.chatId);
    close();
    ui.media = null;
    store.openChat(key).then(() => {
      ui.jumpRequest = { chatKey: key, msgId: msg.id };
    }).catch((e) => store.toastError(e));
  }

  function onKey(e: KeyboardEvent) {
    if ((e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey && (e.key === 'c' || e.key === 's')) {
      // Ctrl+C copies the picture (unless text is selected), Ctrl+S saves it
      if (e.key === 'c' && (isVideo || window.getSelection()?.toString())) return;
      e.preventDefault();
      e.stopPropagation();
      if (e.key === 'c') copy();
      else save();
      return;
    }
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    const k = e.key;
    let handled = true;
    if (k === 'Escape') close();
    else if (!isVideo && (k === '+' || k === '=')) zoomTo(scale * 1.5);
    else if (!isVideo && (k === '-' || k === '_')) zoomTo(scale / 1.5);
    else if (!isVideo && k === '0') zoomTo(1);
    else if (k === 'ArrowRight' || k === 'j' || k === 'l' || k === 'ArrowDown' || k === 'PageDown') go(1);
    else if (k === 'ArrowLeft' || k === 'k' || k === 'h' || k === 'ArrowUp' || k === 'PageUp') go(-1);
    else handled = false;
    if (handled) {
      e.preventDefault();
      e.stopPropagation();
    }
  }
</script>

<svelte:window onkeydown={onKey} />

{#if item}
  <div class="lightbox" role="dialog" aria-modal="true" aria-label={item.name}>
    <div class="lb-bar">
      <span class="lb-name">{item.name}</span>
      {#if count > 1}<span class="lb-pos">{lb.index + 1} / {count}</span>{/if}
      {#if !isVideo}
        <span class="lb-zoom">{Math.round(scale * 100)}%</span>
        <button class="icon-btn" aria-label="Zoom out" title="Zoom out (-)" onclick={() => zoomTo(scale / 1.5)} disabled={scale <= MIN}><ZoomOut size={19} /></button>
        <button class="icon-btn" aria-label="Zoom in" title="Zoom in (+)" onclick={() => zoomTo(scale * 1.5)} disabled={scale >= MAX}><ZoomIn size={19} /></button>
      {/if}
      <span class="lb-sep"></span>
      {#if item.msg}
        <button class="icon-btn" aria-label="Show in chat" title="Show in chat" onclick={showInChat}><MessageSquareText size={19} /></button>
      {/if}
      {#if !isVideo}
        <button class="icon-btn" aria-label="Copy image" title="Copy image (Ctrl+C)" onclick={copy}><Copy size={19} /></button>
      {/if}
      <button class="icon-btn" aria-label="Download" title="Save as… (Ctrl+S)" onclick={save}><Download size={19} /></button>
      <button class="icon-btn" aria-label="Open with default app" title="Open with default app" onclick={() => api.openPath(path).catch((e) => store.toastError(e))}><ExternalLink size={19} /></button>
      <button class="icon-btn" aria-label="Close" title="Close (Esc)" onclick={close}><X size={20} /></button>
    </div>
    <div
      class="lb-stage"
      class:zoomed={scale > 1}
      role="presentation"
      bind:this={stage}
      onwheel={onWheel}
      ondblclick={onDblClick}
      onpointerdown={onPointerDown}
      onpointermove={onPointerMove}
      onpointerup={onPointerUp}
    >
      {#if isVideo}
        {#if videoFailed}
          <div class="lb-failed">
            <p>This video cannot be played here.</p>
            <small>Most videos (MP4, H.264) need GStreamer's libav plugin:<br />
              <code>sudo apt install gstreamer1.0-libav</code> (Debian, Ubuntu)<br />
              <code>sudo dnf install gstreamer1-plugin-libav</code> (Fedora) · <code>sudo pacman -S gst-libav</code> (Arch)<br />
              then restart braids.</small>
            <button class="btn" onclick={() => api.openPath(item.path).catch((e) => store.toastError(e))}><ExternalLink size={16} />Open in the video player</button>
          </div>
        {:else}
          {#key item.path}
            <!-- svelte-ignore a11y_media_has_caption -->
            <video use:mediaSrc={{ path: item.path }} controls autoplay onerror={() => (videoFailed = true)}></video>
          {/key}
        {/if}
      {:else}
        <img src={api.fileUrl(path)} alt={item.name} class:avatar={!!item.fullAvatar} draggable="false" style:transform={`translate(${x}px, ${y}px) scale(${scale})`} />
      {/if}
      {#if count > 1}
        <button class="nav prev" aria-label="Previous image" title="Previous (← or k)" onclick={() => go(-1)} onpointerdown={(e) => e.stopPropagation()}><ChevronLeft size={26} /></button>
        <button class="nav next" aria-label="Next image" title="Next (→ or j)" onclick={() => go(1)} onpointerdown={(e) => e.stopPropagation()}><ChevronRight size={26} /></button>
      {/if}
    </div>
    <div class="lb-hint">{isVideo ? '' : 'Scroll or + / − to zoom · drag to move · '}{count > 1 ? '← → or j k to switch · ' : ''}Esc to close</div>
  </div>
{/if}

<style>
  .lightbox { position: fixed; inset: 0; z-index: 70; background: rgba(15, 14, 13, 0.94); display: flex; flex-direction: column; animation: fade var(--duration-fast) var(--ease); }
  .lb-sep { width: 1px; height: 20px; margin: 0 4px; background: rgba(255, 255, 255, 0.18); }
  .lb-bar { display: flex; align-items: center; gap: 4px; padding: 10px 12px 10px 20px; color: #f2efeb; }
  .lb-bar .icon-btn { color: #f2efeb; }
  .lb-bar .icon-btn:hover:not(:disabled) { background: rgba(255, 255, 255, 0.1); }
  .lb-bar .icon-btn:disabled { opacity: 0.35; }
  .lb-name { flex: 1; font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .lb-pos, .lb-zoom { font-size: 12px; color: rgba(242, 239, 235, 0.7); font-variant-numeric: tabular-nums; padding: 0 6px; }
  .lb-stage { flex: 1; min-height: 0; position: relative; display: flex; align-items: center; justify-content: center; overflow: hidden; padding: 0 64px; touch-action: none; cursor: default; }
  .lb-stage.zoomed { cursor: grab; }
  .lb-stage.zoomed:active { cursor: grabbing; }
  .lb-stage img.avatar { width: min(640px, 100%); height: auto; }
  .lb-stage img { max-width: 100%; max-height: 100%; object-fit: contain; border-radius: 6px; user-select: none; transform-origin: center center; transition: transform 60ms linear; }
  .lb-stage video { max-width: 100%; max-height: 100%; border-radius: 6px; background: #000; }
  .lb-failed { display: flex; flex-direction: column; align-items: center; gap: 8px; color: #f2efeb; text-align: center; }
  .lb-failed p { margin: 0; font-size: 15px; }
  .lb-failed small { color: rgba(242, 239, 235, 0.65); }
  .lb-failed .btn { display: inline-flex; align-items: center; gap: 6px; margin-top: 6px; }
  .nav {
    position: absolute; top: 50%; transform: translateY(-50%); width: 44px; height: 44px; border-radius: 22px; border: 0;
    display: flex; align-items: center; justify-content: center; background: rgba(255, 255, 255, 0.08); color: #f2efeb;
  }
  .nav:hover { background: rgba(255, 255, 255, 0.18); }
  .prev { left: 12px; }
  .next { right: 12px; }
  .lb-hint { text-align: center; font-size: 11px; color: rgba(242, 239, 235, 0.5); padding: 6px 12px 10px; }
  .lb-failed code { user-select: text; font-size: 12px; }
</style>
