<!--
  InviteBar.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Shown instead of the composer in a room you are invited to (Matrix): join
  it, or decline (the invite is rejected and the room removed).
-->
<script lang="ts">
  import { LoaderCircle } from '@lucide/svelte';
  import * as api from '../lib/api';
  import { store } from '../lib/store.svelte';
  import type { Chat } from '../lib/types';

  let { chat }: { chat: Chat } = $props();
  let busy = $state<'' | 'join' | 'decline'>('');

  const inviter = $derived(chat.lastMessage && chat.lastMessage.id.startsWith('invite:') ? store.senderName(chat.lastMessage) : '');

  async function join() {
    busy = 'join';
    try {
      await store.joinChat(chat.accountId, chat.id);
    } catch (e) {
      store.toastError(e);
    } finally {
      busy = '';
    }
  }

  async function decline() {
    busy = 'decline';
    try {
      await api.call('chats.delete', { accountId: chat.accountId, chatId: chat.id });
      store.toast('Invitation declined', 'info');
    } catch (e) {
      store.toastError(e);
    } finally {
      busy = '';
    }
  }
</script>

<div class="invite-bar" role="group" aria-label="Invitation">
  <span class="text">{inviter ? `${inviter} invited you to join.` : 'You are invited to join.'}</span>
  <span class="grow"></span>
  <button class="btn" onclick={decline} disabled={!!busy}>{#if busy === 'decline'}<LoaderCircle size={16} class="spin" />{/if}Decline</button>
  <button class="btn primary" onclick={join} disabled={!!busy}>{#if busy === 'join'}<LoaderCircle size={16} class="spin" />{/if}Join</button>
</div>

<style>
  .invite-bar {
    display: flex; align-items: center; gap: 8px; flex-wrap: wrap; padding: 10px 16px; border-top: 1px solid var(--border);
    background: var(--bg-1); min-height: 60px;
  }
  .text { font-size: 14px; color: var(--text-2); }
  .grow { flex: 1; }
  .btn { display: inline-flex; align-items: center; gap: 6px; white-space: nowrap; }
</style>
