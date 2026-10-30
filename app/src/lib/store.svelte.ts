// store.svelte.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// Client state: a mirror of the braidsd model (accounts, chats, loaded
// messages) kept current by API events, plus UI state. Svelte 5 runes.

import * as api from './api';
import { plainText } from './format';
import { ACCOUNT_COLORS, protocolMeta } from './protocols';
import { settings } from './settings.svelte';
import type { Account, ApiEvent, AuthPrompt, Chat, Contact, Message } from './types';
import { ApiError } from './types';

export const API_MAJOR = '0';
const PAGE = 40;

export function chatKey(accountId: string, chatId: string): string {
  return `${accountId}\u0001${chatId}`;
}

export function splitKey(key: string): [string, string] {
  const i = key.indexOf('\u0001');
  return [key.slice(0, i), key.slice(i + 1)];
}

export interface MessageList {
  items: Message[];
  loading: boolean;
  atStart: boolean;
  loaded: boolean;
  firstUnreadId: string; // "new messages" separator, set when the chat is opened
}

export interface Setup {
  setupId: string;
  protocol: string;
  prompt: AuthPrompt | null; // current input / qr / info step
  error: string; // last error step
  working: boolean; // input sent, waiting for the next step
  done: 'no' | 'added' | 'failed';
  accountId: string;
  failMessage: string;
}

export interface Toast {
  id: number;
  text: string;
  kind: 'info' | 'error' | 'success';
  action?: { label: string; run: () => void };
}

export type Filter = { kind: 'all' } | { kind: 'unread' } | { kind: 'archived' } | { kind: 'account'; accountId: string };

class Store {
  // daemon
  connected = $state(false);
  synced = $state(false); // initial state loaded after connect
  daemonVersion = $state('');
  apiVersion = $state('');
  protocols = $state<string[]>([]);
  fatal = $state(''); // e.g. api version mismatch

  // model
  accounts = $state<Record<string, Account>>({});
  connectedAccounts = $state<Record<string, boolean>>({});
  chats = $state<Record<string, Chat>>({});
  messages = $state<Record<string, MessageList>>({});
  contacts = $state<Record<string, Record<string, Contact>>>({});
  members = $state<Record<string, Contact[]>>({});
  typing = $state<Record<string, Record<string, string>>>({}); // key -> userId -> name
  presence = $state<Record<string, { isOnline: boolean; timeSeen: number }>>({}); // accountId\u0001userId
  setups = $state<Record<string, Setup>>({});
  accountPrompts = $state<Record<string, AuthPrompt>>({}); // runtime prompts per account

  // ui
  current = $state(''); // chat key
  filter = $state<Filter>({ kind: 'all' });
  search = $state('');
  drafts = $state<Record<string, string>>({});
  toasts = $state<Toast[]>([]);
  wizardSetupId = $state(''); // open add-account wizard ('' closed, 'new' choosing protocol)
  focused = $state(true);

  private typingTimers = new Map<string, ReturnType<typeof setTimeout>>();
  private toastId = 1;

  // ---- derived ----

  get accountList(): Account[] {
    return Object.values(this.accounts).sort((a, b) => a.accountId.localeCompare(b.accountId));
  }

  get chatList(): Chat[] {
    const f = this.filter;
    const q = this.search.trim().toLowerCase();
    return Object.values(this.chats)
      .filter((c) => this.accounts[c.accountId])
      .filter((c) => (f.kind === 'archived' ? c.isArchived : !c.isArchived))
      .filter((c) => f.kind !== 'unread' || c.isUnread || chatKey(c.accountId, c.id) === this.current)
      .filter((c) => f.kind !== 'account' || c.accountId === f.accountId)
      .filter((c) => !q || this.chatName(c).toLowerCase().includes(q))
      .sort((a, b) => Number(b.isPinned) - Number(a.isPinned) || b.lastActivity - a.lastActivity);
  }

  unreadCount(filter: Filter): number {
    return Object.values(this.chats).filter((c) => {
      if (!c.isUnread || !this.accounts[c.accountId]) return false;
      if (filter.kind === 'archived') return c.isArchived;
      if (c.isArchived) return false;
      if (filter.kind === 'account') return c.accountId === filter.accountId;
      return true;
    }).length;
  }

