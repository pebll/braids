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

class Ui {
  width = $state(typeof window !== 'undefined' ? window.innerWidth : 1280);
  infoOpen = $state(false);
  settingsOpen = $state(false);
  settingsSection = $state('general');
  settingsAccount = $state('');
  newChatOpen = $state(false);
  drawerOpen = $state(false); // sidebar as drawer (narrow layout)
  chatSearchOpen = $state(false);
  lightbox = $state<{ src: string; name: string; path: string } | null>(null);
  confirmRequest = $state<ConfirmRequest | null>(null);
  menu = $state<{ x: number; y: number; items: MenuItem[] } | null>(null);
  reply = $state<Message | null>(null);
  edit = $state<Message | null>(null);
  highlightId = $state('');
  dropHover = $state(false);
  focusComposer = $state(0); // bump to focus
  focusSearch = $state(0);

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

  openMenu(e: MouseEvent | { x: number; y: number }, items: MenuItem[]) {
    const x = 'clientX' in e ? e.clientX : e.x;
    const y = 'clientY' in e ? e.clientY : e.y;
    this.menu = { x, y, items };
  }

  highlight(id: string) {
    this.highlightId = id;
    setTimeout(() => {
      if (this.highlightId === id) this.highlightId = '';
    }, 1600);
  }
}

export const ui = new Ui();
