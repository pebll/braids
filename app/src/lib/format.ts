// format.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// Text and time formatting: message markup to HTML, times, initials, colors.

const AVATAR_COLORS = ['#93B5B0', '#C9A48A', '#A9B98C', '#8FA9C4', '#C4A0B9', '#B9AE8C', '#C2A28F', '#A6A2C9'];

function hash(s: string): number {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}

export function avatarColor(id: string): string {
  return AVATAR_COLORS[hash(id) % AVATAR_COLORS.length];
}

// --sender-1..8 for group sender names
export function senderColor(id: string): string {
  return `var(--sender-${(hash(id) % 8) + 1})`;
}

export function initials(name: string): string {
  const clean = name.replace(/[^\p{L}\p{N}\s]/gu, ' ').trim();
  if (!clean) return name.trim().slice(0, 1).toUpperCase() || '?';
  const words = clean.split(/\s+/);
  const first = Array.from(words[0])[0] ?? '';
  const second = words.length > 1 ? Array.from(words[words.length - 1])[0] ?? '' : '';
  return (first + second).toUpperCase();
}

export function startOfDay(t: number): number {
  const d = new Date(t);
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

export function timeOfDay(t: number): string {
  return new Date(t).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
}

// Chat list: time today, weekday this week, date before.
export function listTime(t: number, now = Date.now()): string {
  if (!t || t < 0) return '';
  const days = Math.round((startOfDay(now) - startOfDay(t)) / 86400000);
  if (days <= 0) return timeOfDay(t);
  if (days === 1) return 'Yesterday';
  if (days < 7) return new Date(t).toLocaleDateString([], { weekday: 'short' });
  const sameYear = new Date(t).getFullYear() === new Date(now).getFullYear();
  return new Date(t).toLocaleDateString([], sameYear ? { day: 'numeric', month: 'short' } : { day: 'numeric', month: 'short', year: 'numeric' });
}

export function dayLabel(t: number, now = Date.now()): string {
  const days = Math.round((startOfDay(now) - startOfDay(t)) / 86400000);
  if (days <= 0) return 'Today';
  if (days === 1) return 'Yesterday';
  if (days < 7) return new Date(t).toLocaleDateString([], { weekday: 'long' });
  const sameYear = new Date(t).getFullYear() === new Date(now).getFullYear();
  return new Date(t).toLocaleDateString([], sameYear ? { weekday: 'short', day: 'numeric', month: 'long' } : { day: 'numeric', month: 'long', year: 'numeric' });
}

export function isSameDay(a: number, b: number): boolean {
  return startOfDay(a) === startOfDay(b);
}

export function lastSeen(t: number): string {
  if (!t || t <= 0) return '';
  const mins = Math.round((Date.now() - t) / 60000);
  if (mins < 1) return 'last seen just now';
  if (mins < 60) return `last seen ${mins} min ago`;
  if (isSameDay(t, Date.now())) return `last seen at ${timeOfDay(t)}`;
  return `last seen ${listTime(t).toLowerCase()}`;
}

export function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);
}

// Plain one-line preview: markup characters removed.
export function plainText(text: string): string {
  return text
    .replace(/```([\s\S]*?)```/g, '$1')
    .replace(/`([^`\n]+)`/g, '$1')
    .replace(/(^|[\s(])[*_~]([^*_~\n]+)[*_~](?=$|[\s).,!?:;])/g, '$1$2')
    .replace(/@\[([^\]]+)\]/g, '@$1')
    .replace(/\s+/g, ' ')
    .trim();
}

const URL_RE = /\b(https?:\/\/[^\s<>"']+[^\s<>"'.,;:!?)\]])/g;

function inline(text: string, mentions: Record<string, string>, selfId: string): string {
  // tokens that must not be touched by the emphasis rules: code and mentions
  const slots: string[] = [];
  const keep = (html: string) => `\u0000${slots.push(html) - 1}\u0000`;

  let s = text.replace(/`([^`\n]+)`/g, (_, code) => keep(`<code>${escapeHtml(code)}</code>`));
  s = s.replace(/@\[([^\]]+)\]|@([\p{L}\p{N}_.-]+)/gu, (all, spaced, plain) => {
    const name = spaced ?? plain;
    const userId = mentions[name] ?? mentions[`@${name}`];
    if (userId === undefined && !spaced) return all;
    const isMe = userId !== undefined && userId === selfId;
    return keep(`<span class="mention${isMe ? ' me' : ''}">@${escapeHtml(name)}</span>`);
  });
  s = s.replace(URL_RE, (url) => keep(`<a href="${escapeHtml(url)}" data-link="1">${escapeHtml(url)}</a>`));

  s = escapeHtml(s);
  const emphasis: [RegExp, string][] = [
    [/(^|[\s(\u0000])\*([^*\n]+?)\*(?=$|[\s).,!?:;\u0000])/g, '$1<strong>$2</strong>'],
    [/(^|[\s(\u0000])_([^_\n]+?)_(?=$|[\s).,!?:;\u0000])/g, '$1<em>$2</em>'],
    [/(^|[\s(\u0000])~([^~\n]+?)~(?=$|[\s).,!?:;\u0000])/g, '$1<s>$2</s>'],
  ];
  for (const [re, rep] of emphasis) s = s.replace(re, rep);
  return s.replace(/\u0000(\d+)\u0000/g, (_, i) => slots[Number(i)]);
}

// Message text (internal markdown-like format, doc/API.md) to safe HTML.
export function renderText(text: string, mentions: Record<string, string> = {}, selfId = ''): string {
  const parts = text.split(/```(?:[a-z0-9_+-]*\n)?([\s\S]*?)```/g);
  let html = '';
  for (let i = 0; i < parts.length; i++) {
    if (i % 2 === 1) {
      html += `<pre><code>${escapeHtml(parts[i].replace(/\n$/, ''))}</code></pre>`;
    } else if (parts[i]) {
      html += parts[i].split('\n').map((line) => inline(line, mentions, selfId)).join('<br>');
    }
  }
  return html;
}

export function isOnlyEmoji(text: string): boolean {
  const t = text.trim();
  if (!t || t.length > 16) return false;
  return /^(\p{Extended_Pictographic}|\p{Emoji_Component}|‍|️|\s)+$/u.test(t) && !/^[\d#*\s]+$/.test(t);
}

export function fileSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(0)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}