  get badgeCount(): number {
    return Object.values(this.chats).filter((c) => c.isUnread && !c.isArchived && (!c.isMuted || c.isUnreadMention) && this.accounts[c.accountId]).length;
  }

  get currentChat(): Chat | null {
    return this.current ? (this.chats[this.current] ?? null) : null;
  }

  chatName(c: Chat): string {
    return c.name || this.contacts[c.accountId]?.[c.id]?.name || c.id;
  }

  accountName(accountId: string): string {
    const meta = settings.accounts[accountId];
    if (meta?.name) return meta.name;
    const a = this.accounts[accountId];
    if (!a) return accountId;
    const proto = protocolMeta(a.protocol).name;
    // one account per protocol: the protocol name; several: also the user
    const sameProto = Object.values(this.accounts).filter((x) => x.protocol === a.protocol).length;
    if (sameProto > 1) return `${proto} ${a.selfId || a.displayName}`;
    return proto;
  }

  accountColor(accountId: string): string {
    const color = settings.accounts[accountId]?.color ?? this.defaultColor(accountId);
    return `var(--acct-${color})`;
  }

  private defaultColor(accountId: string): string {
    const a = this.accounts[accountId];
    const base = protocolMeta(a?.protocol ?? '').color;
    // later accounts of the same protocol get the next free color
    const others = Object.values(this.accounts)
      .filter((x) => x.protocol === a?.protocol)
      .map((x) => x.accountId)
      .sort();
    const idx = others.indexOf(accountId);
    if (idx <= 0) return base;
    const used = new Set(Object.keys(this.accounts).filter((id) => id !== accountId).map((id) => settings.accounts[id]?.color ?? ''));
    const free = ACCOUNT_COLORS.filter((c) => c !== base && !used.has(c));
    return free[(idx - 1) % Math.max(1, free.length)] ?? base;
  }

  senderName(m: Message): string {
    if (m.isOutgoing) return 'You';
    return m.senderName || this.contacts[m.accountId]?.[m.senderId]?.name || m.senderId;
  }

  previewText(m: Message | null): string {
    if (!m) return '';
    if (m.isDeleted) return 'Message deleted';
    const text = plainText(m.text);
    if (m.file) return text ? `${fileLabel(m)}: ${text}` : `${fileLabel(m)}: ${m.file.name}`;
    return text;
  }

  typingNames(key: string): string[] {
    return Object.values(this.typing[key] ?? {});
  }

  isOnline(accountId: string, userId: string): boolean {
    return this.presence[`${accountId}\u0001${userId}`]?.isOnline ?? false;
  }

  // ---- connection and sync ----

  async start() {
    window.addEventListener('focus', () => {
      this.focused = true;
      this.markCurrentRead();
    });
    window.addEventListener('blur', () => (this.focused = false));
    this.focused = document.hasFocus();
    await api.subscribe(
      (e) => this.onEvent(e),
      (connected) => {
        this.connected = connected;
        if (connected) this.resync();
      },
    );
  }

  private async resync() {
    try {
      const hello = await api.call('hello');
      this.apiVersion = hello.apiVersion;
      this.daemonVersion = hello.version;
      this.protocols = hello.protocols;
      if (!String(hello.apiVersion).startsWith(`${API_MAJOR}.`)) {
        this.fatal = `braids core API ${hello.apiVersion} is not compatible with this app (needs ${API_MAJOR}.x).`;
        return;
      }

      const accounts: Account[] = await api.call('accounts.list');
      const next: Record<string, Account> = {};
      for (const a of accounts) next[a.accountId] = a;
      this.accounts = next;

      const chats: Chat[] = await api.call('chats.list', { includeArchived: true });
      const chatMap: Record<string, Chat> = {};
      for (const c of chats) chatMap[chatKey(c.accountId, c.id)] = c;
      this.chats = chatMap;

      for (const a of accounts) this.loadContacts(a.accountId);

      // reload what is open, events were missed while disconnected
      for (const key of Object.keys(this.messages)) {
        if (key !== this.current) delete this.messages[key];
      }
      if (this.current && this.chats[this.current]) {
        this.messages[this.current] = emptyList();
        await this.loadOlder(this.current);
      }
      this.synced = true;
    } catch (e) {
      console.error('resync failed', e);
    }
  }

