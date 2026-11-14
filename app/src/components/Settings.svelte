<!--
  Settings.svelte

  Copyright (c) 2026 braids contributors

  braids is distributed under the GNU AGPL v3 license, see LICENSE for details.
-->
<script lang="ts">
  import { Bell, Bug, ChevronLeft, Info, Keyboard, Lightbulb, Monitor, Moon, Pencil, Plus, RotateCcw, Settings as SettingsIcon, Sun, Trash2, Users, X } from '@lucide/svelte';
  import * as api from '../lib/api';
  import { ACCOUNT_COLORS, protocolMeta } from '../lib/protocols';
  import {
    ACTIONS, GROUPS, addBinding, bindings, comboKeys, comboLabel, comboOf, isDefault, isMac, isUsableCombo, removeBinding, resetBindings, type Action,
  } from '../lib/keybinds.svelte';
  import { settings, type TextSize, type Theme } from '../lib/settings.svelte';
  import { store } from '../lib/store.svelte';
  import { ui } from '../lib/ui.svelte';
  import Logo from './Logo.svelte';
  import ProtocolBadge from './ProtocolBadge.svelte';

  const ISSUES = 'https://github.com/pebll/braids/issues';
  const SECTIONS = [
    { id: 'general', label: 'General', icon: SettingsIcon },
    { id: 'notifications', label: 'Notifications', icon: Bell },
    { id: 'keyboard', label: 'Keyboard', icon: Keyboard },
    { id: 'accounts', label: 'Accounts', icon: Users },
    { id: 'about', label: 'About', icon: Info },
  ];

  const mod = isMac ? '⌘' : 'Ctrl';

  // shortcut recording: the next key combo is added to this action
  let recording = $state<Action | ''>('');
  let recordError = $state('');

  function startRecording(action: Action) {
    recording = action;
    recordError = '';
    ui.recordingKeys = true;
  }

  function stopRecording() {
    recording = '';
    ui.recordingKeys = false;
  }

  function onRecordKey(e: KeyboardEvent) {
    if (!recording) return;
    e.preventDefault();
    e.stopImmediatePropagation();
    if (e.key === 'Escape') {
      stopRecording();
      return;
    }
    const combo = comboOf(e);
    if (!combo) return; // modifier alone: wait for the key
    if (!isUsableCombo(combo)) {
      recordError = `${comboLabel(combo)} would type text: use ${mod} or Alt with a key.`;
      return;
    }
    const takenFrom = addBinding(recording, combo);
    if (takenFrom) store.toast(`${comboLabel(combo)} removed from “${ACTIONS.find((a) => a.id === takenFrom)!.label}”.`);
    stopRecording();
  }

  $effect(() => {
    if (!recording) return;
    window.addEventListener('keydown', onRecordKey, true);
    return () => window.removeEventListener('keydown', onRecordKey, true);
  });

  // closing settings or leaving the section ends a recording
  $effect(() => {
    if ((!ui.settingsOpen || ui.settingsSection !== 'keyboard') && recording) stopRecording();
  });

  const account = $derived(ui.settingsAccount ? store.accounts[ui.settingsAccount] : undefined);
  let removing = $state(false);

  // start at login (the plugin's state, not a setting) and updates
  let autostart = $state(false);
  let installKind = $state('');
  let appVersion = $state('');
  let updateStatus = $state('');
  if (api.isTauri) {
    api.autostartEnabled().then((on) => (autostart = on)).catch(() => {});
    api.installKind().then((k) => (installKind = k)).catch(() => {});
    api.appVersion().then((v) => (appVersion = v)).catch(() => {});
  }

  async function toggleAutostart() {
    try {
      await api.setAutostart(!autostart);
      autostart = await api.autostartEnabled();
    } catch (e) {
      store.toastError(e);
    }
  }

  // the bug report template (.github/ISSUE_TEMPLATE/bugreport.md) with the
  // environment filled in
  async function reportBug() {
    const info = await api.systemInfo().catch(() => null);
    const protocols = [...new Set(store.accountList.map((a) => protocolMeta(a.protocol).name))].join(', ');
    const version = [info?.app, store.daemonVersion && info?.app !== store.daemonVersion ? `core ${store.daemonVersion}` : ''].filter(Boolean).join(', ') || store.daemonVersion;
    const os = [info?.os, info?.kernel && `kernel ${info.kernel}`, info?.arch, info?.desktop].filter(Boolean).join(', ');
    const body = [
      '**Description**:',
      '',
      '**How to reproduce it**:',
      '',
      '**Environment**:',
      `- braids version: ${version}`,
      `- Installed with (install script / .deb / Flatpak / AppImage / archive / Nix / source): ${info?.install ?? ''}`,
      `- OS / distro: ${os}`,
      `- Messenger(s): ${protocols}`,
      '',
      '**Log**:',
      '<!--',
      'Please attach a copy of `~/.config/braids/log.txt` after reviewing and removing',
      'any sensitive information from it.',
      '-->',
      '',
    ].join('\n');
    await api.openUrl(`${ISSUES}/new?labels=bug&body=${encodeURIComponent(body)}`);
  }

  async function checkNow() {
    updateStatus = 'Checking…';
    const u = await store.checkUpdates(true);
    updateStatus = u ? '' : 'braids is up to date.';
  }

  function set<K extends keyof typeof settings>(key: K, value: (typeof settings)[K]) {
    (settings as any)[key] = value;
    settings.save();
    if (key === 'textSize') ui.showTextSize(value as number);
    if (key === 'closeToTray') api.setCloseToTray(value as boolean);
  }

  function setAccount(field: 'name' | 'color' | 'notify', value: any) {
    if (!account) return;
    settings.account(account.accountId)[field] = value;
    settings.save();
  }

  // kept by braidsd: it leaves the messages and their new chats out
  async function setShowJoined(value: boolean) {
    if (!account) return;
    const a = account;
    try {
      await api.call('accounts.setOption', { accountId: a.accountId, name: 'showJoinedMessages', value });
      a.options = { ...a.options, showJoinedMessages: value };
    } catch (e) {
      store.toastError(e);
    }
  }

  function stateLabel(state: string): string {
    return { online: 'online', connecting: 'connecting', offline: 'offline', error: 'error' }[state] ?? state;
  }

  async function removeAccount() {
    if (!account) return;
    const name = store.accountName(account.accountId);
    const ok = await ui.confirm(
      `Remove ${name}?`,
      'Its chats and message history are deleted from this computer. The device link stays listed in your other apps until you remove it there.',
      'Remove account',
      true,
    );
    if (!ok) return;
    removing = true;
    try {
      await api.call('accounts.remove', { accountId: account.accountId });
      delete settings.accounts[account.accountId];
      settings.save();
      ui.settingsAccount = '';
    } catch (e) {
      store.toastError(e);
    } finally {
      removing = false;
    }
  }

  function close() {
    ui.settingsOpen = false;
    ui.settingsAccount = '';
  }

  const themes: { id: Theme; label: string; icon: any }[] = [
    { id: 'system', label: 'System', icon: Monitor },
    { id: 'light', label: 'Light', icon: Sun },
    { id: 'dark', label: 'Dark', icon: Moon },
  ];
  const textSizes: TextSize[] = [70, 80, 90, 100, 110, 125, 150];

  function setCustomTextSize(value: string) {
    const size = Math.round(Number(value));
    if (Number.isFinite(size) && size >= 50 && size <= 200) set('textSize', size);
  }
