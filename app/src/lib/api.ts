// api.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// Transport to braidsd: through the Tauri shell (src-tauri, Unix socket), or
// in a plain browser through the Vite dev bridge (dev/bridge.ts). Also wraps
// the few platform services the UI needs (files, notifications, opening).

import { ApiError, type ApiEvent } from './types';

export const isTauri = typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window;

export async function setWebviewZoom(scale: number): Promise<boolean> {
  if (!isTauri) return false;
  const { getCurrentWebview } = await import('@tauri-apps/api/webview');
  await getCurrentWebview().setZoom(scale);
  return true;
}

type EventHandler = (event: ApiEvent) => void;
type ConnectionHandler = (connected: boolean) => void;

export async function call<T = any>(method: string, params: Record<string, unknown> = {}): Promise<T> {
  if (isTauri) {
    const { invoke } = await import('@tauri-apps/api/core');
    try {
      return await invoke<T>('api_call', { method, params });
    } catch (e: any) {
      throw new ApiError(e?.code ?? -32000, e?.message ?? String(e));
    }
  }

  const res = await fetch('/__braids/call', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ method, params }),
  });
  const reply = await res.json();
  if (reply.error) throw new ApiError(reply.error.code, reply.error.message);
  return reply.result as T;
}

// Subscribes to daemon events and connection changes; the connection handler
// is called with true after each (re)connect, when the client should resync.
export async function subscribe(onEvent: EventHandler, onConnection: ConnectionHandler): Promise<void> {
  if (isTauri) {
    const { listen } = await import('@tauri-apps/api/event');
    const { invoke } = await import('@tauri-apps/api/core');
    await listen<ApiEvent>('braids://event', (e) => onEvent(e.payload));
    await listen<boolean>('braids://connection', (e) => onConnection(e.payload));
    onConnection(await invoke<boolean>('daemon_connected'));
    return;
  }

  const source = new EventSource('/__braids/events');
  source.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg.method === '$connection') onConnection(msg.params.connected);
    else onEvent(msg);
  };
  source.onerror = () => onConnection(false);
}

// Text to the clipboard. The desktop app goes through the shell: WebKitGTK
// does not allow the async clipboard API on the app's own scheme.
export async function copyText(text: string): Promise<void> {
  if (isTauri) {
    const { writeText } = await import('@tauri-apps/plugin-clipboard-manager');
    await writeText(text);
    return;
  }
  await navigator.clipboard.writeText(text);
}

// Copies a picture to the clipboard as PNG (Tauri decodes only PNG, and
// browsers accept only PNG on the clipboard; photos are mostly JPEG).
export async function copyImage(path: string): Promise<void> {
  const response = await fetch(fileUrl(path));
  if (!response.ok) throw new Error(`Could not read image (${response.status})`);
  const image = await createImageBitmap(await response.blob());
  const canvas = document.createElement('canvas');
  canvas.width = image.width;
  canvas.height = image.height;
  canvas.getContext('2d')!.drawImage(image, 0, 0);
  image.close();
  const png = await new Promise<Blob>((resolve, reject) => {
    canvas.toBlob((value) => (value ? resolve(value) : reject(new Error('Could not encode image'))), 'image/png');
  });
  if (isTauri) {
    const { writeImage } = await import('@tauri-apps/plugin-clipboard-manager');
    await writeImage(new Uint8Array(await png.arrayBuffer()));
    return;
  }
  if (!navigator.clipboard?.write || typeof ClipboardItem === 'undefined') {
    throw new Error('Image clipboard is not supported by this browser');
  }
  await navigator.clipboard.write([new ClipboardItem({ 'image/png': png })]);
}

// ---- updates and start at login (desktop app only) ----

export interface Update {
  version: string;
  url: string;
}

// A newer release than this app, or null (up to date, offline, browser).
export async function checkUpdate(): Promise<Update | null> {
  if (!isTauri) return null;
  const { invoke } = await import('@tauri-apps/api/core');
  return invoke<Update | null>('check_update');
}

// "flatpak", "appimage" or "" (tarball, .deb, development)
export async function installKind(): Promise<string> {
  if (!isTauri) return 'browser';
  const { invoke } = await import('@tauri-apps/api/core');
  return invoke<string>('install_kind');
}

export async function autostartEnabled(): Promise<boolean> {
  if (!isTauri) return false;
  const { isEnabled } = await import('@tauri-apps/plugin-autostart');
  return isEnabled();
}

export async function setAutostart(on: boolean): Promise<void> {
  const { enable, disable } = await import('@tauri-apps/plugin-autostart');
  await (on ? enable() : disable());
}