  private async loadContacts(accountId: string) {
    try {
      const contacts: Contact[] = await api.call('contacts.list', { accountId });
      const map: Record<string, Contact> = {};
      for (const c of contacts) map[c.id] = c;
      this.contacts[accountId] = map;
    } catch {
      // account may be gone
    }
  }

  // ---- events ----

  onEvent(e: ApiEvent) {
    const p = e.params;
    switch (e.method) {
      case 'account.auth':
        this.onAuth(p as AuthPrompt);
        break;
      case 'account.added': {
        const a: Account = p.account;
        this.accounts[a.accountId] = a;
        const setup = this.ensureSetup(p.setupId);
        setup.done = 'added';
        setup.accountId = a.accountId;
        setup.working = false;
        this.loadContacts(a.accountId);
        break;
      }
      case 'account.setupFailed': {
        const setup = this.ensureSetup(p.setupId);
        setup.done = 'failed';
        setup.working = false;
        setup.failMessage = p.message || setup.error || 'The account could not be added.';
        break;
      }
      case 'account.removed':
        this.forgetAccount(p.accountId);
        break;
      case 'account.state': {
        const a = this.accounts[p.accountId];
        if (a) {
          a.state = p.state;
          a.error = p.error ?? '';
        }
        break;
      }
      case 'account.connected':
        this.connectedAccounts[p.accountId] = p.success;
        break;
      case 'chat.updated': {
        const c: Chat = p;
        this.chats[chatKey(c.accountId, c.id)] = c;
        break;
      }
      case 'chat.removed': {
        const key = chatKey(p.accountId, p.chatId);
        delete this.chats[key];
        delete this.messages[key];
        if (this.current === key) this.current = '';
        break;
      }
      case 'chat.moved': {
        const oldKey = chatKey(p.accountId, p.chatId);
        const newKey = chatKey(p.accountId, p.newChatId);
        delete this.messages[oldKey];
        if (this.current === oldKey) this.openChat(newKey);
        break;
      }
      case 'chat.created':
        this.pendingCreated?.(chatKey(p.accountId, p.chatId));
        break;
      case 'contacts.updated': {
        const map = p.fullSync ? {} : { ...(this.contacts[p.accountId] ?? {}) };
        for (const c of p.contacts as Contact[]) map[c.id] = c;
        this.contacts[p.accountId] = map;
        break;
      }
      case 'members.updated':
        this.members[chatKey(p.accountId, p.chatId)] = p.members;
        break;
      case 'message.added':
      case 'message.updated':
        this.upsertMessage(p as Message);
        break;
      case 'message.removed': {
        const list = this.messages[chatKey(p.accountId, p.chatId)];
        if (list) list.items = list.items.filter((m) => m.id !== p.msgId);
        break;
      }
      case 'message.sendFailed':
        this.toast('Message could not be sent.', 'error');
        break;
      case 'typing':
        this.onTyping(p);
        break;
      case 'presence':
        this.presence[`${p.accountId}\u0001${p.userId}`] = { isOnline: p.isOnline, timeSeen: p.timeSeen };
        break;
      case 'notification':
        this.onNotification(p);
        break;
    }
  }

  private pendingCreated: ((key: string) => void) | null = null;

  private onAuth(p: AuthPrompt) {
    if (p.setupId) {
      const setup = this.ensureSetup(p.setupId);
      if (p.step === 'error') {
        setup.error = p.message;
        setup.working = false;
      } else {
        if (p.step === 'input' && setup.prompt?.field !== p.field) setup.error = '';
        setup.prompt = p;
        setup.working = false;
      }
    } else if (p.accountId) {
      if (p.step === 'input' || p.step === 'qr') {
        this.accountPrompts[p.accountId] = p;
      } else if (p.step === 'error') {
        this.toast(`${this.accountName(p.accountId)}: ${p.message}`, 'error');
      } else {
        this.toast(`${this.accountName(p.accountId)}: ${p.message}`, 'info');
        delete this.accountPrompts[p.accountId];
      }
    }
  }

