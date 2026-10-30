// theme.svelte.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

import { settings } from './settings.svelte';

class ThemeState {
  systemDark = $state(false);

  get dark(): boolean {
    return settings.theme === 'dark' || (settings.theme === 'system' && this.systemDark);
  }

  init() {
    const mq = window.matchMedia('(prefers-color-scheme: dark)');
    this.systemDark = mq.matches;
    mq.addEventListener('change', (e) => (this.systemDark = e.matches));
    $effect.root(() => {
      $effect(() => {
        const root = document.documentElement;
        root.classList.toggle('t-dark', this.dark);
        root.classList.toggle('t-light', !this.dark);
      });
    });
  }
}

export const theme = new ThemeState();
