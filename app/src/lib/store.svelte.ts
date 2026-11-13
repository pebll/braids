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
import { settings, type ChatCategory } from './settings.svelte';
import type { LightboxItem } from './ui.svelte';
import type { Account, ApiEvent, AuthPrompt, Chat, Contact, FileRef, Message } from './types';
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
  stalled: boolean; // the protocol did not answer: load more only on request
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

export type Filter = { kind: 'all' } | { kind: 'unread' } | { kind: 'favorites' } | { kind: 'archived' } | { kind: 'account'; accountId: string };

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
  membersCanAdd = $state<Record<string, boolean>>({}); // chat key -> self may add members
  private pendingMemberEdits = new Map<string, (r: { success: boolean; error: string }) => void>();
  typing = $state<Record<string, Record<string, string>>>({}); // key -> userId -> name
  presence = $state<Record<string, { isOnline: boolean; timeSeen: number }>>({}); // accountId\u0001userId
  avatars = $state<Record<string, string>>({}); // accountId\u0001user or chat id -> image path ('' none)
  fullAvatars = $state<Record<string, string>>({}); // same keys: full-size pictures, fetched for the viewer
  descriptions = $state<Record<string, string>>({}); // accountId\u0001user or chat id -> group description / bio ('' none)
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
  networkOnline = $state(true);
  staleSince = $state(0); // after sleep or a network outage, until the accounts caught up (ms, 0: fresh)
  lastSynced = $state<Record<string, number>>({}); // accountId -> when it was last online and done fetching (ms)
  now = $state(Date.now()); // for "updated 3 min ago", ticks every 30 s

  private typingTimers = new Map<string, ReturnType<typeof setTimeout>>();
  private avatarAsked = new Set<string>();
  private descriptionAsked = new Set<string>();
  private avatarBatch = new Map<string, Set<string>>(); // accountId -> ids
  private avatarTimer: ReturnType<typeof setTimeout> | undefined;
  private toastId = 1;

  // ---- derived ----

  get accountList(): Account[] {
    return Object.values(this.accounts).sort((a, b) => a.accountId.localeCompare(b.accountId));
  }

  category(c: Chat): ChatCategory {
    return settings.categories[chatKey(c.accountId, c.id)] ?? (c.isArchived ? 'archived' : 'default');
  }

  // Newest activity first; favorites first too when the setting says so.
  get chatList(): Chat[] {
    const f = this.filter;
    const q = this.search.trim().toLowerCase();
    const favoritesFirst = settings.favorites !== 'folder' && f.kind !== 'favorites';
    const fav = (c: Chat) => (favoritesFirst && this.category(c) === 'favorite' ? 1 : 0);
    return Object.values(this.chats)
      .filter((c) => this.accounts[c.accountId])
      .filter((c) => (f.kind === 'archived' ? this.category(c) === 'archived' : this.category(c) !== 'archived'))
      .filter((c) => f.kind !== 'favorites' || this.category(c) === 'favorite')
      // the open chat stays while it is read
      .filter((c) => f.kind !== 'unread' || (c.isUnread && !this.eventsOnly(c)) || chatKey(c.accountId, c.id) === this.current)
      .filter((c) => f.kind !== 'account' || c.accountId === f.accountId)
      .filter((c) => !q || this.chatName(c).toLowerCase().includes(q))
      .sort((a, b) => fav(b) - fav(a) || b.lastActivity - a.lastActivity);
  }

  // a room the account is invited to and has not joined (Matrix): its only
  // message is the local invite note ("invite:<room id>")
  isInvite(c: Chat | undefined): boolean {
    return !!c?.lastMessage?.id.startsWith('invite:');
  }

  // only group events (joined, left, ...) are new: shown grey, not counted
  // in folders or the tray, like a muted chat
  // A contact that is a person who can be added to a group. Matrix lists its
  // rooms as contacts too (direct chats named after the person, groups):
  // only user ids (@user:server) are people there; elsewhere groups are
  // left out.
  isPerson(accountId: string, c: Contact): boolean {
    if (c.isSelf || !(c.name || c.id)) return false;
    if (this.accounts[accountId]?.protocol === 'Matrix') return c.id.startsWith('@');
    return !this.chats[chatKey(accountId, c.id)]?.isGroup;
  }

  eventsOnly(c: Chat): boolean {
    return c.isUnread && (c.unreadCount ?? 1) === 0 && (c.unreadEvents ?? 0) > 0;
  }

  // the number on a chat: its unread messages, or its unread events when
  // there are only those
  chatUnread(c: Chat): number {
    if (!c.isUnread) return 0;
    return this.eventsOnly(c) ? c.unreadEvents : Math.max(1, c.unreadCount ?? 1);
  }

  // unread chats per folder (each counts once, however many messages);
  // muted chats count only when mentioned; the archive counts its chats with
  // new messages (all quiet, like muted chats)
  unreadCount(filter: Filter): number {
    return this.unreadChats(filter).length;
  }

  // archived chats behave like muted ones: grey count, no notification
  isQuiet(c: Chat): boolean {
    return c.isMuted || this.category(c) === 'archived';
  }

  private unreadChats(filter: Filter): Chat[] {
    return Object.values(this.chats).filter((c) => {
      if (!c.isUnread || !this.accounts[c.accountId]) return false;
      if (this.eventsOnly(c)) return false;
      const category = this.category(c);
      if (filter.kind === 'archived') return category === 'archived';
      if (c.isMuted && !c.isUnreadMention) return false;
      if (category === 'archived') return false;
      if (filter.kind === 'favorites') return category === 'favorite';
      if (filter.kind === 'account') return c.accountId === filter.accountId;
      return true;
    });
  }

  // unread chats for tray and dock: not archived, not muted (unless mentioned)
  get badgeCount(): number {
    return this.unreadCount({ kind: 'all' });
  }

  // called with every new or changed message, also of chats not loaded
  // (e.g. downloads started from the shared media dialog)
  messageListeners = new Set<(m: Message) => void>();

  // Moves a chat to a category; archiving is mirrored to the messenger where it
  // supports archives, so other devices see it too.
  async setCategory(c: Chat, category: ChatCategory) {
    const key = chatKey(c.accountId, c.id);
    settings.categories[key] = category;
    settings.save();
    const archived = category === 'archived';
    if (this.accounts[c.accountId]?.capabilities.archiveChat && c.isArchived !== archived) {
      await api.call('chats.archive', { accountId: c.accountId, chatId: c.id, isArchived: archived }).catch((e) => this.toastError(e));
    }
  }

  // Mutes notifications (and unread counts) of a chat: braids' own setting,
  // kept by braidsd for every messenger.
  async setMuted(c: Chat, isMuted: boolean) {
    const key = chatKey(c.accountId, c.id);
    if (this.chats[key]) this.chats[key].isMuted = isMuted;
    await api.call('chats.mute', { accountId: c.accountId, chatId: c.id, isMuted }).catch((e) => {
      if (this.chats[key]) this.chats[key].isMuted = !isMuted;
      this.toastError(e);
    });
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

  // Text of the message a reply quotes: from the quote, else from the loaded history.
  quotedText(m: Message): string {
    const q = m.quoted;
    if (!q) return '';
    if (q.text) return plainText(q.text);
    const original = this.messages[chatKey(m.accountId, m.chatId)]?.items.find((x) => x.id === q.id);
    if (original) return this.previewText(original);
    return 'Original message';
  }

  // A group event as a line: "Pam joined", "You added Jim"; without a person
  // (e.g. sent by the group itself) just "Changed the group name to …".
  systemLine(m: Message): string {
    const chat = this.chats[chatKey(m.accountId, m.chatId)];
    const who = m.isOutgoing ? 'You' : m.senderId && m.senderId !== m.chatId ? this.senderName(m) : '';
    if (!who || (chat && who === this.chatName(chat))) return m.system.charAt(0).toUpperCase() + m.system.slice(1);
    return `${who} ${m.system}`;
  }

  previewText(m: Message | null): string {
    if (!m) return '';
    if (m.isDeleted) return 'Message deleted';
    if (m.system) return this.systemLine(m);
    if (m.poll) return `📊 ${m.poll.question}`;
    const text = plainText(m.text);
    if (m.file) {
      const label = fileLabel(m);
      if (text) return `${label}: ${text}`;
      // photos, videos and voice messages by kind, not by their (generated) file name
      return label === 'File' || label === 'Audio' ? `${label}: ${m.file.name}` : label;
    }
    return text;
  }

  typingNames(key: string): string[] {
    return Object.values(this.typing[key] ?? {});
  }

  // Avatar image of a user or chat; asks braidsd once (batched), the picture
  // arrives later as avatar.updated. '' while unknown or without a picture.
  // Fetches the full-size picture of a user / chat (avatar.full, or the reply).
  requestFullAvatar(accountId: string, id: string) {
    const key = chatKey(accountId, id);
    if (key in this.fullAvatars) return;
    api.call<{ path?: string }>('avatars.getFull', { accountId, id })
      .then((r) => { if (r.path !== undefined) this.fullAvatars[key] = r.path; })
      .catch(() => {}); // the small picture stays
  }

  avatar(accountId: string, id: string): string {
    const key = chatKey(accountId, id);
    const path = this.avatars[key];
    if (path === undefined && !this.avatarAsked.has(key) && this.accounts[accountId]?.capabilities.avatars) {
      this.avatarAsked.add(key);
      if (!this.avatarBatch.has(accountId)) this.avatarBatch.set(accountId, new Set());
      this.avatarBatch.get(accountId)!.add(id);
      clearTimeout(this.avatarTimer);
      this.avatarTimer = setTimeout(() => this.flushAvatars(), 50);
    }
    return path ?? '';
  }

  private flushAvatars() {
    for (const [accountId, ids] of this.avatarBatch) {
      api.call('avatars.get', { accountId, ids: [...ids] })
        .then((r) => {
          for (const [id, path] of Object.entries(r.avatars as Record<string, string>)) this.avatars[chatKey(accountId, id)] = path;
        })
        .catch(() => {});
    }
    this.avatarBatch.clear();
  }

  // Group description or user bio; asked for once, arrives later as
  // description.updated. '' while unknown or without one.
  description(accountId: string, id: string): string {
    const key = chatKey(accountId, id);
    const text = this.descriptions[key];
    if (text === undefined && !this.descriptionAsked.has(key) && this.accounts[accountId]?.capabilities.descriptions) {
      this.descriptionAsked.add(key);
      api.call('descriptions.get', { accountId, ids: [id] })
        .then((r) => {
          for (const [id, text] of Object.entries(r.descriptions as Record<string, string>)) this.descriptions[chatKey(accountId, id)] = text;
        })
        .catch(() => {});
    }
    return text ?? '';
  }

  isOnline(accountId: string, userId: string): boolean {
    return this.presence[`${accountId}\u0001${userId}`]?.isOnline ?? false;
  }

  // ---- connection and sync ----

  // What the shown state is up to, for the indicator over the chat list:
  // '' current, 'offline' no network, 'updating' connecting, syncing or
  // possibly outdated (after the computer slept or the network came back).
  // The account's state as shown: offline while the system has no network
  // (protocols notice that late), "fetching" while online and fetching.
  accountStatus(a: Account): 'online' | 'fetching' | 'connecting' | 'offline' | 'error' {
    if (a.state === 'error') return 'error';
    if (!this.networkOnline || a.state === 'offline') return 'offline';
    if (a.state === 'connecting') return 'connecting';
    return a.syncing ? 'fetching' : 'online';
  }

  accountStatusText(a: Account): string {
    switch (this.accountStatus(a)) {
      case 'error': return a.error || 'Error';
      case 'offline': return this.networkOnline ? 'Offline' : 'Offline (no network)';
      case 'connecting': return 'Connecting…';
      case 'fetching': return 'Fetching new messages…';
    }
    const t = this.lastSynced[a.accountId];
    if (!t) return 'Online';
    const min = Math.floor((this.now - t) / 60000);
    return `Online · up to date${min >= 1 ? ` (checked ${min < 60 ? `${min} min` : `${Math.floor(min / 60)} h`} ago)` : ''}`;
  }

  // What the indicator over the chat list says, naming the accounts it waits for.
  get syncDetail(): string {
    if (!this.networkOnline) return 'No network: chats may be outdated';
    const busy = Object.values(this.accounts).filter((a) => a.state === 'connecting' || a.syncing);
    const names = busy.map((a) => this.accountName(a.accountId));
    if (!busy.length) return this.staleSince ? 'Checking for new messages…' : '';
    const verb = busy.some((a) => a.state === 'connecting') ? 'Connecting' : 'Updating';
    return `${verb} ${names.length > 2 ? `${names.length} accounts` : names.join(' and ')}…`;
  }

  // Short form for the chat list header: the account when only one is busy.
  get syncLabel(): string {
    const busy = Object.values(this.accounts).filter((a) => a.state === 'connecting' || a.syncing);
    if (busy.length !== 1) return 'Updating…';
    return `${busy[0].state === 'connecting' ? 'Connecting' : 'Updating'} ${this.accountName(busy[0].accountId)}…`;
  }

  get syncStatus(): '' | 'offline' | 'updating' {
    if (!this.networkOnline) return 'offline';
    const accounts = Object.values(this.accounts);
    if (this.staleSince || accounts.some((a) => a.state === 'connecting' || a.syncing)) return 'updating';
    return '';
  }

  // Marks the state as possibly outdated; cleared once every account is
  // online and done syncing (at the earliest after a few seconds, so that a
  // protocol noticing a dead connection late is caught; at the latest after
  // a minute).
  markStale() {
    const since = Date.now();
    this.staleSince = since;
    // connections may have died meanwhile: reconnect now instead of waiting
    // for the protocols' keep-alive timeouts
    if (this.connected) api.call('accounts.reconnect').catch(() => {});
    const check = () => {
      if (this.staleSince !== since) return;
      const settled = Object.values(this.accounts).every((a) => a.state !== 'connecting' && !a.syncing);
      const age = Date.now() - since;
      if ((settled && this.networkOnline && age >= 4000) || age >= 60000) this.staleSince = 0;
      else setTimeout(check, 1000);
    };
    setTimeout(check, 1000);
  }

  private watchWake() {
    setInterval(() => (this.now = Date.now()), 30000);
    // timers stop while the computer sleeps: a long gap means it woke up
    let last = Date.now();
    setInterval(() => {
      const now = Date.now();
      if (now - last > 30000) this.markStale();
      last = now;
    }, 5000);
    this.networkOnline = navigator.onLine;
    window.addEventListener('offline', () => (this.networkOnline = false));
    window.addEventListener('online', () => {
      this.networkOnline = true;
      this.markStale();
    });
  }

  async start() {
    this.watchWake();
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
      for (const a of accounts) if (a.state === 'online' && !a.syncing) this.lastSynced[a.accountId] = Date.now();

      const chats: Chat[] = await api.call('chats.list', { includeArchived: true });
      const chatMap: Record<string, Chat> = {};
      for (const c of chats) chatMap[chatKey(c.accountId, c.id)] = c;
      this.chats = chatMap;

      for (const a of accounts) this.loadContacts(a.accountId);

      // reload what is open, events were missed while disconnected
      for (const key of Object.keys(this.messages)) {
        if (key !== this.current) delete this.messages[key];
      }
      // member lists (and who may add or remove members) too
      this.members = {};
      this.membersCanAdd = {};
      if (this.current && this.chats[this.current]) {
        const [accountId, chatId] = splitKey(this.current);
        if (this.chats[this.current].isGroup) {
          api.call('members.get', { accountId, chatId }).then((r) => this.setMembers(r)).catch(() => {});
        }
        this.messages[this.current] = emptyList();
        await this.loadOlder(this.current);
      }
      // avatars are asked for again after a reconnect
      this.avatarAsked.clear();
      this.descriptionAsked.clear();
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
          a.syncing = !!p.syncing;
          if (a.state === 'online' && !a.syncing) this.lastSynced[a.accountId] = Date.now();
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
        if (settings.categories[key]) {
          delete settings.categories[key];
          settings.save();
        }
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
      case 'chat.joined':
        this.pendingJoins.get(`${p.accountId}\u0001${p.link}`)?.(p);
        break;
      case 'chat.groupCreated':
        this.pendingJoins.get(`${p.accountId}\u0002${p.name}`)?.(p);
        break;
      case 'contacts.updated': {
        const map = p.fullSync ? {} : { ...(this.contacts[p.accountId] ?? {}) };
        for (const c of p.contacts as Contact[]) map[c.id] = c;
        this.contacts[p.accountId] = map;
        break;
      }
      case 'members.updated':
        this.setMembers(p);
        break;
      case 'members.edited':
        this.pendingMemberEdits.get(`${p.accountId}\u0002${p.chatId}\u0002${p.remove ? '-' : '+'}`)?.(p);
        break;
      case 'message.added':
      case 'message.updated':
        this.upsertMessage(p as Message);
        for (const listener of this.messageListeners) listener(p as Message);
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
      case 'avatar.updated':
        this.avatars[chatKey(p.accountId, p.id)] = p.path;
        delete this.fullAvatars[chatKey(p.accountId, p.id)];
        break;
      case 'avatar.full':
        this.fullAvatars[chatKey(p.accountId, p.id)] = p.path;
        break;
      case 'description.updated':
        this.descriptions[chatKey(p.accountId, p.id)] = p.description;
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
  private pendingJoins = new Map<string, (p: { success: boolean; chatId: string; error: string }) => void>();

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
    const chat = this.chats[key];
    if (chat && this.category(chat) === 'archived') return; // quiet, like muted (braidsd skips them too)
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
    // older than the loaded history (e.g. a quoted message fetched for its
    // text): it belongs to a page not loaded yet
    if (items.length && !list.atStart && m.time < items[0].time) return;
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
    for (const key of Object.keys(this.avatars)) {
      if (splitKey(key)[0] === accountId) delete this.avatars[key];
    }
    for (const key of Object.keys(this.descriptions)) {
      if (splitKey(key)[0] === accountId) delete this.descriptions[key];
    }
    for (const key of Object.keys(settings.categories)) {
      if (splitKey(key)[0] === accountId) delete settings.categories[key];
    }
    settings.save();
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
    }
    // the "new messages" separator marks what is unread now, not at an
    // earlier opening
    const list = this.messages[key];
    if (list) list.firstUnreadId = this.chats[key]?.isUnread ? firstUnreadId(list.items, this.chats[key].unreadCount) : '';
    this.markCurrentRead();
  }

  async loadOlder(key: string, force = false): Promise<number> {
    const list = this.messages[key];
    if (!list || list.loading || list.atStart || (list.stalled && !force)) return 0;
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
      current.stalled = older.length === 0 && !!page.timedOut;
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

  // Joins a group by invite link / address and opens it; throws with the
  // protocol's reason when that fails.
  async joinChat(accountId: string, link: string): Promise<void> {
    const id = `${accountId}\u0001${link}`;
    const result = new Promise<{ success: boolean; chatId: string; error: string }>((resolve) => {
      this.pendingJoins.set(id, resolve);
      setTimeout(() => resolve({ success: false, chatId: '', error: 'No answer from the messenger.' }), 60000);
    });
    try {
      await api.call('chats.join', { accountId, link });
      const r = await result;
      if (!r.success) throw new Error(r.error || 'Could not join.');
      const key = chatKey(accountId, r.chatId);
      for (let i = 0; i < 100 && !this.chats[key]; i++) await new Promise((res) => setTimeout(res, 100));
      if (this.chats[key]) await this.openChat(key);
    } finally {
      this.pendingJoins.delete(id);
    }
  }

  // Creates a group with the given members and opens it; throws with the
  // protocol's reason when that fails.
  async createGroup(accountId: string, name: string, userIds: string[]): Promise<void> {
    const id = `${accountId}\u0002${name.trim()}`;
    const result = new Promise<{ success: boolean; chatId: string; error: string }>((resolve) => {
      this.pendingJoins.set(id, resolve);
      setTimeout(() => resolve({ success: false, chatId: '', error: 'No answer from the messenger.' }), 60000);
    });
    try {
      await api.call('chats.createGroup', { accountId, name: name.trim(), userIds });
      const r = await result;
      if (!r.success) throw new Error(r.error || 'Could not create the group.');
      const key = chatKey(accountId, r.chatId);
      for (let i = 0; i < 100 && !this.chats[key]; i++) await new Promise((res) => setTimeout(res, 100));
      if (this.chats[key]) await this.openChat(key);
    } finally {
      this.pendingJoins.delete(id);
    }
  }

  // a members.get reply or members.updated event
  setMembers(r: { accountId: string; chatId: string; members: Contact[]; canAdd?: boolean }) {
    const key = chatKey(r.accountId, r.chatId);
    this.members[key] = r.members;
    this.membersCanAdd[key] = !!r.canAdd;
  }

  // Adds people to a group (Matrix: invites them) or removes one; throws with
  // the protocol's reason when that fails. The new member list follows as
  // members.updated.
  async editMembers(accountId: string, chatId: string, userIds: string[], remove: boolean): Promise<void> {
    const id = `${accountId}\u0002${chatId}\u0002${remove ? '-' : '+'}`;
    const result = new Promise<{ success: boolean; error: string }>((resolve) => {
      this.pendingMemberEdits.set(id, resolve);
      setTimeout(() => resolve({ success: false, error: 'No answer from the messenger.' }), 60000);
    });
    try {
      if (remove) await api.call('members.remove', { accountId, chatId, userId: userIds[0] });
      else await api.call('members.add', { accountId, chatId, userIds });
      const r = await result;
      if (!r.success) throw new Error(r.error || (remove ? 'Could not remove the member.' : 'Could not add the members.'));
    } finally {
      this.pendingMemberEdits.delete(id);
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

  // ---- updates ----

  update = $state<api.Update | null>(null);

  // Asks GitHub for a newer release (at most once a day unless forced) and
  // offers it in a toast once per version.
  async checkUpdates(force = false): Promise<api.Update | null> {
    const KEY = 'braids.update.checked';
    try {
      if (!force && Date.now() - Number(localStorage.getItem(KEY) ?? 0) < 24 * 3600 * 1000) return this.update;
      localStorage.setItem(KEY, String(Date.now()));
    } catch {
      // no storage: check every start
    }
    this.update = await api.checkUpdate().catch(() => null);
    const u = this.update;
    if (u && !force) {
      let told = '';
      try {
        told = localStorage.getItem('braids.update.told') ?? '';
        localStorage.setItem('braids.update.told', u.version);
      } catch {
        // tell again next time
      }
      if (told !== u.version) {
        const getIt = () => api.openUrl(u.url).catch((e) => this.toastError(e));
        this.toast(`braids ${u.version} is available.`, 'info', { label: 'Get it', run: getIt });
        // also as a desktop notification: the window is often hidden in the tray
        if (settings.notifications) {
          api.notify(`braids ${u.version} is available`, 'Update from the website: pebll.github.io/braids', getIt).catch(() => {});
        }
      }
    }
    return u;
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
  return { items: [], loading: false, atStart: false, stalled: false, loaded: false, firstUnreadId: '' };
}

const EXT_TYPES: Record<string, string> = {
  jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', gif: 'image/gif', webp: 'image/webp', bmp: 'image/bmp',
  svg: 'image/svg+xml', heic: 'image/heic', mp4: 'video/mp4', mov: 'video/quicktime', m4v: 'video/mp4',
  ogg: 'audio/ogg', opus: 'audio/opus', mp3: 'audio/mpeg', m4a: 'audio/mp4', pdf: 'application/pdf',
};

// Mime type of a file; from its name when the protocol (or an older braidsd) gives none.
export function fileMime(f: FileRef | null): string {
  if (!f) return '';
  if (f.mimeType) return f.mimeType;
  const name = f.name || f.path;
  const dot = name.lastIndexOf('.');
  return dot >= 0 ? (EXT_TYPES[name.slice(dot + 1).toLowerCase()] ?? '') : '';
}

// A local copy exists: downloaded, or a file sent from this computer.
export function fileReady(f: FileRef | null): boolean {
  return !!f && (f.status === 'downloaded' || (f.status === 'none' && f.path.startsWith('/')));
}

// images and videos of messages, for the viewer
export function galleryItems(messages: Message[]): LightboxItem[] {
  return messages
    .filter((x) => fileReady(x.file) && x.file!.kind !== 'sticker' && /^(image|video)\//.test(fileMime(x.file)))
    .map((x) => ({ path: x.file!.path, name: x.file!.name, video: fileMime(x.file).startsWith('video/'),
                   msg: { accountId: x.accountId, chatId: x.chatId, id: x.id } }));
}

// Where the "new messages" separator goes: the first received unread message
// after the last own or read one (as braidsd counts), and not above the
// chat's unread count (the newest that many received messages).
export function firstUnreadId(items: Message[], unreadCount: number): string {
  let lastRead = -1;
  items.forEach((m, i) => {
    if (m.isOutgoing || m.isRead) lastRead = i;
  });
  let first = items.findIndex((m, i) => i > lastRead && !m.isOutgoing && !m.isRead && !m.system);
  if (first < 0) return '';
  if (unreadCount > 0) {
    let seen = 0;
    for (let i = items.length - 1; i >= 0; i--) {
      const m = items[i];
      if (m.isOutgoing || m.system) continue;
      if (++seen === unreadCount) {
        if (i > first) first = i;
        break;
      }
    }
  }
  return items[first].id;
}

export function fileLabel(m: Message): string {
  const mime = fileMime(m.file);
  if (m.file?.kind === 'sticker') return 'Sticker';
  if (m.file?.kind === 'gif') return 'GIF';
  if (mime.startsWith('image/')) return 'Photo';
  if (mime.startsWith('video/')) return 'Video';
  if (isVoiceNote(m)) {
    const sec = m.file?.durationSec ?? 0;
    return sec > 0 ? `Voice message (${Math.floor(sec / 60)}:${String(sec % 60).padStart(2, '0')})` : 'Voice message';
  }
  if (mime.startsWith('audio/')) return 'Audio';
  return 'File';
}

// a voice message: Opus / AAC audio as the messengers' apps record it
export function isVoiceNote(m: Message): boolean {
  return /^audio\/(ogg|opus|aac|x-hx-aac|mp4)/.test(fileMime(m.file));
}

export const store = new Store();
