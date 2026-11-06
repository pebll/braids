// ui.svelte.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

import type { Message } from './types';

export interface MenuItem {
  label: string;
  icon?: any;
  danger?: boolean;
  disabled?: boolean;
  hint?: string;
  run: () => void;
}

export interface ConfirmRequest {
  title: string;
  body: string;
  confirm: string;
  danger?: boolean;
  resolve: (ok: boolean) => void;
}

export interface LightboxItem {
  path: string;
  name: string;
  video?: boolean; // played in the viewer instead of shown
  fullAvatar?: string; // profile picture (chat key): shown in full size once fetched
}

class Ui {
  width = $state(typeof window !== 'undefined' ? window.innerWidth : 1280);
  infoOpen = $state(false);
  settingsOpen = $state(false);
  settingsSection = $state('general');
  settingsAccount = $state('');
  newChatOpen = $state(false);
  joinLink = $state(''); // opens the New chat dialog on "Join a group" with this link
  drawerOpen = $state(false); // sidebar as drawer (narrow layout)
  chatSearchOpen = $state(false);
  // image viewer: the images it can switch between and the one shown
  lightbox = $state<{ items: LightboxItem[]; index: number } | null>(null);
  confirmRequest = $state<ConfirmRequest | null>(null);
  hoverMessage: Message | null = null; // under the mouse, for Ctrl+C without a selection (not reactive)
  menu = $state<{ x: number; y: number; items: MenuItem[]; leftOf?: boolean } | null>(null); // leftOf: x is the right edge
  reply = $state<Message | null>(null);
  edit = $state<Message | null>(null);
  forward = $state<Message | null>(null); // message being forwarded (dialog open)
  shortcuts = $state(false); // keyboard shortcuts overview open
  media = $state<{ chatKey: string } | null>(null); // shared media dialog: of a chat, or all ('')
  jumpRequest = $state<{ chatKey: string; msgId: string } | null>(null); // scroll the conversation to a message
  highlightId = $state('');
  dropHover = $state(false);
  focusComposer = $state(0); // bump to focus
  recordingKeys = $state(false); // a shortcut is being recorded in Settings
  focusSearch = $state(0);
  textSizeNotice = $state(0);
  private textSizeNoticeTimer: ReturnType<typeof setTimeout> | undefined;
  // the chat list's keyboard highlight (Alt+J/K, arrows): a chat key, or a
  // message search result ("m\u0002" + chat key + "\u0001" + id); opened with
  // Enter. Shown while the chat list or its search has the focus.
  listCursor = $state('');
  // set by the mounted chat list
  // move: highlight the next / previous; focus: at the open chat; first: a
  // light highlight on the first chat (after a "go to")
  list: { move(delta: number): void; focus(): void; first(): void } | null = null;

  get narrow(): boolean {
    return this.width < 900;
  }

  get wide(): boolean {
    return this.width >= 1280;
  }

  confirm(title: string, body: string, confirm: string, danger = false): Promise<boolean> {
    return new Promise((resolve) => {
      this.confirmRequest = { title, body, confirm, danger, resolve };
    });
  }

  openMenu(e: MouseEvent | { x: number; y: number }, items: MenuItem[], leftOf = false) {
    const x = 'clientX' in e ? e.clientX : e.x;
    const y = 'clientY' in e ? e.clientY : e.y;
    this.menu = { x, y, items, leftOf };
  }

  showTextSize(size: number) {
    this.textSizeNotice = size;
    clearTimeout(this.textSizeNoticeTimer);
    this.textSizeNoticeTimer = setTimeout(() => (this.textSizeNotice = 0), 5000);
  }

  highlight(id: string) {
    this.highlightId = id;
    setTimeout(() => {
      if (this.highlightId === id) this.highlightId = '';
    }, 1600);
  }
}

export const ui = new Ui();

// Opens the image viewer on one of several images (paths of local files).
export function openLightbox(items: LightboxItem[], path: string) {
  const index = Math.max(0, items.findIndex((it) => it.path === path));
  ui.lightbox = { items: items.length ? items : [{ path, name: path.split('/').pop() ?? '' }], index };
}
