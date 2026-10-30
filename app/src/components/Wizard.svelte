<!--
  Wizard.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Add-account modal: 1 choose protocol, 2 the protocol's login steps (from
  account.auth events), 3 syncing. Closing it cancels a pending setup.
-->
<script lang="ts">
  import { ChevronLeft, ChevronRight, CircleAlert, LoaderCircle, Lock, RotateCw, TriangleAlert, X } from '@lucide/svelte';
  import { protocolMeta, stepTitle } from '../lib/protocols';
  import { store } from '../lib/store.svelte';
  import { availableProtocols } from '../lib/visible';
  import AuthStep from './AuthStep.svelte';
  import Braid from './Braid.svelte';
  import ProtocolBadge from './ProtocolBadge.svelte';

  const id = $derived(store.wizardSetupId);
  const setup = $derived(id && id !== 'new' ? store.setups[id] : undefined);
  const stage = $derived(!setup ? 1 : setup.done === 'added' ? 3 : 2);
  const meta = $derived(setup ? protocolMeta(setup.protocol) : undefined);
  const chatCount = $derived(setup?.accountId ? Object.values(store.chats).filter((c) => c.accountId === setup.accountId).length : 0);
  const syncDone = $derived(!!setup?.accountId && !!store.connectedAccounts[setup.accountId]);
  let starting = $state('');

  const title = $derived.by(() => {
    if (!setup) return 'Add an account';
    if (setup.done === 'failed') return `Couldn’t connect ${meta!.name}`;
    if (setup.done === 'added') return `${meta!.name} is connected`;
    const p = setup.prompt;
    if (p?.step === 'qr') return meta!.linkTitle ?? `Link ${meta!.name}`;
    if (p?.step === 'input') return stepTitle(setup.protocol, p.field);
    return `Connect ${meta!.name}`;
  });

  async function choose(protocol: string) {
    if (starting) return;
    starting = protocol;
    try {
      store.wizardSetupId = await store.addAccount(protocol);
    } catch (e) {
      store.toastError(e);
    } finally {
      starting = '';
    }
  }

  async function close() {
    if (setup && setup.done === 'no') await store.authCancel({ setupId: setup.setupId });
    store.wizardSetupId = '';
  }

  async function back() {
    if (setup && setup.done === 'no') await store.authCancel({ setupId: setup.setupId });
    store.wizardSetupId = 'new';
  }

  function retry() {
    if (setup) choose(setup.protocol);
  }

  function finish() {
    if (setup?.accountId) store.filter = { kind: 'all' };
    store.wizardSetupId = '';
  }

  function onKey(e: KeyboardEvent) {
    if (e.key === 'Escape') close();
  }
</script>

<svelte:window onkeydown={onKey} />

