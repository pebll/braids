<!--
  VoicePlayer.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Inline player for voice messages and other audio: play / pause, a
  bar-style progress (click or arrow keys to seek), time and speed
  (1x, 1.5x, 2x). One audio plays at a time across the app.
-->
<script module lang="ts">
  let playing: HTMLAudioElement | null = null;
</script>

<script lang="ts">
  import { LoaderCircle, Pause, Play, TriangleAlert } from '@lucide/svelte';
  import { onDestroy } from 'svelte';
  import * as api from '../lib/api';
  import { mediaSrc } from '../lib/media';
  import { store } from '../lib/store.svelte';

  let { path, id, status, onDownload, mimeType = '' }: {
    path: string; // local file, '' while not downloaded
    mimeType?: string; // the file's type
    id: string; // seeds the bar pattern
    status: string; // file status
    onDownload: () => void;
  } = $props();

  const BARS = 36;
  const SPEEDS = [1, 1.5, 2];

  let audio: HTMLAudioElement | undefined = $state();
  let isPlaying = $state(false);
  let current = $state(0);
  let duration = $state(0);
  let speed = $state(1);
  let failed = $state(false);
  // the file comes as a blob typed for WebKitGTK: Opus voice notes as
  // audio/ogg, AAC ones (Signal, iPhones) as audio/aac or audio/mp4
  const type = $derived(/aac/i.test(mimeType) ? 'audio/aac' : /mp4|m4a/i.test(mimeType) ? 'audio/mp4' : 'audio/ogg');
  function onError() {
    failed = true;
  }

  function openExternally() {
    api.openPath(path).catch((e) => store.toastError(e));
  }

  // a stable pseudo waveform per message (the protocols do not all send one)
  const bars = $derived.by(() => {
    let h = 2166136261;
    for (const ch of id) h = Math.imul(h ^ ch.charCodeAt(0), 16777619);
    return Array.from({ length: BARS }, (_, i) => {
      h = Math.imul(h ^ (i + 1), 16777619);
      return 0.25 + (((h >>> 0) % 1000) / 1000) * 0.75;
    });
  });
  const progress = $derived(duration > 0 ? Math.min(1, current / duration) : 0);

  function fmt(sec: number): string {
    if (!isFinite(sec) || sec < 0) return '0:00';
    const s = Math.floor(sec);
    return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  }

  function toggle() {
    if (!path) {
      if (status !== 'downloading') onDownload();
      return;
    }
    if (!audio) return;
    if (audio.paused) {
      if (playing && playing !== audio) playing.pause();
      playing = audio;
      audio.playbackRate = speed;
      audio.play().catch(() => (failed = true));
    } else {
      audio.pause();
    }
  }

  function seekTo(fraction: number) {
    if (!audio || !(duration > 0)) return;
    audio.currentTime = Math.max(0, Math.min(1, fraction)) * duration;
    current = audio.currentTime;
  }

  function onBarClick(e: MouseEvent) {
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    seekTo((e.clientX - r.left) / r.width);
  }

  function onBarKey(e: KeyboardEvent) {
    if (e.key === 'ArrowRight') seekTo((current + 5) / duration);
    else if (e.key === 'ArrowLeft') seekTo((current - 5) / duration);
    else return;
    e.preventDefault();
  }

  function cycleSpeed() {
    speed = SPEEDS[(SPEEDS.indexOf(speed) + 1) % SPEEDS.length];
    if (audio) audio.playbackRate = speed;
  }

  // Opus in Ogg often reports no duration until read through: ask for the end
  function onMeta() {
    if (!audio) return;
    if (isFinite(audio.duration)) {
      duration = audio.duration;
      return;
    }
    const fix = () => {
      if (!audio) return;
      audio.removeEventListener('timeupdate', fix);
      duration = isFinite(audio.duration) ? audio.duration : 0;
      audio.currentTime = 0;
    };
    audio.addEventListener('timeupdate', fix);
    audio.currentTime = 1e7;
  }

  onDestroy(() => {
    if (playing === audio) playing = null;
    audio?.pause();
  });
</script>

<div class="voice" class:playing={isPlaying}>
  <button class="play" class:failed onclick={failed ? openExternally : toggle}
    aria-label={failed ? 'Open in your audio player' : !path ? 'Download voice message' : isPlaying ? 'Pause' : 'Play'}
    title={failed ? 'Open in your audio player. Playing AAC (.m4a) and MP3 in braids needs GStreamer’s codecs, e.g. gstreamer1.0-libav.' : undefined}>
    {#if failed}<TriangleAlert size={18} />
    {:else if status === 'downloading'}<LoaderCircle size={18} class="spin" />
    {:else if isPlaying}<Pause size={18} fill="currentColor" />
    {:else}<Play size={18} fill="currentColor" />{/if}
  </button>
  <div class="track">
    <div class="bars" role="slider" tabindex="0" aria-label="Position" aria-valuemin={0} aria-valuemax={Math.round(duration)} aria-valuenow={Math.round(current)}
      onclick={onBarClick} onkeydown={onBarKey}>
      {#each bars as b, i}
        <span class="bar" class:done={i / BARS < progress} style:height={`${Math.round(b * 100)}%`}></span>
      {/each}
    </div>
    <div class="info">
      <span class="time">{failed ? 'Cannot play here — click to open it' : isPlaying || current > 0 ? fmt(current) : fmt(duration)}</span>
      <button class="speed" onclick={cycleSpeed} aria-label="Playback speed">{speed}×</button>
    </div>
  </div>
  {#if path}
    <audio
      bind:this={audio}
      use:mediaSrc={{ path, type }}
      preload="metadata"
      onloadedmetadata={onMeta}
      ontimeupdate={() => audio && (current = audio.currentTime)}
      onplay={() => (isPlaying = true)}
      onpause={() => (isPlaying = false)}
      onended={() => { isPlaying = false; current = 0; }}
      onerror={onError}
    ></audio>
  {/if}
</div>

<style>
  .voice { display: flex; align-items: center; gap: 10px; width: 280px; max-width: 100%; padding: 2px 0 4px; }
  .play {
    width: 38px; height: 38px; border-radius: 19px; border: 0; flex-shrink: 0; display: flex; align-items: center; justify-content: center;
    background: var(--accent); color: var(--on-accent);
  }
  .play.failed { background: var(--surface-2); color: var(--text-muted); }
  .track { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 3px; }
  .bars { height: 26px; display: flex; align-items: center; gap: 2px; cursor: pointer; outline: none; border-radius: 4px; }
  .bars:focus-visible { box-shadow: 0 0 0 2px var(--focus); }
  .bar { flex: 1; min-width: 2px; border-radius: 1px; background: color-mix(in srgb, var(--text-muted) 45%, transparent); }
  .bar.done { background: var(--accent); }
  .info { display: flex; align-items: center; justify-content: space-between; font-size: 11px; color: var(--text-muted); font-variant-numeric: tabular-nums; }
  .speed { border: 0; background: var(--surface-2); color: var(--text-2); font-size: 11px; font-weight: 700; padding: 0 6px; border-radius: 8px; height: 18px; }
</style>
