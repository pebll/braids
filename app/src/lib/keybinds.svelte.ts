// keybinds.svelte.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// Global keyboard shortcuts: actions with default key combos, user changes
// kept in settings.keybinds (only the actions that differ from the default).
//
// A combo is "Mod+Alt+Shift+Key": Mod is Ctrl (Cmd on macOS), Key a letter or
// digit of the system keyboard layout, or a key name such as ArrowDown,
// Space, ",".

import { settings } from './settings.svelte';

export type Action =
  | 'goAll'
  | 'goFavorites'
  | 'goMatrix'
  | 'goSignal'
  | 'goTelegram'
  | 'goWhatsApp'
  | 'nextChat'
  | 'prevChat'
  | 'nextUnread'
  | 'searchChats'
  | 'searchInChat'
  | 'editLast'
  | 'toggleInfo'
  | 'settings';

export type ActionGroup = 'goto' | 'action';

export const GROUPS: { id: ActionGroup; label: string }[] = [
  { id: 'goto', label: 'Go to' },
  { id: 'action', label: 'Actions' },
];

export const ACTIONS: { id: Action; group: ActionGroup; label: string; defaults: string[] }[] = [
  { id: 'goAll', group: 'goto', label: 'All chats', defaults: ['Mod+A'] },
  { id: 'goFavorites', group: 'goto', label: 'Favorites', defaults: ['Mod+B'] },
  { id: 'goMatrix', group: 'goto', label: 'Matrix account', defaults: ['Mod+M'] },
  { id: 'goSignal', group: 'goto', label: 'Signal account', defaults: ['Mod+S'] },
  { id: 'goTelegram', group: 'goto', label: 'Telegram account', defaults: ['Mod+T'] },
  { id: 'goWhatsApp', group: 'goto', label: 'WhatsApp account', defaults: ['Mod+W'] },
  { id: 'nextChat', group: 'goto', label: 'Next chat', defaults: ['Alt+ArrowDown', 'Alt+J'] },
  { id: 'prevChat', group: 'goto', label: 'Previous chat', defaults: ['Alt+ArrowUp', 'Alt+K'] },
  { id: 'nextUnread', group: 'goto', label: 'Next unread chat', defaults: ['Mod+N'] },
  { id: 'searchChats', group: 'action', label: 'Search chats', defaults: ['Mod+Space'] },
  { id: 'searchInChat', group: 'action', label: 'Search in chat', defaults: ['Mod+F'] },
  { id: 'editLast', group: 'action', label: 'Edit last own message', defaults: ['Mod+E'] },
  { id: 'toggleInfo', group: 'action', label: 'Toggle info panel', defaults: ['Mod+I'] },
  { id: 'settings', group: 'action', label: 'Settings', defaults: ['Mod+,'] },
];

// protocol of the account shortcuts (several accounts of one protocol: cycled)
export const ACCOUNT_ACTIONS: Partial<Record<Action, string>> = {
  goMatrix: 'Matrix',
  goSignal: 'Signal',
  goTelegram: 'Telegram',
  goWhatsApp: 'WhatsAppMd',
};

// text editing keys keep their usual meaning in a text field with text
// (Ctrl+A selects all there, in an empty field it is the shortcut)
const TEXT_EDITING = new Set(['Mod+A', 'Mod+C', 'Mod+V', 'Mod+X', 'Mod+Z', 'Mod+Shift+Z', 'Mod+Y']);

export const isMac = typeof navigator !== 'undefined' && navigator.platform.toLowerCase().includes('mac');

const MODIFIER_KEYS = new Set(['Control', 'Meta', 'Alt', 'Shift', 'AltGraph', 'CapsLock', 'Fn', 'OS']);

