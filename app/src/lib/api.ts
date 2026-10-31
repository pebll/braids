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

// Copies a received file into the Downloads folder; returns the new path
// ('' in the browser, which saves through its own download).
export async function saveToDownloads(path: string, name: string): Promise<string> {
  if (isTauri) {
    const { invoke } = await import('@tauri-apps/api/core');
    return invoke<string>('save_to_downloads', { path, name });
  }
  const a = document.createElement('a');
  a.href = fileUrl(path);
  a.download = name || path.split('/').pop() || 'file';
  a.click();
  return '';
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