  private onTyping(p: { accountId: string; chatId: string; userId: string; userName: string; isTyping: boolean }) {
    const key = chatKey(p.accountId, p.chatId);
    const timerKey = `${key}\u0001${p.userId}`;
    clearTimeout(this.typingTimers.get(timerKey));
    if (p.isTyping) {
      if (!this.typing[key]) this.typing[key] = {};
      this.typing[key][p.userId] = p.userName || this.contacts[p.accountId]?.[p.userId]?.name || p.userId;
      // protocols do not always send the stop
      this.typingTimers.set(timerKey, setTimeout(() => this.clearTyping(key, p.userId), 7000));
    } else {
      this.clearTyping(key, p.userId);
    }
  }

  private clearTyping(key: string, userId: string) {
    if (this.typing[key]) delete this.typing[key][userId];
  }

  private onNotification(p: any) {
    const key = chatKey(p.accountId, p.chatId);
    if (!settings.notifications || settings.accounts[p.accountId]?.notify === false) return;
    if (this.focused && this.current === key) return;
    const title = p.isGroup ? `${p.chatName}` : p.senderName || p.chatName;
    let body = settings.notificationPreview ? plainText(p.text) || (p.hasFile ? 'Attachment' : '') : 'New message';
    if (settings.notificationPreview && p.isGroup && p.senderName) body = `${p.senderName}: ${body}`;
    api.notify(title, body, () => this.openChat(key));
  }

  private upsertMessage(m: Message) {
    const key = chatKey(m.accountId, m.chatId);
    const list = this.messages[key];
    if (!list) return;
    const items = list.items;
    const idx = items.findIndex((x) => x.id === m.id);
    if (idx >= 0) {
      items[idx] = m;
      return;
    }
    // usually newest: append; else insert in time order
    let pos = items.length;
    while (pos > 0 && items[pos - 1].time > m.time) pos--;
    items.splice(pos, 0, m);
    if (m.senderId && this.typing[key]?.[m.senderId]) this.clearTyping(key, m.senderId);
    if (key === this.current) this.markCurrentRead();
  }

  private forgetAccount(accountId: string) {
    delete this.accounts[accountId];
    delete this.contacts[accountId];
    delete this.accountPrompts[accountId];
    for (const key of Object.keys(this.chats)) {
      if (splitKey(key)[0] === accountId) {
        delete this.chats[key];
        delete this.messages[key];
      }
    }
    if (this.current && splitKey(this.current)[0] === accountId) this.current = '';
    if (this.filter.kind === 'account' && this.filter.accountId === accountId) this.filter = { kind: 'all' };
  }

  // ---- chats and messages ----

  async openChat(key: string) {
    if (!this.chats[key]) return;
    this.current = key;
    const [accountId, chatId] = splitKey(key);
    api.call('chats.setCurrent', { accountId, chatId }).catch(() => {});
    if (!this.messages[key]?.loaded) {
      this.messages[key] = emptyList();
      await this.loadOlder(key);
      const list = this.messages[key];
      if (list) {
        const firstUnread = list.items.find((m) => !m.isOutgoing && !m.isRead);
        list.firstUnreadId = this.chats[key]?.isUnread && firstUnread ? firstUnread.id : '';
      }
    }
    this.markCurrentRead();
  }

