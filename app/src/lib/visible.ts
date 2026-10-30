// visible.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

import { store } from './store.svelte';

const ORDER = ['Telegram', 'WhatsAppMd', 'Signal', 'Matrix', 'Dummy'];

// The Dummy test protocol is shown in development builds, or when enabled with
// localStorage 'braids.dev' = '1'.
export function showTestProtocols(): boolean {
  try {
    return import.meta.env.DEV || localStorage.getItem('braids.dev') === '1';
  } catch {
    return import.meta.env.DEV;
  }
}

// Protocols the daemon was built with, in display order.
export function availableProtocols(): string[] {
  return store.protocols
    .filter((p) => p !== 'Dummy' || showTestProtocols())
    .sort((a, b) => (ORDER.indexOf(a) + 1 || 99) - (ORDER.indexOf(b) + 1 || 99));
}
