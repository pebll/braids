// settings.svelte.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// GUI settings, persisted per machine in localStorage (the daemon does not
// know about them).

export type Theme = 'system' | 'light' | 'dark';
export type TextSize = number;

// chat category set in braids; without one, a chat archived in the
// messenger counts as archived, else as default
export type ChatCategory = 'favorite' | 'default' | 'archived';

export interface AccountMeta {
  name?: string;
  color?: string;
  notify?: boolean;
}

const KEY = 'braids.settings.v1';

class Settings {
  theme = $state<Theme>('system');
  textSize = $state<TextSize>(100);
  notifications = $state(true);
  notificationPreview = $state(true);
  closeToTray = $state(true);
  sidebarCollapsed = $state(false);
  enterSends = $state(true);
  linkPreviews = $state(true); // fetch the title and image of linked pages
  checkUpdates = $state(true); // ask GitHub for a newer release once a day
  chatListWidth = $state(330); // px, dragged at the divider
  // favorites: their own folder in the sidebar, pinned at the top of the
  // chat lists, or both
  favorites = $state<'folder' | 'top' | 'both'>('folder');
  accounts = $state<Record<string, AccountMeta>>({});
  categories = $state<Record<string, ChatCategory>>({}); // chat key -> category
  keybinds = $state<Partial<Record<string, string[]>>>({}); // action -> combos, only changed ones (keybinds.svelte.ts)

  constructor() {
    try {
      const saved = JSON.parse(localStorage.getItem(KEY) ?? '{}');
      for (const k of ['theme', 'textSize', 'notifications', 'notificationPreview', 'closeToTray', 'sidebarCollapsed', 'enterSends', 'linkPreviews', 'checkUpdates', 'chatListWidth', 'favorites', 'accounts', 'categories', 'keybinds'] as const) {
        if (saved[k] !== undefined && (k !== 'textSize' || Number.isFinite(saved[k]) && saved[k] >= 50 && saved[k] <= 200)) (this as any)[k] = saved[k];
      }
    } catch {
      // defaults
    }
  }

  save() {
    try {
      localStorage.setItem(KEY, JSON.stringify({
        theme: this.theme,
        textSize: this.textSize,
        notifications: this.notifications,
        notificationPreview: this.notificationPreview,
        closeToTray: this.closeToTray,
        sidebarCollapsed: this.sidebarCollapsed,
        enterSends: this.enterSends,
        linkPreviews: this.linkPreviews,
        checkUpdates: this.checkUpdates,
        chatListWidth: this.chatListWidth,
        favorites: this.favorites,
        accounts: this.accounts,
        categories: this.categories,
        keybinds: this.keybinds,
      }));
    } catch {
      // storage unavailable: settings last for this session
    }
  }

  account(accountId: string): AccountMeta {
    if (!this.accounts[accountId]) this.accounts[accountId] = {};
    return this.accounts[accountId];
  }
}

export const settings = new Settings();
