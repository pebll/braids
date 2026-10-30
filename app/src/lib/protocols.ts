// protocols.ts
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

import { siMatrix, siSignal, siTelegram, siWhatsapp } from 'simple-icons';

export interface ProtocolMeta {
  name: string;
  short: string;
  iconPath: string; // 24x24 svg path (simple-icons), empty: letter glyph
  color: string; // default account color
  description: string;
  warning?: string;
  qrSteps?: string[];
  linkTitle?: string;
  linkHint?: string;
}

export const ACCOUNT_COLORS = ['blue', 'green', 'violet', 'teal', 'amber', 'rose', 'sienna', 'slate'];

export const PROTOCOLS: Record<string, ProtocolMeta> = {
  Telegram: {
    name: 'Telegram',
    short: 'T',
    iconPath: siTelegram.path,
    color: 'blue',
    description: 'QR code or phone number',
    qrSteps: ['Open <b>Telegram</b> on your phone', 'Go to <b>Settings</b> → <b>Devices</b>', 'Tap <b>Link Desktop Device</b>', 'Point your phone at this code'],
    linkTitle: 'Link Telegram',
    linkHint: 'braids appears as a new device in Telegram.',
  },
  WhatsAppMd: {
    name: 'WhatsApp',
    short: 'W',
    iconPath: siWhatsapp.path,
    color: 'green',
    description: 'Link as a device with a QR code',
    warning: 'Unofficial client — WhatsApp may restrict accounts using it.',
    qrSteps: ['Open <b>WhatsApp</b> on your phone', 'Tap <b>Settings</b> → <b>Linked devices</b>', 'Tap <b>Link a device</b>', 'Point your phone at this code'],
    linkTitle: 'Link WhatsApp',
    linkHint: 'braids appears as a linked device, like WhatsApp Web.',
  },
  Signal: {
    name: 'Signal',
    short: 'S',
    iconPath: siSignal.path,
    color: 'violet',
    description: 'Link as a secondary device',
    qrSteps: ['Open <b>Signal</b> on your phone', 'Tap <b>Settings</b> → <b>Linked devices</b>', 'Tap <b>Link new device</b>', 'Point your phone at this code'],
    linkTitle: 'Link Signal',
    linkHint: 'braids appears as a linked device, like Signal Desktop.',
  },
  Matrix: {
    name: 'Matrix',
    short: 'M',
    iconPath: siMatrix.path,
    color: 'teal',
    description: 'Any homeserver, user id + password',
  },
  Dummy: {
    name: 'Dummy',
    short: 'D',
    iconPath: '',
    color: 'slate',
    description: 'Test protocol with demo chats (development)',
  },
};

export function protocolMeta(protocol: string): ProtocolMeta {
  return PROTOCOLS[protocol] ?? { name: protocol, short: protocol.slice(0, 1), iconPath: '', color: 'slate', description: '' };
}

// Labels and hints for login fields (doc/API.md "Login flows").
export const FIELD_INFO: Record<string, { label: string; placeholder?: string; hint?: string; mono?: boolean; autocomplete?: string }> = {
  phone: { label: 'Phone number', placeholder: '+49 151 2345 6789', hint: 'In international format, starting with + and the country code.', autocomplete: 'tel' },
  code: { label: 'Verification code', placeholder: '12345', autocomplete: 'one-time-code' },
  password: { label: 'Password', autocomplete: 'current-password' },
  userId: { label: 'Matrix user id', placeholder: '@name:example.org', autocomplete: 'username' },
  homeserver: { label: 'Homeserver', hint: 'Filled in from your user id. Change it only if your server lives elsewhere.' },
  recoveryKey: { label: 'Recovery key', placeholder: 'EsTj 3yST y93F …', hint: 'Paste it — spaces don’t matter.', mono: true },
  firstName: { label: 'First name' },
  lastName: { label: 'Last name' },
  passkeyResponse: { label: 'Passkey response', mono: true },
  confirm: { label: 'Confirm' },
};

export function stepTitle(protocol: string, field: string): string {
  const name = protocolMeta(protocol).name;
  switch (field) {
    case 'phone': return `${name} — phone number`;
    case 'code': return `${name} — confirm`;
    case 'password': return protocol === 'Matrix' ? 'Sign in to Matrix' : `${name} — two-step verification`;
    case 'userId':
    case 'homeserver': return 'Sign in to Matrix';
    case 'recoveryKey': return 'Unlock encrypted history';
    case 'firstName':
    case 'lastName': return `${name} — your name`;
    default: return `Connect ${name}`;
  }
}