<div class="scrim" role="presentation">
  <div class="dialog wizard" role="dialog" aria-modal="true" aria-labelledby="wizard-title">
    <div class="top">
      {#if stage === 2}
        <button class="icon-btn sm" aria-label="Back" onclick={back}><ChevronLeft size={18} /></button>
      {:else}
        <span class="w28"></span>
      {/if}
      <div class="steps" aria-label="Step {stage} of 3">
        {#each [1, 2, 3] as s}<span class:on={s === stage} class:done={s < stage}></span>{/each}
      </div>
      <button class="icon-btn sm" aria-label="Close" onclick={close}><X size={18} /></button>
    </div>

    <div class="dialog-head"><h2 id="wizard-title">{title}</h2></div>

    <div class="dialog-body">
      {#if stage === 1}
        <p class="sub">Choose the messenger you want to connect.</p>
        <div class="protocols">
          {#each availableProtocols() as p}
            {@const pm = protocolMeta(p)}
            <button class="proto" onclick={() => choose(p)} disabled={!!starting}>
              <ProtocolBadge protocol={p} color={`var(--acct-${pm.color})`} size={36} />
              <span class="proto-text">
                <b>{pm.name}</b>
                <span>{pm.description}</span>
                {#if pm.warning}<span class="warn"><TriangleAlert size={13} />{pm.warning}</span>{/if}
              </span>
              {#if starting === p}<LoaderCircle size={18} class="spin" />{:else}<ChevronRight size={18} />{/if}
            </button>
          {/each}
        </div>
        <p class="note"><Lock size={13} />Credentials and messages are stored only on this computer.</p>
      {:else if setup && setup.done === 'failed'}
        <div class="banner danger"><CircleAlert size={18} /><span><b>{setup.failMessage}</b>{#if setup.error && setup.error !== setup.failMessage}<br />{setup.error}{/if}</span></div>
        <div class="foot">
          <button class="btn ghost" onclick={() => (store.wizardSetupId = '')}>Close</button>
          <button class="btn primary" onclick={retry}><RotateCw size={16} />Try again</button>
        </div>
      {:else if stage === 3 && setup}
        <p class="sub">{syncDone ? 'Your chats are ready.' : 'Syncing your chats for the first time.'}</p>
        <div class="sync">
          <Braid width={360} height={44} animated={!syncDone} />
          <div class="bar"><span style:width="{syncDone ? 100 : Math.min(90, 15 + chatCount * 3)}%"></span></div>
          <p class="hint">
            {chatCount ? `${chatCount} chats so far` : 'Fetching chats…'} · you can start chatting while older history loads
          </p>
        </div>
        <div class="foot">
          <button class="btn primary" onclick={finish}>Done — go to inbox</button>
        </div>
      {:else if setup}
        {#if setup.prompt && setup.prompt.step !== 'error'}
          {#if setup.prompt.step === 'qr' && meta?.linkHint}<p class="sub">{meta.linkHint}</p>{/if}
          {#if setup.error && setup.prompt.step !== 'input'}
            <div class="banner danger"><CircleAlert size={18} /><span>{setup.error}</span></div>
          {/if}
          <AuthStep protocol={setup.protocol} prompt={setup.prompt} error={setup.prompt.step === 'input' ? setup.error : ''} working={setup.working}
            onSubmit={(v) => store.authInput({ setupId: setup.setupId }, setup.prompt!.field, v)} onCancel={close} />
        {:else}
          {#if setup.error}<div class="banner danger"><CircleAlert size={18} /><span>{setup.error}</span></div>{/if}
          <div class="connecting"><LoaderCircle size={18} class="spin" />Connecting to {meta?.name}…</div>
          <div class="foot"><button class="btn ghost" onclick={close}>Cancel</button></div>
        {/if}
      {/if}
    </div>
  </div>
</div>

<style>
  .wizard { width: 460px; }
  .top { display: flex; align-items: center; justify-content: space-between; padding: 14px 14px 0; }
  .w28 { width: 28px; }
  .steps { display: flex; gap: 5px; }
  .steps span { width: 6px; height: 6px; border-radius: 3px; background: var(--border-strong); transition: width var(--duration-base) var(--ease); }
  .steps span.on { width: 18px; background: var(--accent); }
  .steps span.done { background: var(--accent); }
  .dialog-head { padding-top: 10px; }
  .protocols { display: flex; flex-direction: column; gap: 8px; }
  .proto {
    display: flex; align-items: center; gap: 12px; padding: 12px; border-radius: var(--radius-lg); border: 1px solid var(--border);
    background: var(--surface); text-align: left; box-shadow: var(--shadow-1); color: var(--text-2);
  }
  .proto:hover { border-color: var(--border-strong); background: var(--surface-2); }
  .proto-text { flex: 1; display: flex; flex-direction: column; gap: 1px; font-size: 13px; color: var(--text-2); }
  .proto-text b { font-size: 14px; color: var(--text); }
  .warn { display: flex; gap: 5px; align-items: flex-start; color: var(--warning); font-size: 12px; margin-top: 3px; line-height: 16px; }
  .warn :global(svg) { margin-top: 1px; flex-shrink: 0; }
  .note { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--text-muted); margin: 0; }
  .sync { display: flex; flex-direction: column; align-items: center; gap: 14px; padding: 8px 0; overflow: hidden; }
  .bar { width: 100%; height: 4px; border-radius: 2px; background: var(--border); overflow: hidden; }
  .bar span { display: block; height: 100%; background: var(--accent); border-radius: 2px; transition: width var(--duration-slow) var(--ease); }
  .hint { font-size: 12px; color: var(--text-muted); text-align: center; margin: 0; }
  .connecting { display: flex; align-items: center; gap: 10px; color: var(--text-2); padding: 12px 0; }
  .foot { display: flex; justify-content: flex-end; gap: 8px; margin: 4px -20px -20px; padding: 14px 20px; border-top: 1px solid var(--border); }
</style>
