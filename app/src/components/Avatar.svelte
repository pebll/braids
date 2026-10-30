<!--
  Avatar.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.
-->
<script lang="ts">
  import * as api from '../lib/api';
  import { avatarColor, initials } from '../lib/format';
  import { store } from '../lib/store.svelte';
  import ProtocolBadge from './ProtocolBadge.svelte';

  let {
    name,
    id,
    size = 44,
    accountId = '',
    online = false,
    ring = 'var(--bg-1)',
    group = false,
    owner = '',
  }: {
    name: string;
    id: string;
    size?: number;
    accountId?: string; // shows the protocol badge of this account
    online?: boolean;
    ring?: string;
    group?: boolean;
    owner?: string; // account whose picture of id is shown (default: accountId)
  } = $props();

  const pictureAccount = $derived(owner || accountId);
  const picture = $derived(pictureAccount && id ? store.avatar(pictureAccount, id) : '');
  let failed = $state('');

  const account = $derived(accountId ? store.accounts[accountId] : undefined);
  const badge = $derived(size >= 40 ? 18 : size >= 30 ? 14 : 12);
</script>

<span class="avatar" style:width="{size}px" style:height="{size}px">
  <span
    class="circle"
    class:group
    style:background={avatarColor(id || name)}
    style:font-size="{Math.round(size * (size >= 40 ? 0.39 : 0.37))}px"
    aria-hidden="true"
    >{#if picture && failed !== picture}<img src={api.fileUrl(picture)} alt="" draggable="false" onerror={() => (failed = picture)} />{:else}{initials(name)}{/if}</span
  >
  {#if account}
    <span class="badge"><ProtocolBadge protocol={account.protocol} color={store.accountColor(account.accountId)} size={badge} {ring} /></span>
  {/if}
  {#if online}
    <span class="online" style:box-shadow="0 0 0 2px {ring}" title="Online"></span>
  {/if}
</span>

<style>
  .avatar { position: relative; flex-shrink: 0; display: block; }
  .circle {
    width: 100%; height: 100%; border-radius: 50%; overflow: hidden; color: #1c1917; font-weight: 600; display: flex;
    align-items: center; justify-content: center; letter-spacing: 0.01em; user-select: none;
  }
  /* groups: rounded square, people: circle */
  .circle.group { border-radius: 30%; }
  .circle img { width: 100%; height: 100%; object-fit: cover; display: block; }
  .badge { position: absolute; right: -2px; bottom: -2px; display: flex; }
  .online { position: absolute; right: 0; top: 0; width: 10px; height: 10px; border-radius: 50%; background: var(--success); }
</style>
