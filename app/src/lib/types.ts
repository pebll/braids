// types.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// Objects of the braidsd API (doc/API.md).

export type ProtocolName = 'Telegram' | 'WhatsAppMd' | 'Signal' | 'Matrix' | 'Dummy' | string;
export type AccountState = 'connecting' | 'online' | 'offline' | 'error';

export interface Capabilities {
  editWindowSec: number; // -1 unlimited, 0 none
  pinChat: boolean;
  archiveChat: boolean;
  pinMessage: boolean;
  limitedReactions: boolean;
  readReceiptsPerMessage: boolean;
  avatars: boolean;
  descriptions: boolean;
  joinChat: boolean;
  createGroup: boolean;
  muteChat: boolean;
  polls: boolean;
  pollsInGroupsOnly: boolean; // Telegram
}

export interface Account {
  accountId: string;
  protocol: ProtocolName;
  displayName: string;
  selfId: string;
  state: AccountState;
  error: string;
  syncing: boolean; // the protocol is fetching / syncing
  capabilities: Capabilities;
  options?: { showJoinedMessages?: boolean }; // braids' own (accounts.setOption)
}

export interface FileRef {
  id: string;
  path: string;
  name: string;
  mimeType: string;
  kind?: '' | 'sticker' | 'gif'; // shown without a bubble / muted and looping
  status: 'none' | 'notDownloaded' | 'downloading' | 'downloaded' | 'failed';
}

export interface PollOption {
  id: string;
  text: string;
  votes: number;
  voters: string[]; // user ids, where the messenger tells (not the own vote)
}

export interface Poll {
  question: string;
  options: PollOption[];
  multiple: boolean;
  closed: boolean;
  anonymous: boolean;
  own: string[]; // option ids of the own vote
  total: number; // people who voted
}

export interface Message {
  accountId: string;
  chatId: string;
  id: string;
  senderId: string;
  senderName: string;
  text: string;
  time: number;
  isOutgoing: boolean;
  isRead: boolean;
  isEdited: boolean;
  isDeleted: boolean;
  isPinned: boolean;
  hasMention: boolean;
  quoted: { id: string; text: string; senderId: string; senderName: string } | null;
  file: FileRef | null;
  reactions: { counts: Record<string, number>; senders: Record<string, string>; own: string };
  mentions: Record<string, string>;
  link: string;
  system: string; // group event ("joined", "added Pam"), shown as a line; '' for messages
  poll: Poll | null;
  // own message not yet with the messenger (braids' outbox, id "braids-out-…")
  pending?: 'waiting' | 'sending' | 'failed';
}

export interface Chat {
  accountId: string;
  id: string;
  name: string;
  isGroup: boolean;
  isUnread: boolean;
  isUnreadMention: boolean;
  isMuted: boolean;
  unreadCount: number; // received unread messages (group events not included)
  unreadEvents: number; // unread group events (joined, left, ...)
  isPinned: boolean;
  isArchived: boolean;
  lastActivity: number;
  lastMessage: Message | null;
  typing: string[];
}

export interface Contact {
  id: string;
  name: string;
  phone: string;
  isSelf: boolean;
}

export interface AuthPrompt {
  setupId?: string;
  accountId?: string;
  step: 'input' | 'qr' | 'info' | 'error';
  field: string;
  message: string;
  data: string;
  isSecret: boolean;
  isOptional: boolean;
}

export interface ApiEvent {
  method: string;
  params: any;
}

export class ApiError extends Error {
  constructor(public code: number, message: string) {
    super(message);
  }
}