</script>

<svelte:window onkeydown={(e) => e.key === 'Escape' && !ui.confirmRequest && close()} />

<div class="scrim" role="presentation" onclick={(e) => e.target === e.currentTarget && close()}>
  <div class="dialog settings" role="dialog" aria-modal="true" aria-label="Settings">
    <nav>
      <div class="nav-title">Settings</div>
      {#each SECTIONS as s}
        <button class:active={ui.settingsSection === s.id} onclick={() => { ui.settingsSection = s.id; ui.settingsAccount = ''; }}>
          <s.icon size={16} />{s.label}
          {#if s.id === 'about' && store.update}<span class="nav-dot" title={`braids ${store.update.version} is available`}></span>{/if}
        </button>
      {/each}
    </nav>
    <div class="pane scroll">
      <button class="icon-btn close" aria-label="Close settings" onclick={close}><X size={18} /></button>

      {#if ui.settingsSection === 'general'}
        <h2>General</h2>
        <div class="row">
          <div class="row-text"><b>Appearance</b><span>Follows your system unless you pick one.</span></div>
          <div class="segmented" role="group" aria-label="Appearance">
            {#each themes as t}
              <button aria-pressed={settings.theme === t.id} onclick={() => set('theme', t.id)}><t.icon size={14} />{t.label}</button>
            {/each}
          </div>
        </div>
        <div class="row">
          <div class="row-text"><b>Text size</b><span>Scale the whole interface, including menus and dialogs.</span></div>
          <div class="text-size-control">
            <select aria-label="Text size preset" value={textSizes.includes(settings.textSize) ? settings.textSize : ''}
              onchange={(e) => set('textSize', Number((e.target as HTMLSelectElement).value))}>
              {#each textSizes as size}<option value={size}>{size} %</option>{/each}
            </select>
            <label class="custom-size">
              <input aria-label="Custom text size" type="number" min="50" max="200" step="1" value={settings.textSize}
                oninput={(e) => setCustomTextSize((e.target as HTMLInputElement).value)} />
              <span>%</span>
            </label>
          </div>
        </div>
        <div class="row">
          <div class="row-text"><b>Favorites</b><span>Their own folder in the sidebar, pinned at the top of every chat list, or both.</span></div>
          <div class="segmented" role="group" aria-label="Favorites">
            {#each [{ id: 'folder', label: 'Folder' }, { id: 'top', label: 'On top' }, { id: 'both', label: 'Both' }] as const as o}
              <button aria-pressed={settings.favorites === o.id} onclick={() => set('favorites', o.id)}>{o.label}</button>
            {/each}
          </div>
        </div>
        <div class="row">
          <div class="row-text"><b>Unread counts on Favorites and accounts</b><span>Grey numbers in the sidebar. All chats always shows the total.</span></div>
          <button class="toggle" role="switch" aria-checked={settings.sidebarCounts} aria-label="Unread counts on Favorites and accounts" onclick={() => set('sidebarCounts', !settings.sidebarCounts)}></button>
        </div>
        <div class="row">
          <div class="row-text"><b>Preview chats on hover</b><span>Rest the pointer on a chat to read its new messages without opening it (they stay unread).</span></div>
          <button class="toggle" role="switch" aria-checked={settings.chatPreview} aria-label="Preview chats on hover" onclick={() => set('chatPreview', !settings.chatPreview)}></button>
        </div>
        <div class="row">
          <div class="row-text"><b>Enter sends the message</b><span>Off: Ctrl+Enter sends, Enter adds a line.</span></div>
          <button class="toggle" role="switch" aria-checked={settings.enterSends} aria-label="Enter sends" onclick={() => set('enterSends', !settings.enterSends)}></button>
        </div>
        <div class="row">
          <div class="row-text"><b>Link previews</b><span>Show the title and picture of linked pages. braids loads them from this computer, so the site sees your IP address.</span></div>
          <button class="toggle" role="switch" aria-checked={settings.linkPreviews} aria-label="Link previews" onclick={() => set('linkPreviews', !settings.linkPreviews)}></button>
        </div>
        {#if api.isTauri && installKind !== 'flatpak'}
          <div class="row">
            <div class="row-text"><b>Start braids at login</b><span>In the tray, without opening the window.</span></div>
            <button class="toggle" role="switch" aria-checked={autostart} aria-label="Start at login" onclick={toggleAutostart}></button>
          </div>
        {/if}
        {#if api.isTauri}
          <div class="row">
            <div class="row-text"><b>Keep running in the tray when the window is closed</b><span>You’ll keep getting notifications.</span></div>
            <button class="toggle" role="switch" aria-checked={settings.closeToTray} aria-label="Keep running in tray" onclick={() => set('closeToTray', !settings.closeToTray)}></button>
          </div>
        {/if}
      {:else if ui.settingsSection === 'notifications'}
        <h2>Notifications</h2>
        <div class="row">
          <div class="row-text"><b>Desktop notifications</b><span>For new messages in chats that are not muted.</span></div>
          <button class="toggle" role="switch" aria-checked={settings.notifications} aria-label="Desktop notifications" onclick={() => set('notifications', !settings.notifications)}></button>
        </div>
        <div class="row">
          <div class="row-text"><b>Show message preview</b><span>Off: notifications only say “New message”.</span></div>
          <button class="toggle" role="switch" aria-checked={settings.notificationPreview} aria-label="Show message preview" disabled={!settings.notifications}
            onclick={() => set('notificationPreview', !settings.notificationPreview)}></button>
        </div>
      {:else if ui.settingsSection === 'keyboard'}
        <h2>Keyboard</h2>
        <div class="row">
          <div class="row-text"><b>Vim mode</b><span>j/k to move, i to type, Esc to leave. Coming in a later version.</span></div>
          <button class="toggle" role="switch" aria-checked="false" aria-label="Vim mode" disabled></button>
        </div>
        <div class="section shortcuts-head">
          <span>Shortcuts</span>
          <button class="btn ghost" onclick={() => resetBindings()} disabled={!Object.keys(settings.keybinds).length}><RotateCcw size={14} />Reset all</button>
        </div>
        {#each GROUPS as g (g.id)}
          <div class="group-label">{g.label}</div>
          {#each ACTIONS.filter((a) => a.group === g.id) as a (a.id)}
            <div class="row compact shortcut" data-action={a.id}>
              <span>{a.label}</span>
              <span class="keys">
                {#each bindings(a.id) as combo (combo)}
                  <span class="combo">
                    {#each comboKeys(combo) as k}<kbd>{k}</kbd>{/each}
                    <button class="remove" aria-label={`Remove ${comboLabel(combo)} from ${a.label}`} title="Remove" onclick={() => removeBinding(a.id, combo)}><X size={12} /></button>
                  </span>
                {/each}
                {#if recording === a.id}
                  <span class="recording" role="status">Press keys… <span class="hint">Esc cancels</span></span>
                {:else}
                  <button class="icon-btn sm" aria-label={`Add shortcut for ${a.label}`} title="Add shortcut" onclick={() => startRecording(a.id)}><Plus size={14} /></button>
                {/if}
                <button class="icon-btn sm" class:invisible={isDefault(a.id)} aria-label={`Reset ${a.label}`} title="Reset to default" onclick={() => resetBindings(a.id)}><RotateCcw size={14} /></button>
              </span>
            </div>
            {#if recording === a.id && recordError}<div class="record-error">{recordError}</div>{/if}
          {/each}
        {/each}
        <p class="hint">In the message field, ↑ also edits your last message and Esc cancels a reply or edit. {mod}+A selects the text when the field has some.</p>
      {:else if ui.settingsSection === 'accounts' && account}
        <h2 class="with-back">
          <button class="icon-btn sm" aria-label="Back to accounts" onclick={() => (ui.settingsAccount = '')}><ChevronLeft size={18} /></button>
          {store.accountName(account.accountId)}
        </h2>
        <div class="card">
          <ProtocolBadge protocol={account.protocol} color={store.accountColor(account.accountId)} size={32} />
          <div class="row-text">
            <b>{protocolMeta(account.protocol).name} · {account.displayName || account.selfId}</b>
            <span>{account.accountId} · {stateLabel(account.state)}</span>
          </div>
        </div>
        {#if account.state === 'error' && account.error}
          <div class="banner danger">{account.error}</div>
        {/if}
        <div class="field">
          <label for="acct-name">Display name</label>
          <input id="acct-name" class="input" placeholder={protocolMeta(account.protocol).name} value={settings.accounts[account.accountId]?.name ?? ''}
            oninput={(e) => setAccount('name', (e.target as HTMLInputElement).value)} />
        </div>
        <div class="field">
          <span class="label">Account color</span>
          <div class="colors" role="radiogroup" aria-label="Account color">
            {#each ACCOUNT_COLORS as c}
              <button role="radio" aria-checked={store.accountColor(account.accountId) === `var(--acct-${c})`} aria-label={c}
                style:background="var(--acct-{c})" onclick={() => setAccount('color', c)}></button>
            {/each}
          </div>
          <span class="hint">Shown on avatars, in the sidebar and in the chat header.</span>
        </div>
        <div class="row">
          <div class="row-text"><b>Notifications for this account</b></div>
          <button class="toggle" role="switch" aria-label="Notifications for this account" aria-checked={settings.accounts[account.accountId]?.notify !== false}
            onclick={() => setAccount('notify', settings.accounts[account.accountId]?.notify === false)}></button>
        </div>
        {#if account.protocol === 'Telegram'}
          <div class="row">
            <div class="row-text"><b>“Joined Telegram” messages</b><span>Off: a contact joining Telegram adds no message and no new chat</span></div>
            <button class="toggle" role="switch" aria-label="Joined Telegram messages" aria-checked={!!account.options?.showJoinedMessages}
              onclick={() => setShowJoined(!account.options?.showJoinedMessages)}></button>
          </div>
        {/if}
        <div class="buttons">
          <button class="btn danger" onclick={removeAccount} disabled={removing}><Trash2 size={16} />Remove account…</button>
        </div>
      {:else if ui.settingsSection === 'accounts'}
        <h2>Accounts</h2>
        {#each store.accountList as a (a.accountId)}
          <button class="acct-row" onclick={() => (ui.settingsAccount = a.accountId)}>
            <ProtocolBadge protocol={a.protocol} color={store.accountColor(a.accountId)} size={24} />
            <span class="row-text"><b>{store.accountName(a.accountId)}</b><span>{protocolMeta(a.protocol).name} · {stateLabel(a.state)}</span></span>
            <Pencil size={16} />
          </button>
        {:else}
          <p class="hint">No accounts yet.</p>
        {/each}
        <div class="buttons">
          <button class="btn" onclick={() => { close(); store.wizardSetupId = 'new'; }}><Plus size={16} />Add account</button>
        </div>
      {:else if ui.settingsSection === 'about'}
        <div class="about">
          <Logo size={56} />
          <h2>braids</h2>
          <span class="hint">{#if appVersion}{`Version ${appVersion} · `}{/if}Core {store.daemonVersion || '–'} · API {store.apiVersion || '–'} · built on nchat</span>
          {#if api.isTauri}
            <div class="update">
              {#if store.update}
                <span>braids {store.update.version} is available.</span>
                <button class="btn primary sm" onclick={() => api.openUrl(store.update!.url).catch((e) => store.toastError(e))}>Get it</button>
              {:else}
                <span>{updateStatus}</span>
                <button class="btn sm" onclick={checkNow} disabled={updateStatus === 'Checking…'}>Check for updates</button>
              {/if}
            </div>
            <label class="check"><input type="checkbox" checked={settings.checkUpdates} onchange={(e) => set('checkUpdates', e.currentTarget.checked)} />Check for updates automatically</label>
          {/if}
          <div class="feedback">
            <b>Feedback</b>
            <span>Found a bug or have an idea? Tell us on GitHub (needs a GitHub account).</span>
            <div class="buttons center">
              <button class="btn" onclick={() => reportBug().catch((e) => store.toastError(e))}><Bug size={16} />Report a bug</button>
              <button class="btn" onclick={() => api.openUrl(`${ISSUES}/new?template=feature_request.md`).catch((e) => store.toastError(e))}><Lightbulb size={16} />Suggest a feature</button>
              <button class="btn" onclick={() => api.openUrl(ISSUES).catch((e) => store.toastError(e))}>Known issues</button>
            </div>
          </div>
          <p>Free software under the GNU Affero General Public License v3.</p>
          <div class="buttons center">
            <button class="btn" onclick={() => api.openUrl('https://github.com/pebll/braids').catch((e) => store.toastError(e))}>Source code</button>
            <button class="btn" onclick={() => api.openLicenses().catch((e) => store.toastError(e))}>Third-party licenses</button>
          </div>
        </div>
      {/if}
    </div>
  </div>
</div>

<style>
  .update { display: flex; align-items: center; gap: 10px; margin-top: 6px; font-size: 13px; color: var(--text-2); }
  .check { display: flex; align-items: center; gap: 6px; font-size: 13px; color: var(--text-muted); }
  .settings { width: 760px; height: min(560px, calc(100vh - 32px)); flex-direction: row; }
  nav { width: 180px; flex-shrink: 0; background: var(--surface-2); border-right: 1px solid var(--border); padding: 14px 8px; display: flex; flex-direction: column; gap: 2px; }
  .nav-title { font-size: 12px; color: var(--text-muted); padding: 4px 10px 8px; font-weight: 600; }
  nav button { display: flex; align-items: center; gap: 10px; height: 32px; padding: 0 10px; border: 0; border-radius: var(--radius-md); background: none; text-align: left; font-size: 14px; color: var(--text); }
  nav button:hover { background: var(--hover); }
  nav button.active { background: var(--selected); font-weight: 600; }
  .nav-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--accent); margin-left: auto; }
  .pane { flex: 1; padding: 20px 24px; position: relative; display: flex; flex-direction: column; gap: 4px; }
  .close { position: absolute; right: 12px; top: 12px; }
  h2 { margin: 0 0 10px; font-size: 20px; font-weight: 650; }
  .with-back { display: flex; align-items: center; gap: 6px; margin-left: -6px; }
  .row { display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 12px 0; border-bottom: 1px solid var(--border); }
  .row.compact { padding: 8px 0; font-size: 14px; }
  .row-text { display: flex; flex-direction: column; min-width: 0; font-size: 14px; }
  .row-text b { font-weight: 500; }
  .row-text span { font-size: 12px; color: var(--text-muted); line-height: 16px; overflow-wrap: anywhere; }
  .text-size-control { display: flex; align-items: center; gap: 6px; flex-shrink: 0; }
  .text-size-control select, .custom-size input {
    height: 30px; border: 1px solid var(--border-strong); border-radius: var(--radius-md);
    background: var(--surface-2); padding: 0 8px; color: var(--text); font-size: 13px;
  }
  .custom-size { display: flex; align-items: center; gap: 4px; color: var(--text-2); }
  .custom-size input { width: 64px; }
  .section { font-size: 11px; font-weight: 600; letter-spacing: 0.06em; text-transform: uppercase; color: var(--text-muted); margin-top: 16px; }
  .keys { display: flex; gap: 6px; align-items: center; flex-wrap: wrap; justify-content: flex-end; }
  .shortcuts-head { display: flex; align-items: center; justify-content: space-between; }
  .shortcuts-head .btn { height: 26px; padding: 0 8px; gap: 4px; font-size: 12px; text-transform: none; letter-spacing: 0; }
  .shortcut { gap: 12px; }
  .group-label { font-size: 13px; font-weight: 600; color: var(--text-2); margin-top: 12px; }
  .combo { display: inline-flex; align-items: center; gap: 3px; padding: 2px 2px 2px 4px; border-radius: 6px; background: var(--surface-2); }
  .combo .remove { border: 0; background: none; padding: 2px; display: flex; color: var(--text-muted); border-radius: 4px; }
  .combo .remove:hover { color: var(--danger); background: var(--hover); }
  .recording { font-size: 12px; color: var(--accent); font-weight: 600; }
  .record-error { font-size: 12px; color: var(--danger); text-align: right; margin-top: -4px; }
  .invisible { visibility: hidden; }
  kbd { font-family: var(--mono); font-size: 11px; border: 1px solid var(--border-strong); border-bottom-width: 2px; border-radius: 4px; padding: 0 5px; min-width: 20px; text-align: center; }
  .card { display: flex; align-items: center; gap: 12px; padding: 12px; border-radius: var(--radius-lg); background: var(--surface-2); border: 1px solid var(--border); margin-bottom: 12px; }
  .field { margin: 8px 0; }
  .label { font-size: 13px; font-weight: 600; }
  .colors { display: flex; gap: 8px; }
  .colors button { width: 26px; height: 26px; border-radius: 50%; border: 0; padding: 0; }
  .colors button[aria-checked='true'] { box-shadow: 0 0 0 2px var(--surface), 0 0 0 4px var(--text); }
  .hint { font-size: 12px; color: var(--text-muted); }
  .buttons { display: flex; gap: 8px; margin-top: 16px; }
  .buttons.center { justify-content: center; }
  .acct-row { display: flex; align-items: center; gap: 12px; padding: 10px 4px; border: 0; border-bottom: 1px solid var(--border); background: none; text-align: left; color: var(--text-2); }
  .acct-row .row-text { flex: 1; }
  .acct-row:hover { background: var(--hover); }
  .about { display: flex; flex-direction: column; align-items: center; text-align: center; gap: 4px; padding-top: 24px; }
  .about h2 { margin: 8px 0 0; }
  .about p { color: var(--text-2); max-width: 340px; }
  .feedback { display: flex; flex-direction: column; align-items: center; gap: 4px; margin: 16px 0 4px; padding: 14px 16px; border: 1px solid var(--border); border-radius: var(--radius-lg); max-width: 460px; }
  .feedback b { font-weight: 600; }
  .feedback > span { font-size: 13px; color: var(--text-2); }
  .feedback .btn { display: inline-flex; align-items: center; gap: 6px; }
  @media (max-width: 700px) {
    .settings { flex-direction: column; height: calc(100vh - 32px); }
    nav { width: auto; flex-direction: row; overflow-x: auto; border-right: 0; border-bottom: 1px solid var(--border); }
    .nav-title { display: none; }
  }
</style>