// The combo of a key event, '' for a lone modifier.
export function comboOf(e: KeyboardEvent): string {
  if (MODIFIER_KEYS.has(e.key)) return '';
  // the key as the system layout names it (Ctrl+A is the key labelled A on
  // AZERTY too); the physical key only when the layout gives no plain letter
  // or digit for a letter / digit key: non-latin layouts, Option+letter
  // symbols on macOS, the AZERTY digit row
  let key: string;
  const physical = /^Key([A-Z])$/.exec(e.code) ?? /^Digit([0-9])$/.exec(e.code);
  if (e.key === ' ' || e.code === 'Space') key = 'Space';
  else if (/^[a-z0-9]$/i.test(e.key)) key = e.key.toUpperCase();
  else if (physical) key = physical[1];
  else if (e.key.length === 1) key = e.key.toUpperCase();
  else key = e.key;
  const parts: string[] = [];
  if (e.ctrlKey || e.metaKey) parts.push('Mod');
  if (e.altKey) parts.push('Alt');
  // shift is part of the key for symbols ("?"), a modifier otherwise
  if (e.shiftKey && (key.length > 1 || /[A-Z0-9]/.test(key))) parts.push('Shift');
  parts.push(key);
  return parts.join('+');
}

const KEY_LABELS: Record<string, string> = { ArrowDown: '↓', ArrowUp: '↑', ArrowLeft: '←', ArrowRight: '→', Escape: 'Esc' };

// Keys of a combo for display, e.g. ["Ctrl", "Space"].
export function comboKeys(combo: string): string[] {
  return combo.split(/\+(?!$)/).map((k) => {
    if (k === 'Mod') return isMac ? '⌘' : 'Ctrl';
    if (k === 'Alt') return isMac ? '⌥' : 'Alt';
    if (k === 'Shift') return isMac ? '⇧' : 'Shift';
    return KEY_LABELS[k] ?? k;
  });
}

export function comboLabel(combo: string): string {
  return comboKeys(combo).join('+');
}

// A combo is usable as a global shortcut when it does not type text.
export function isUsableCombo(combo: string): boolean {
  if (!combo) return false;
  if (combo.startsWith('Mod+') || combo.startsWith('Alt+')) return true;
  return /^(Shift\+)?F\d{1,2}$/.test(combo);
}

export function bindings(action: Action): string[] {
  return settings.keybinds[action] ?? ACTIONS.find((a) => a.id === action)!.defaults;
}

export function isDefault(action: Action): boolean {
  return settings.keybinds[action] === undefined;
}

// First shortcut of an action for tooltips, e.g. " (Ctrl+F)".
export function hint(action: Action): string {
  const combo = bindings(action)[0];
  return combo ? ` (${comboLabel(combo)})` : '';
}

export function actionFor(e: KeyboardEvent): Action | undefined {
  const combo = comboOf(e);
  if (!combo) return undefined;
  const target = e.target as HTMLInputElement | null;
  const hasText = !!target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA') && target.value !== '';
  if (hasText && TEXT_EDITING.has(combo)) return undefined;
  return ACTIONS.find((a) => bindings(a.id).includes(combo))?.id;
}

function setBindings(action: Action, combos: string[]) {
  const defaults = ACTIONS.find((a) => a.id === action)!.defaults;
  const same = combos.length === defaults.length && combos.every((c, i) => c === defaults[i]);
  const next = { ...settings.keybinds };
  if (same) delete next[action];
  else next[action] = combos;
  settings.keybinds = next;
}

// Adds a combo to an action; returns the action it was taken from, if any.
export function addBinding(action: Action, combo: string): Action | undefined {
  let takenFrom: Action | undefined;
  for (const a of ACTIONS) {
    if (a.id !== action && bindings(a.id).includes(combo)) {
      takenFrom = a.id;
      setBindings(a.id, bindings(a.id).filter((c) => c !== combo));
    }
  }
  if (!bindings(action).includes(combo)) setBindings(action, [...bindings(action), combo]);
  settings.save();
  return takenFrom;
}

export function removeBinding(action: Action, combo: string) {
  setBindings(action, bindings(action).filter((c) => c !== combo));
  settings.save();
}

// Back to the default combos (of one action or all); another action's
// custom combo that is a default here is taken from it.
export function resetBindings(action?: Action) {
  if (!action) {
    settings.keybinds = {};
    settings.save();
    return;
  }
  const next = { ...settings.keybinds };
  delete next[action];
  settings.keybinds = next;
  for (const combo of bindings(action)) {
    for (const a of ACTIONS) {
      if (a.id !== action && bindings(a.id).includes(combo)) setBindings(a.id, bindings(a.id).filter((c) => c !== combo));
    }
  }
  settings.save();
}
