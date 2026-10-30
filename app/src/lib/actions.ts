// actions.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// Chat and message actions shared by the chat list, conversation and info panel.

import { Archive, ArchiveRestore, MailCheck, Star, StarOff, Trash2 } from '@lucide/svelte';
import * as api from './api';
import { store } from './store.svelte';
import type { Chat } from './types';
import { ui, type MenuItem } from './ui.svelte';

export function chatMenu(c: Chat): MenuItem[] {
  const items: MenuItem[] = [];
  if (c.isUnread) {
    items.push({
      label: 'Mark as read',
      icon: MailCheck,
      run: async () => {
        const msg = c.lastMessage;
        if (msg) await api.call('messages.markRead', { accountId: c.accountId, chatId: c.id, msgId: msg.id }).catch((e) => store.toastError(e));
      },
    });
  }
  const category = store.category(c);
  items.push({
    label: category === 'favorite' ? 'Remove from favorites' : 'Add to favorites',
    icon: category === 'favorite' ? StarOff : Star,
    run: () => store.setCategory(c, category === 'favorite' ? 'default' : 'favorite'),
  });
  items.push({
    label: category === 'archived' ? 'Unarchive' : 'Archive',
    icon: category === 'archived' ? ArchiveRestore : Archive,
    run: () => store.setCategory(c, category === 'archived' ? 'default' : 'archived'),
  });
  items.push({
    label: c.isGroup ? 'Leave group…' : 'Delete chat…',
    icon: Trash2,
    danger: true,
    run: async () => {
      const name = store.chatName(c);
      const ok = await ui.confirm(
        c.isGroup ? `Leave “${name}”?` : `Delete chat with ${name}?`,
        c.isGroup ? 'You will stop receiving messages from this group.' : 'The chat and its history are removed from this account.',
        c.isGroup ? 'Leave' : 'Delete',
        true,
      );
      if (ok) await api.call('chats.delete', { accountId: c.accountId, chatId: c.id }).catch((e) => store.toastError(e));
    },
  });
  return items;
}
