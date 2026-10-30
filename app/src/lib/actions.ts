// actions.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// Chat and message actions shared by the chat list, conversation and info panel.

import { Archive, ArchiveRestore, MailCheck, Pin, PinOff, Trash2 } from '@lucide/svelte';
import * as api from './api';
import { store } from './store.svelte';
import type { Chat } from './types';
import { ui, type MenuItem } from './ui.svelte';

export function chatMenu(c: Chat): MenuItem[] {
  const caps = store.accounts[c.accountId]?.capabilities;
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
  if (caps?.pinChat) {
    items.push({
      label: c.isPinned ? 'Unpin' : 'Pin',
      icon: c.isPinned ? PinOff : Pin,
      run: () => api.call('chats.pin', { accountId: c.accountId, chatId: c.id, isPinned: !c.isPinned }).catch((e) => store.toastError(e)),
    });
  }
  if (caps?.archiveChat) {
    items.push({
      label: c.isArchived ? 'Unarchive' : 'Archive',
      icon: c.isArchived ? ArchiveRestore : Archive,
      run: () => api.call('chats.archive', { accountId: c.accountId, chatId: c.id, isArchived: !c.isArchived }).catch((e) => store.toastError(e)),
    });
  }
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