  async loadOlder(key: string): Promise<number> {
    const list = this.messages[key];
    if (!list || list.loading || list.atStart) return 0;
    const [accountId, chatId] = splitKey(key);
    list.loading = true;
    try {
      const fromMsgId = list.items[0]?.id ?? '';
      const page = await api.call('messages.get', { accountId, chatId, fromMsgId, limit: PAGE });
      const current = this.messages[key];
      if (!current) return 0;
      const have = new Set(current.items.map((m) => m.id));
      const older = (page.messages as Message[]).filter((m) => !have.has(m.id));
      current.items = [...older, ...current.items].sort((a, b) => a.time - b.time);
      current.loaded = true;
      // no older messages (protocols may repeat the last page) means the start
      if (older.length === 0 && !page.timedOut) current.atStart = true;
      return older.length;
    } catch (e) {
      this.toastError(e);
      return 0;
    } finally {
      const current = this.messages[key];
      if (current) current.loading = false;
    }
  }

  markCurrentRead() {
    const key = this.current;
    const chat = this.chats[key];
    const list = this.messages[key];
    if (!chat || !list?.loaded || !this.focused) return;
    const last = [...list.items].reverse().find((m) => !m.isOutgoing);
    if (!last || (last.isRead && !chat.isUnread)) return;
    const [accountId, chatId] = splitKey(key);
    api.call('messages.markRead', { accountId, chatId, msgId: last.id }).catch(() => {});
    last.isRead = true;
  }

  closeChat() {
    this.current = '';
  }

  async createChat(accountId: string, userId: string): Promise<void> {
    const created = new Promise<string>((resolve) => {
      this.pendingCreated = resolve;
      setTimeout(() => resolve(''), 15000);
    });
    await api.call('chats.create', { accountId, userId });
    const key = await created;
    this.pendingCreated = null;
    if (key) {
      // the chat.updated event may arrive just after chat.created
      for (let i = 0; i < 20 && !this.chats[key]; i++) await new Promise((r) => setTimeout(r, 100));
      await this.openChat(key);
    }
  }

  // ---- setups ----

  async addAccount(protocol: string): Promise<string> {
    const { setupId } = await api.call('accounts.add', { protocol });
    // the first steps may have arrived before this reply
    this.ensureSetup(setupId).protocol = protocol;
    return setupId;
  }

  private ensureSetup(setupId: string): Setup {
    if (!this.setups[setupId]) {
      this.setups[setupId] = {
        setupId, protocol: '', prompt: null, error: '', working: true, done: 'no', accountId: '', failMessage: '',
      };
    }
    return this.setups[setupId];
  }

  async authInput(scope: { setupId?: string; accountId?: string }, field: string, value: string) {
    if (scope.setupId) {
      const setup = this.setups[scope.setupId];
      if (setup) {
        setup.working = true;
        setup.error = '';
      }
    }
    try {
      await api.call('accounts.authInput', { ...scope, field, value });
      if (scope.accountId) delete this.accountPrompts[scope.accountId];
    } catch (e) {
      if (scope.setupId && this.setups[scope.setupId]) {
        this.setups[scope.setupId].working = false;
        this.setups[scope.setupId].error = (e as Error).message;
      } else {
        this.toastError(e);
      }
    }
  }

  async authCancel(scope: { setupId?: string; accountId?: string }) {
    await api.call('accounts.authCancel', scope).catch(() => {});
    if (scope.accountId) delete this.accountPrompts[scope.accountId];
  }

  // ---- toasts ----

  toast(text: string, kind: Toast['kind'] = 'info', action?: Toast['action']) {
    const id = this.toastId++;
    this.toasts.push({ id, text, kind, action });
    setTimeout(() => this.dismissToast(id), kind === 'error' ? 8000 : 4000);
  }

  toastError(e: unknown) {
    const msg = e instanceof ApiError ? e.message : String((e as Error)?.message ?? e);
    this.toast(msg.charAt(0).toUpperCase() + msg.slice(1), 'error');
  }

  dismissToast(id: number) {
    this.toasts = this.toasts.filter((t) => t.id !== id);
  }
}

function emptyList(): MessageList {
  return { items: [], loading: false, atStart: false, loaded: false, firstUnreadId: '' };
}

export function fileLabel(m: Message): string {
  const mime = m.file?.mimeType ?? '';
  if (mime.startsWith('image/')) return 'Photo';
  if (mime.startsWith('video/')) return 'Video';
  if (mime.startsWith('audio/')) return 'Audio';
  return 'File';
}

export const store = new Store();
