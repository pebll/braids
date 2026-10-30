<!--
  AuthStep.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

  One login step from braidsd (doc/API.md "Login flows"): an input field, a
  QR code to scan, or an info message. Used by the wizard and for prompts of
  existing accounts (e.g. Telegram re-login, Matrix recovery key).
-->
<script lang="ts">
  import { CircleAlert, Eye, EyeOff, Info, KeyRound, LoaderCircle } from '@lucide/svelte';
  import { tick } from 'svelte';
  import { FIELD_INFO, protocolMeta } from '../lib/protocols';
  import type { AuthPrompt } from '../lib/types';
  import Qr from './Qr.svelte';

  let {
    protocol,
    prompt,
    error = '',
    working = false,
    onSubmit,
    onCancel,
    cancelLabel = 'Cancel',
  }: {
    protocol: string;
    prompt: AuthPrompt;
    error?: string;
    working?: boolean;
    onSubmit: (value: string) => void;
    onCancel: () => void;
    cancelLabel?: string;
  } = $props();

  let value = $state('');
  let reveal = $state(false);
  let input: HTMLInputElement | HTMLTextAreaElement | undefined = $state();
  let lastKey = '';

  const info = $derived(FIELD_INFO[prompt.field] ?? { label: prompt.message || prompt.field });
  const meta = $derived(protocolMeta(protocol));
  const isRecovery = $derived(prompt.field === 'recoveryKey');

  // new field: reset the value (default from data) and focus
  $effect(() => {
    const key = `${prompt.step}:${prompt.field}:${prompt.message}`;
    if (key !== lastKey) {
      lastKey = key;
      value = prompt.step === 'input' && !prompt.isSecret ? prompt.data : '';
      reveal = false;
      tick().then(() => input?.focus());
    }
  });

  // re-focus after an error (e.g. wrong code)
  $effect(() => {
    if (error && !working) tick().then(() => input?.focus());
  });

  function submit(e?: Event) {
    e?.preventDefault();
    if (working) return;
    if (!value.trim() && !prompt.isOptional) return;
    onSubmit(isRecovery ? value.trim() : value);
  }
</script>

{#if prompt.step === 'qr'}
  <div class="qr-step">
    <div class="qr-box"><Qr data={prompt.data} /></div>
    <ol>
      {#each meta.qrSteps ?? [prompt.message] as step, i}
        <li><span class="num">{i + 1}</span><span>{@html step}</span></li>
      {/each}
    </ol>
  </div>
  <div class="waiting"><LoaderCircle size={14} class="spin" />Waiting for your phone… a new code appears automatically when this one expires.</div>
  {#if meta.qrSteps && prompt.message}<p class="hint small">{prompt.message}</p>{/if}
  <div class="foot">
    <button class="btn ghost" onclick={onCancel}>{cancelLabel}</button>
  </div>
{:else if prompt.step === 'info'}
  <div class="banner info"><Info size={18} /><span>{prompt.message}{#if prompt.data}<br /><b class="code">{prompt.data}</b>{/if}</span></div>
  <div class="waiting"><LoaderCircle size={14} class="spin" />Working…</div>
  <div class="foot"><button class="btn ghost" onclick={onCancel}>{cancelLabel}</button></div>
{:else if prompt.step === 'input'}
  <form onsubmit={submit} class="form">
    {#if isRecovery}
      <div class="banner info">
        <KeyRound size={18} />
        <span><b>What is this?</b> Your recovery key unlocks older encrypted messages on this new device. It was shown when you set up secure backup — it looks like four-letter groups.</span>
      </div>
    {/if}
    <div class="field">
      <label for="auth-input">{info.label}</label>
      {#if isRecovery}
        <textarea
          id="auth-input"
          class="input mono"
          class:invalid={!!error}
          rows="2"
          bind:this={input}
          bind:value
          placeholder={info.placeholder}
          spellcheck="false"
          autocomplete="off"
          onkeydown={(e) => { if (e.key === 'Enter' && !e.shiftKey) submit(e); }}
        ></textarea>
      {:else if prompt.isSecret}
        <div class="input-wrap">
          <input
            id="auth-input"
            class="input"
            class:mono={info.mono}
            class:invalid={!!error}
            type={reveal ? 'text' : 'password'}
            bind:this={input}
            bind:value
            placeholder={info.placeholder}
            autocomplete={(info.autocomplete ?? 'off') as any}
            spellcheck="false"
          />
          <button type="button" class="icon-btn sm" aria-label={reveal ? 'Hide' : 'Show'} onclick={() => (reveal = !reveal)}>
            {#if reveal}<EyeOff size={16} />{:else}<Eye size={16} />{/if}
          </button>
        </div>
      {:else}
        <input
          id="auth-input"
          class="input"
          class:mono={info.mono}
          class:invalid={!!error}
          bind:this={input}
          bind:value
          placeholder={info.placeholder ?? ''}
          autocomplete={(info.autocomplete ?? 'off') as any}
          spellcheck="false"
          inputmode={prompt.field === 'code' ? 'numeric' : prompt.field === 'phone' ? 'tel' : undefined}
        />
      {/if}
      {#if error}
        <span class="error"><CircleAlert size={14} />{error}</span>
      {/if}
      {#if prompt.message && prompt.message !== info.label}
        <span class="hint">{prompt.message}</span>
      {/if}
      {#if info.hint && !(prompt.message && prompt.message !== info.label)}<span class="hint">{info.hint}</span>{/if}
      {#if isRecovery}
        <span class="hint">Don’t have it? Skip for now — older encrypted messages will show as “waiting for keys”. Type <code>never</code> to stop being asked.</span>
      {/if}
    </div>
    <div class="foot">
      <button type="button" class="btn ghost" onclick={onCancel}>{cancelLabel}</button>
      {#if prompt.isOptional && !value}
        <button type="submit" class="btn" disabled={working}>{isRecovery ? 'Skip for now' : 'Skip'}</button>
      {:else}
        <button type="submit" class="btn primary" class:working disabled={working || (!value.trim() && !prompt.isOptional)}>
          {#if working}<LoaderCircle size={16} class="spin" />Connecting…{:else}{isRecovery ? 'Unlock' : 'Continue'}{/if}
        </button>
      {/if}
    </div>
  </form>
{/if}

<style>
  .form { display: flex; flex-direction: column; gap: 16px; }
  .qr-step { display: flex; gap: 20px; align-items: flex-start; flex-wrap: wrap; }
  .qr-box { padding: 12px; border: 1px solid var(--border); border-radius: var(--radius-lg); background: #fff; }
  ol { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 10px; flex: 1; min-width: 180px; font-size: 14px; }
  li { display: flex; gap: 10px; align-items: flex-start; }
  .num {
    width: 20px; height: 20px; border-radius: 50%; background: var(--accent-soft); color: var(--accent); font-size: 11px; font-weight: 700;
    display: flex; align-items: center; justify-content: center; flex-shrink: 0;
  }
  .waiting { display: flex; align-items: center; gap: 8px; font-size: 13px; color: var(--text-muted); }
  .hint.small { margin: 0; font-size: 12px; color: var(--text-muted); }
  .code { font-family: var(--mono); font-size: 20px; letter-spacing: 0.12em; display: inline-block; margin-top: 6px; }
  .foot { display: flex; justify-content: flex-end; gap: 8px; margin: 4px -20px -20px; padding: 14px 20px; border-top: 1px solid var(--border); }
  code { font-family: var(--mono); font-size: 12px; }
</style>