export interface LinkPreview {
  url: string;
  title: string;
  description: string;
  siteName: string;
  image: string; // local file (desktop app) or URL (browser), '' without one
}

const previews = new Map<string, Promise<LinkPreview | null>>();

// Title, description and image of a linked page (null: none); one request
// per URL and session.
export function linkPreview(url: string): Promise<LinkPreview | null> {
  let p = previews.get(url);
  if (!p) {
    p = (async () => {
      if (isTauri) {
        const { invoke } = await import('@tauri-apps/api/core');
        return invoke<LinkPreview | null>('link_preview', { url });
      }
      const res = await fetch(`/__braids/preview?url=${encodeURIComponent(url)}`);
      return res.ok ? ((await res.json()) as LinkPreview | null) : null;
    })().catch(() => null);
    previews.set(url, p);
  }
  return p;
}

export function fileUrl(path: string): string {
  if (!path) return '';
  if (isTauri) {
    const internals = (window as any).__TAURI_INTERNALS__;
    return internals.convertFileSrc(path, 'asset');
  }
  return `/__braids/file?path=${encodeURIComponent(path)}`;
}

// Saves data (pasted or dropped in the browser) to a temp file for sending.
export async function saveTempFile(name: string, data: Blob): Promise<string> {
  if (isTauri) {
    const { invoke } = await import('@tauri-apps/api/core');
    const bytes = Array.from(new Uint8Array(await data.arrayBuffer()));
    return invoke<string>('save_temp_file', { name, data: bytes });
  }
  const res = await fetch(`/__braids/upload?name=${encodeURIComponent(name)}`, { method: 'POST', body: data });
  return (await res.json()).path;
}

// Lets the user pick files; returns absolute paths.
export async function pickFiles(): Promise<string[]> {
  if (isTauri) {
    const { open } = await import('@tauri-apps/plugin-dialog');
    const result = await open({ multiple: true, directory: false });
    if (!result) return [];
    return Array.isArray(result) ? result : [result];
  }

  return new Promise((resolve) => {
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.onchange = async () => {
      const files = Array.from(input.files ?? []);
      resolve(await Promise.all(files.map((f) => saveTempFile(f.name, f))));
    };
    input.click();
  });
}

// Saves a copy of a received file where the user picks (system save
// dialog); returns the new path, '' when cancelled or in the browser (which
// saves through its own download).
export async function saveFileAs(path: string, name: string): Promise<string> {
  if (isTauri) {
    const { invoke } = await import('@tauri-apps/api/core');
    return invoke<string>('save_file_as', { path, name });
  }
  const a = document.createElement('a');
  a.href = fileUrl(path);
  a.download = name || path.split('/').pop() || 'file';
  a.click();
  return '';
}

// The third-party notices shipped with the app, else the repository's.
export async function openLicenses(): Promise<void> {
  if (isTauri) {
    const { invoke } = await import('@tauri-apps/api/core');
    const path = await invoke<string>('licenses_path');
    if (path) return openPath(path);
  }
  await openUrl('https://github.com/pebll/braids/blob/main/app/THIRD_PARTY_LICENSES');
}

export async function openPath(path: string): Promise<void> {
  if (isTauri) {
    const { openPath } = await import('@tauri-apps/plugin-opener');
    await openPath(path);
  } else {
    window.open(fileUrl(path), '_blank');
  }
}

export async function openUrl(url: string): Promise<void> {
  if (isTauri) {
    const { openUrl } = await import('@tauri-apps/plugin-opener');
    await openUrl(url);
  } else {
    window.open(url, '_blank', 'noopener');
  }
}

let notificationPermission: boolean | null = null;

export async function notify(title: string, body: string, onClick?: () => void): Promise<void> {
  if (isTauri) {
    const n = await import('@tauri-apps/plugin-notification');
    if (notificationPermission === null) {
      notificationPermission = await n.isPermissionGranted();
      if (!notificationPermission) notificationPermission = (await n.requestPermission()) === 'granted';
    }
    if (notificationPermission) n.sendNotification({ title, body });
    return;
  }

  if (!('Notification' in window)) return;
  if (Notification.permission === 'default') await Notification.requestPermission();
  if (Notification.permission !== 'granted') return;
  const notification = new Notification(title, { body });
  notification.onclick = () => {
    window.focus();
    onClick?.();
  };
}

