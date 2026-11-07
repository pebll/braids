<!--
  PollDialog.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  Create a poll in the open chat: a question, 2 to 12 options (a new empty
  one appears while typing in the last), single or multiple choice.
-->
<script lang="ts">
  import { Plus, X } from '@lucide/svelte';
  import { onMount, tick } from 'svelte';
  import * as api from '../lib/api';
  import { splitKey, store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';

  let { chatKey }: { chatKey: string } = $props();

  const MAX = 12;
  let question = $state('');
  let options = $state(['', '']);
  let multiple = $state(false);
  let busy = $state(false);
  let questionInput: HTMLInputElement | undefined = $state();
  let list: HTMLDivElement | undefined = $state();

  const filled = $derived(options.map((o) => o.trim()).filter(Boolean));
  const valid = $derived(!!question.trim() && filled.length >= 2);

  onMount(() => questionInput?.focus());

  function close() {
    ui.pollFor = '';
    ui.focusComposer++;
  }

  // a new empty option once the last one is used
  $effect(() => {
    if (options[options.length - 1].trim() && options.length < MAX) options.push('');
  });

  async function addOption() {
    if (options.length >= MAX) return;
    options.push('');
    await tick();
    list?.querySelectorAll('input')[options.length - 1]?.focus();
  }

  function removeOption(i: number) {
    options.splice(i, 1);
    if (options.length < 2) options.push('');
  }

  async function send() {
    if (!valid || busy) return;
    busy = true;
    const [accountId, chatId] = splitKey(chatKey);
    try {
      await api.call('messages.send', { accountId, chatId, poll: { question: question.trim(), options: filled, multiple } });
      close();
    } catch (e) {
      store.toastError(e);
    } finally {
      busy = false;
    }
  }

  function onKey(e: KeyboardEvent) {
    if (e.key === 'Escape') {
      e.preventDefault();
      close();
    } else if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      send();
    }
  }
</script>

<svelte:window onkeydown={onKey} />

<div class="scrim" role="presentation" onclick={(e) => e.target === e.currentTarget && close()}>
  <div class="dialog" role="dialog" aria-modal="true" aria-labelledby="poll-title">
    <div class="dialog-head">
      <h2 id="poll-title">New poll</h2>
      <button class="icon-btn" aria-label="Close" onclick={close}><X size={18} /></button>
    </div>
    <div class="dialog-body">
      <div class="field">
        <label for="poll-question">Question</label>
        <input id="poll-question" class="input" bind:this={questionInput} bind:value={question} maxlength="255" placeholder="Ask something" />
      </div>
      <div class="field">
        <span class="label">Options</span>
        <div class="options" bind:this={list}>
          {#each options as _, i (i)}
            <div class="opt">
              <input class="input" aria-label={`Option ${i + 1}`} bind:value={options[i]} maxlength="100" placeholder={`Option ${i + 1}`}
                onkeydown={(e) => { if (e.key === 'Enter' && !e.ctrlKey && !e.metaKey) { e.preventDefault(); (e.currentTarget.parentElement?.nextElementSibling?.querySelector('input') as HTMLInputElement | null)?.focus(); } }} />
              {#if options.length > 2}
                <button class="icon-btn sm" aria-label={`Remove option ${i + 1}`} onclick={() => removeOption(i)}><X size={16} /></button>
              {/if}
            </div>
          {/each}
        </div>
        {#if options.length < MAX}
          <button class="add" onclick={addOption}><Plus size={16} />Add an option</button>
        {/if}
      </div>
      <label class="check"><input type="checkbox" bind:checked={multiple} />Allow multiple answers</label>
    </div>
    <div class="dialog-foot">
      <button class="btn" onclick={close}>Cancel</button>
      <button class="btn primary" class:working={busy} disabled={!valid || busy} onclick={send}>Send poll</button>
    </div>
  </div>
</div>

<style>
  .label { font-size: 13px; font-weight: 600; color: var(--text); }
  .options { display: flex; flex-direction: column; gap: 6px; max-height: 320px; overflow-y: auto; }
  .opt { display: flex; align-items: center; gap: 6px; }
  .opt .input { flex: 1; }
  .add { align-self: flex-start; display: inline-flex; align-items: center; gap: 6px; border: 0; background: none; padding: 4px 0; color: var(--accent); font-size: 13px; }
  .add:hover { text-decoration: underline; }
  .check { display: flex; align-items: center; gap: 8px; font-size: 14px; }
</style>
