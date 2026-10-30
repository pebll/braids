// settings.svelte.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// GUI settings, persisted per machine in localStorage (the daemon does not
// know about them).

export type Theme = 'system' | 'light' | 'dark';

export interface AccountMeta {
  name?: string;
  color?: string;
  notify?: boolean;
}

const KEY = 'braids.settings.v1';

class Settings {
  theme = $state<Theme>('system');
  notifications = $state(true);
  notificationPreview = $state(true);
  closeToTray = $state(true);
  sidebarCollapsed = $state(false);
  enterSends = $state(true);
  accounts = $state<Record<string, AccountMeta>>({});

  constructor() {
    try {
      const saved = JSON.parse(localStorage.getItem(KEY) ?? '{}');
      for (const k of ['theme', 'notifications', 'notificationPreview', 'closeToTray', 'sidebarCollapsed', 'enterSends', 'accounts'] as const) {
        if (saved[k] !== undefined) (this as any)[k] = saved[k];
      }
    } catch {
      // defaults
    }
  }

  save() {
    try {
      localStorage.setItem(KEY, JSON.stringify({
        theme: this.theme,
        notifications: this.notifications,
        notificationPreview: this.notificationPreview,
        closeToTray: this.closeToTray,
        sidebarCollapsed: this.sidebarCollapsed,
        enterSends: this.enterSends,
        accounts: this.accounts,
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
