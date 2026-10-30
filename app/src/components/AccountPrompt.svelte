<!--
  AccountPrompt.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  A login step requested by an existing account at runtime (e.g. Telegram
  re-authentication, Matrix recovery key on an unverified device).
-->
<script lang="ts">
  import { protocolMeta, stepTitle } from '../lib/protocols';
  import { store } from '../lib/store.svelte';
  import type { AuthPrompt } from '../lib/types';
  import AuthStep from './AuthStep.svelte';

  let { prompt }: { prompt: AuthPrompt } = $props();
  const accountId = $derived(prompt.accountId!);
  const protocol = $derived(store.accounts[accountId]?.protocol ?? '');
  let working = $state(false);

  $effect(() => {
    prompt; // a new prompt ends the wait
    working = false;
  });
</script>

<div class="scrim" role="presentation">
  <div class="dialog" role="dialog" aria-modal="true" aria-labelledby="prompt-title">
    <div class="dialog-head">
      <h2 id="prompt-title">{prompt.step === 'qr' ? `Link ${protocolMeta(protocol).name} again` : stepTitle(protocol, prompt.field)}</h2>
    </div>
    <div class="dialog-body">
      <p class="sub">{store.accountName(accountId)} needs your input.</p>
      <AuthStep {protocol} {prompt} {working} cancelLabel="Not now"
        onSubmit={(v) => { working = true; store.authInput({ accountId }, prompt.field, v); }}
        onCancel={() => store.authCancel({ accountId })} />
    </div>
  </div>
</div>