// Unread count for the tray / dock badge.
export async function setBadge(count: number): Promise<void> {
  if (isTauri) {
    const { invoke } = await import('@tauri-apps/api/core');
    await invoke('set_badge', { count }).catch(() => {});
  } else {
    document.title = count > 0 ? `(${count}) braids` : 'braids';
  }
}

export async function setCloseToTray(enabled: boolean): Promise<void> {
  if (!isTauri) return;
  const { invoke } = await import('@tauri-apps/api/core');
  await invoke('set_close_to_tray', { enabled }).catch(() => {});
}

// Files dropped onto the window (Tauri gives paths; the browser gives files).
export async function onFileDrop(handler: (paths: string[]) => void, onHover: (hover: boolean) => void): Promise<void> {
  if (isTauri) {
    const { getCurrentWebview } = await import('@tauri-apps/api/webview');
    await getCurrentWebview().onDragDropEvent((e) => {
      if (e.payload.type === 'enter' || e.payload.type === 'over') onHover(true);
      else if (e.payload.type === 'leave') onHover(false);
      else if (e.payload.type === 'drop') {
        onHover(false);
        handler(e.payload.paths);
      }
    });
    return;
  }

  window.addEventListener('dragover', (e) => {
    if (e.dataTransfer?.types.includes('Files')) {
      e.preventDefault();
      onHover(true);
    }
  });
  window.addEventListener('dragleave', (e) => {
    if (e.relatedTarget === null) onHover(false);
  });
  window.addEventListener('drop', async (e) => {
    e.preventDefault();
    onHover(false);
    const files = Array.from(e.dataTransfer?.files ?? []);
    if (files.length) handler(await Promise.all(files.map((f) => saveTempFile(f.name, f))));
  });
}

// ---- voice messages ----
// The desktop app records Ogg/Opus with GStreamer (src-tauri/src/voice.rs);
// in a browser (development, UI tests) the browser's recorder is used.

export interface VoiceClip {
  path: string;
  durationMs: number;
}

let browserRec: { recorder: MediaRecorder; chunks: Blob[]; started: number; stopLevel: () => void } | null = null;
const levelHandlers = new Set<(level: number) => void>();
let tauriLevelListening = false;

// input level (0..1) while recording; returns the unsubscribe function
export function onVoiceLevel(handler: (level: number) => void): () => void {
  levelHandlers.add(handler);
  if (isTauri && !tauriLevelListening) {
    tauriLevelListening = true;
    import('@tauri-apps/api/event').then(({ listen }) => listen<number>('voice-level', (e) => levelHandlers.forEach((h) => h(e.payload))));
  }
  return () => levelHandlers.delete(handler);
}

export async function voiceStart(): Promise<void> {
  if (isTauri) {
    const { invoke } = await import('@tauri-apps/api/core');
    try {
      await invoke('voice_start');
    } catch (e) {
      throw new Error(String(e));
    }
    return;
  }

  const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
  const recorder = new MediaRecorder(stream);
  const chunks: Blob[] = [];
  recorder.ondataavailable = (e) => e.data.size && chunks.push(e.data);
  // level from an analyser
  const ctx = new AudioContext();
  const analyser = ctx.createAnalyser();
  ctx.createMediaStreamSource(stream).connect(analyser);
  const data = new Float32Array(analyser.fftSize);
  const timer = setInterval(() => {
    analyser.getFloatTimeDomainData(data);
    const rms = Math.sqrt(data.reduce((n, v) => n + v * v, 0) / data.length);
    levelHandlers.forEach((h) => h(Math.min(1, rms * 4)));
  }, 100);
  recorder.start(250);
  browserRec = {
    recorder,
    chunks,
    started: Date.now(),
    stopLevel: () => {
      clearInterval(timer);
      ctx.close();
      stream.getTracks().forEach((t) => t.stop());
    },
  };
}

export async function voiceStop(cancel: boolean): Promise<VoiceClip | null> {
  if (isTauri) {
    const { invoke } = await import('@tauri-apps/api/core');
    try {
      return await invoke<VoiceClip | null>('voice_stop', { cancel });
    } catch (e) {
      throw new Error(String(e));
    }
  }

  const rec = browserRec;
  browserRec = null;
  if (!rec) return null;
  const stopped = new Promise((r) => (rec.recorder.onstop = r));
  rec.recorder.stop();
  await stopped;
  rec.stopLevel();
  if (cancel) return null;
  const type = rec.recorder.mimeType || 'audio/webm';
  const ext = type.includes('ogg') ? 'ogg' : 'webm';
  const path = await saveTempFile(`voice-message.${ext}`, new Blob(rec.chunks, { type }));
  return { path, durationMs: Date.now() - rec.started };
}
