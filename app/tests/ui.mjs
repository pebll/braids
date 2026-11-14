// ui.mjs
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// End-to-end UI test: a fresh braidsd (Dummy protocol), the web UI from the
// Vite dev server (dev bridge instead of the Tauri shell) and Chromium via
// Playwright. Usage: node tests/ui.mjs [--braidsd PATH] [--screenshots DIR]

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const appDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const opt = (name, def) => (args.includes(name) ? args[args.indexOf(name) + 1] : def);
const braidsd = path.resolve(opt('--braidsd', path.join(appDir, '../build/bin/braidsd')));
const shots = opt('--screenshots', '');

let chromium;
try {
  ({ chromium } = require('playwright'));
} catch {
  ({ chromium } = await import('playwright'));
}

let failures = 0;
function check(cond, msg) {
  if (!cond) {
    failures++;
    console.log(`not ok - ${msg}`);
    throw new Error(msg);
  }
  console.log(`ok - ${msg}`);
}

const confdir = fs.mkdtempSync(path.join(os.tmpdir(), 'braids-ui-'));
const daemon = spawn(braidsd, ['--confdir', confdir], { stdio: ['ignore', 'ignore', 'inherit'] });
process.env.BRAIDS_CONFDIR = confdir;

const { createServer } = await import('vite');
const server = await createServer({ root: appDir, configFile: path.join(appDir, 'vite.config.ts'), server: { port: 1421, strictPort: false }, logLevel: 'warn' });
await server.listen();
const url = server.resolvedUrls.local[0];

// a fake microphone for voice messages
const browser = await chromium.launch({ args: ['--use-fake-ui-for-media-stream', '--use-fake-device-for-media-stream'] });
const errors = [];
try {
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 }, permissions: ['microphone', 'clipboard-read', 'clipboard-write'] });
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => m.type() === 'error' && !m.text().includes('Notification') && errors.push(m.text()));
  const shot = async (name) => shots && (await page.screenshot({ path: path.join(shots, `${name}.png`) }));
  const composer = page.locator('textarea[aria-label=Message]');

  await page.goto(url);
  await page.getByText('Welcome to braids').waitFor({ timeout: 30000 });
  check(true, 'welcome screen without accounts');
  await shot('welcome');

  // add account
  await page.getByRole('button', { name: /Dummy/ }).click();
  await page.locator('#auth-input').waitFor();
  check((await page.locator('label[for=auth-input]').textContent()) === 'Phone number', 'wizard asks for phone');
  await page.fill('#auth-input', '+46 700 1234');
  await page.keyboard.press('Enter');
  await page.waitForFunction(() => document.querySelector('label[for=auth-input]')?.textContent === 'Verification code');
  await page.fill('#auth-input', '000000');
  await page.keyboard.press('Enter');
  await page.locator('.field .error').waitFor();
  check(true, 'wrong code shows an error and asks again');
  await page.fill('#auth-input', '1234');
  await page.keyboard.press('Enter');
  await page.getByText('Done — go to inbox').waitFor({ timeout: 30000 });
  check(true, 'account added, syncing step');
  await page.getByText('Done — go to inbox').click();

  // chat list
  await page.getByRole('option', { name: /The Office/ }).waitFor({ timeout: 30000 });
  check((await page.getByRole('option').count()) >= 2, 'chat list shows the chats');
  // profile pictures: shown where the protocol has one, initials otherwise
  await page.waitForFunction(() => [...document.querySelectorAll('[role=option]')].some((o) => o.textContent.includes('Michael') && o.querySelector('.circle img')?.naturalWidth > 0));
  check(true, 'profile picture shown');
  const dwight = page.locator('.row', { has: page.locator('.name', { hasText: /^Dwight$/ }) });
  check(await dwight.locator('.circle img').count() === 0 && (await dwight.locator('.circle').textContent()) === 'D', 'initials without a picture');
  await page.fill('input[aria-label="Search chats"]', 'offi');
  check((await page.getByRole('option').count()) === 1, 'search filters chats');
  await page.locator('input[aria-label="Search chats"]').press('Enter');
  await page.locator('.conv header', { hasText: 'The Office' }).waitFor();
  await page.waitForFunction(() => document.activeElement?.getAttribute('aria-label') === 'Message');
  check(await page.locator('textarea[aria-label=Message]').inputValue() === '', 'Enter in the search opens the chat, no new line in the composer');
  await page.fill('input[aria-label="Search chats"]', '');

  // conversation
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.locator('[data-msg-id]').first().waitFor();
  check((await page.locator('.msg .sender').count()) > 0, 'group shows sender names');
  check((await page.locator('.system-line').first().textContent()) === 'Andy joined', 'group event shown as a system line');
  // search in the chat (Ctrl+F); closing it goes back to the composer
  await page.keyboard.press('Control+f');
  await page.locator('input[aria-label="Search in this chat"]').fill('headhunter');
  await page.keyboard.press('Escape');
  await page.waitForFunction(() => document.activeElement?.getAttribute('aria-label') === 'Message');
  check(await page.locator('input[aria-label="Search in this chat"]').count() === 0, 'closing the chat search focuses the composer');
  await page.keyboard.press('Control+f');
  await page.getByRole('button', { name: 'Close search' }).click();
  await page.waitForFunction(() => document.activeElement?.getAttribute('aria-label') === 'Message');
  check(true, 'the close button focuses the composer too');

  // a sender in a group: profile picture, private chat
  const michaelMsg = page.locator('.msg:not(.own)', { has: page.locator('.sender', { hasText: /^Michael$/ }) }).first();
  await michaelMsg.locator('.sender').click();
  await page.getByRole('menuitem', { name: 'View profile picture' }).click();
  await page.locator('.lightbox img').waitFor();
  check(true, 'group sender: profile picture in the viewer');
  await page.waitForFunction(() => decodeURIComponent(document.querySelector('.lightbox img')?.getAttribute('src') ?? '').includes('Michael_0-full'));
  check(true, 'the viewer switches to the full-size picture');
  await page.keyboard.press('Escape');
  await page.locator('.lightbox').waitFor({ state: 'detached' });
  await michaelMsg.locator('.sender').click();
  await page.getByRole('menuitem', { name: 'Message Michael privately' }).click();
  await page.locator('.conv header', { hasText: 'Michael' }).waitFor();
  check(await page.locator('.conv header', { hasText: 'The Office' }).count() === 0, 'group sender: message privately opens the private chat');
  // the picture from the info panel
  await page.keyboard.press('Control+i');
  await page.getByRole('button', { name: 'View profile picture' }).click();
  await page.locator('.lightbox img').waitFor();
  check(true, 'info panel: the profile picture opens in the viewer');
  await page.keyboard.press('Escape');
  await page.locator('.lightbox').waitFor({ state: 'detached' });
  await page.keyboard.press('Control+i');
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.locator('.conv header', { hasText: 'The Office' }).waitFor();
  await page.keyboard.press('Control+i');
  await page.locator('.info .member').first().waitFor();
  check(await page.locator('.info .member').count() === 5, 'member list shows 5');
  await page.getByRole('button', { name: 'Show all 6' }).click();
  check(await page.locator('.info .member').count() === 6, 'member list expands');

  // add and remove members (#38): Dummy lets self add anyone and remove all but Michael
  await page.locator('.info').getByRole('button', { name: 'Add people' }).click();
  check(await page.getByRole('dialog').getByRole('checkbox', { name: /Party Planning/ }).count() === 0, 'Add people lists no groups');
  await page.locator('input[aria-label="Search people"]').fill('kev');
  await page.getByRole('checkbox', { name: /Kevin/ }).click();
  await page.getByRole('dialog').getByRole('button', { name: 'Add', exact: true }).click();
  await page.locator('.info .member', { hasText: 'Kevin' }).waitFor();
  check(await page.locator('.info .member').count() === 7, 'Add people: Kevin added to the member list');
  await page.locator('.info .member', { hasText: 'Michael' }).click();
  check(await page.getByRole('menuitem', { name: 'Remove from group' }).count() === 0, 'no Remove for a member self may not remove');
  await page.keyboard.press('Escape');
  await page.locator('.info .member', { hasText: 'Kevin' }).click();
  await page.getByRole('menuitem', { name: 'Remove from group' }).click();
  await page.getByRole('button', { name: 'Remove', exact: true }).click();
  await page.locator('.info .member', { hasText: 'Kevin' }).waitFor({ state: 'detached' });
  check(await page.locator('.info .member').count() === 6, 'Remove from group: Kevin gone after confirming');
  await page.keyboard.press('Control+i');
  await composer.fill('Hello *team* `code`');
  await composer.press('Enter');
  const own = page.locator('.msg.own').last();
  await own.locator('strong', { hasText: 'team' }).waitFor();
  check((await own.locator('code').textContent()) === 'code', 'sent message rendered with markup');
  check((await page.getByRole('option', { name: /The Office/ }).textContent()).includes('You:'), 'chat preview shows own message');

  // @-mention suggestions from the group members
  await composer.fill('');
  await composer.pressSequentially('thanks @an');
  await page.locator('.mentions [role=option]', { hasText: 'Andy' }).waitFor();
  await page.waitForFunction(() => document.querySelectorAll('.mentions [role=option]').length === 1, null, { timeout: 5000 }).catch(() => {});
  check(await page.locator('.mentions [role=option]').count() === 1, 'mention suggestions filtered by name');
  await shot('mention');
  await composer.press('Enter');
  check((await composer.inputValue()) === 'thanks @Andy ', 'Enter inserts the mention');
  await composer.press('Enter');
  await page.locator('.msg.own', { hasText: 'thanks @Andy' }).last().waitFor();
  check(true, 'message with mention sent');

  await composer.fill('nice '); 
  await composer.pressSequentially(':thu');
  await page.locator('.emoji-suggest .glyph', { hasText: '👍' }).waitFor({ timeout: 5000 }).catch(() => {});
  const glyphs = await page.locator('.emoji-suggest .glyph').allTextContents();
  check(glyphs.includes('👍'), 'emoji suggestions appear after ":" and two letters');
  await shot('emoji-suggest');
  await composer.press('Enter');
  check((await composer.inputValue()) === `nice ${glyphs[0]}`, 'Enter inserts the emoji');
  // a time or a single letter is not a suggestion
  await composer.fill('at 12:30 and :t');
  await page.waitForTimeout(300);
  check(await page.locator('.emoji-suggest').count() === 0, 'no emoji suggestions for a time or a single letter');
  // arrows and Tab pick another one, Esc closes
  await composer.fill(':thu');
  await page.locator('.emoji-suggest .glyph', { hasText: '👍' }).waitFor({ timeout: 5000 }).catch(() => {});
  const second = await page.locator('.emoji-suggest .glyph').nth(1).textContent();
  await composer.press('ArrowDown');
  await composer.press('Tab');
  check((await composer.inputValue()) === second, 'arrows and Tab pick another suggestion');
  await composer.fill(':thu');
  await page.locator('.emoji-suggest').waitFor();
  await composer.press('Escape');
  await page.locator('.emoji-suggest').waitFor({ state: 'detached' });
  check(true, 'Esc closes the emoji suggestions');
  await composer.fill('');

  // one-line message: react and reply side by side
  const oneLiner = page.locator('.msg.own', { hasText: 'Hello team' }).last();
  await oneLiner.hover();
  check(await oneLiner.locator('.actions').evaluate((el) => getComputedStyle(el).flexDirection === 'row' && el.querySelectorAll('button').length === 2),
    'hover bar of a one-line message: react, reply, horizontal');
  // sender name + three lines: two more actions (copy text, forward)
  const threeLiner = page.locator('.msg', { hasText: 'storm your village' }).last();
  await threeLiner.hover();
  check(await threeLiner.locator('.actions').evaluate((el) => [...el.querySelectorAll('button')].map((b) => b.getAttribute('aria-label')).join(',')) === 'React,Reply,Copy text,Forward…',
    'hover bar grows with the message: react, reply, copy text, forward');
  const lastTheirs = page.locator('.msg:not(.own)').last();
  await lastTheirs.hover();
  await lastTheirs.getByRole('button', { name: 'Reply' }).click();
  await page.getByText(/^Replying to/).waitFor();
  await composer.fill('a reply');
  await composer.press('Enter');
  await page.locator('.msg.own .quote').last().waitFor();
  check(true, 'reply with quote');

  const last = page.locator('.msg.own').last();
  await last.hover();
  check(await last.locator('.actions').evaluate((el) => el.querySelectorAll('button').length >= 2), 'hover bar shown');
  await last.getByRole('button', { name: 'React', exact: true }).click();
  await page.locator('.react-pop button').first().click();
  await last.locator('.chip.mine').waitFor();
  check(true, 'reaction added');
  await last.locator('.chip.mine').click();
  await last.locator('.chip.mine').waitFor({ state: 'detached' });
  check(true, 'reaction removed');

  await composer.focus();
  await composer.press('ArrowUp');
  await page.getByText('Editing message').waitFor();
  await composer.fill('a reply, edited');
  await composer.press('Enter');
  await page.locator('.msg.own').last().getByText('edited', { exact: true }).waitFor();
  check((await page.locator('.msg.own').last().textContent()).includes('a reply, edited'), 'edit shown in place');
  await shot('conversation');

  await page.locator('.msg.own').last().click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Delete for everyone' }).click();
  await page.getByRole('button', { name: 'Delete for everyone' }).click();
  await page.waitForFunction(() => !document.body.textContent.includes('a reply, edited'));
  check(true, 'delete with confirmation');

  // more menu: copy, forward with origin line (to the same chat, so no other
  // chat's preview mentions it)
  await page.getByRole('option', { name: /Michael/ }).click();
  const theirs = page.locator('.msg:not(.own)').last();
  await theirs.waitFor();
  await theirs.hover();
  await shot('hover-bar');
  // two-line message: react and reply stacked, nothing more
  check(await theirs.locator('.actions').evaluate((el) => getComputedStyle(el).flexDirection === 'column' && el.querySelectorAll('button').length === 2),
    'hover bar of a two-line message: react, reply, vertical');
  await theirs.click({ button: 'right' });
  check(await page.getByRole('menuitem', { name: 'Copy text' }).count() === 1 && await page.getByRole('menuitem', { name: 'Open file' }).count() === 0,
    'context menu has copy, no open');
  await page.getByRole('menuitem', { name: 'Forward…' }).click();
  await page.getByText('Forward message').waitFor();
  check(await page.getByLabel('Add “Forwarded from” line').isChecked(), '"Forwarded from" line on by default');
  await page.fill('input[aria-label="Search chats to forward to"]', 'Michael');
  await page.locator('.target', { hasText: 'Michael' }).first().click();
  await page.locator('.toast', { hasText: 'Forwarded to Michael' }).waitFor();
  await page.locator('.msg.own', { hasText: 'Forwarded from Michael' }).last().waitFor();
  check(true, 'message forwarded with origin line');
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.locator('[data-msg-id]').first().waitFor();

  // draft survives switching chats
  await composer.fill('unsent words');
  await page.getByRole('option', { name: /Michael/ }).click();
  check((await page.getByRole('option', { name: /The Office/ }).textContent()).includes('Draft:'), 'draft shown in chat list');
  await page.getByRole('option', { name: /The Office/ }).click();
  check((await composer.inputValue()) === 'unsent words', 'draft restored');
  await composer.fill('');

  // images: a sent photo is shown inline (the type is guessed from the extension)
  const png = path.join(confdir, '..', `braids-ui-${process.pid}.png`);
  fs.writeFileSync(png, Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64'));
  await page.evaluate(async (filePath) => {
    const accounts = await (await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method: 'accounts.list' }) })).json();
    await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method: 'messages.send', params: { accountId: accounts.result[0].accountId, chatId: 'Michael_0', filePath } }) });
  }, png);
  await page.getByRole('option', { name: /Michael/ }).click();
  await page.waitForFunction(() => [...document.querySelectorAll('.msg .image img')].some((img) => img.naturalWidth > 0));
  check(true, 'sent image shown inline');
  const michaelRow = page.getByRole('option', { name: /Michael/ });
  await michaelRow.locator('.preview .thumb').waitFor();
  const rowText = await michaelRow.locator('.preview').textContent();
  check(rowText.includes('Photo') && !rowText.includes('.png'), 'chat list: a photo previewed as a thumbnail, without its file name');
  await page.locator('.msg.own', { has: page.locator('.image') }).last().click({ button: 'right' });
  check(await page.getByRole('menuitem', { name: 'Copy image' }).count() === 1, 'photo menu offers Copy image');
  await page.getByRole('menuitem', { name: 'Copy image' }).click();
  await page.locator('.toast', { hasText: 'Copied' }).waitFor();
  check(true, 'Copy image shows confirmation');

  // a pasted photo is previewed as a small tile before sending
  await page.evaluate(async () => {
    const b64 = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==';
    const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
    const dt = new DataTransfer();
    dt.items.add(new File([bytes], 'holiday.png', { type: 'image/png' }));
    document.querySelector('textarea[aria-label=Message]').dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true, cancelable: true }));
  });
  await page.waitForFunction(() => [...document.querySelectorAll('.composer .thumb img')].some((img) => img.naturalWidth > 0));
  check(true, 'attached photo previewed');
  await shot('attachment-preview');
  await page.locator('.composer .thumb .remove').click();
  check(await page.locator('.composer .thumb').count() === 0, 'preview removed with the attachment');

  // forwarding an image keeps the image and adds the origin line
  const images = () => page.locator('.msg.own .image').count();
  const imagesBefore = await images();
  await page.locator('.msg.own', { has: page.locator('.image') }).last().click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Forward…' }).click();
  await page.fill('input[aria-label="Search chats to forward to"]', 'Michael');
  await page.locator('.target', { hasText: 'Michael' }).first().click();
  await page.waitForFunction((n) => document.querySelectorAll('.msg.own .image').length > n, imagesBefore);
  check((await page.locator('.msg.own', { has: page.locator('.image') }).last().textContent()).includes('Forwarded from me'),
    'forwarded image with origin line');

  // voice messages: inline player with play / pause and time
  const wav = path.join(confdir, '..', `braids-ui-${process.pid}.wav`);
  {
    const rate = 8000;
    const samples = rate; // 1 s
    const buf = Buffer.alloc(44 + samples * 2);
    buf.write('RIFF', 0); buf.writeUInt32LE(36 + samples * 2, 4); buf.write('WAVE', 8); buf.write('fmt ', 12);
    buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(1, 22); buf.writeUInt32LE(rate, 24);
    buf.writeUInt32LE(rate * 2, 28); buf.writeUInt16LE(2, 32); buf.writeUInt16LE(16, 34); buf.write('data', 36); buf.writeUInt32LE(samples * 2, 40);
    for (let i = 0; i < samples; i++) buf.writeInt16LE(Math.round(Math.sin((i * 2 * Math.PI * 440) / rate) * 8000), 44 + i * 2);
    fs.writeFileSync(wav, buf);
  }
  await page.evaluate(async (filePath) => {
    const accounts = await (await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method: 'accounts.list' }) })).json();
    await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method: 'messages.send', params: { accountId: accounts.result[0].accountId, chatId: 'Michael_0', filePath } }) });
  }, wav);
  const voice = page.locator('.msg .voice').last();
  await voice.waitFor();
  await page.waitForFunction(() => document.querySelector('.msg:last-of-type .voice .time, .voice .time')?.textContent === '0:01', null, { timeout: 10000 }).catch(() => {});
  check((await voice.locator('.time').textContent()) === '0:01', 'voice message shows its duration');
  await voice.getByRole('button', { name: 'Play', exact: true }).click();
  await voice.getByRole('button', { name: 'Pause' }).waitFor();
  check(true, 'voice message plays');
  await voice.getByRole('button', { name: 'Playback speed' }).click();
  check((await voice.getByRole('button', { name: 'Playback speed' }).textContent()) === '1.5×', 'playback speed');
  await voice.getByRole('button', { name: 'Play', exact: true }).waitFor({ timeout: 5000 });
  check(true, 'voice message ends');

  // videos: first frame with a play mark in the chat, played in the viewer
  await page.evaluate(async () => {
    const canvas = document.createElement('canvas');
    canvas.width = 160;
    canvas.height = 120;
    const g = canvas.getContext('2d');
    const rec = new MediaRecorder(canvas.captureStream(25), { mimeType: 'video/webm' });
    const chunks = [];
    rec.ondataavailable = (e) => chunks.push(e.data);
    rec.start(100);
    for (let i = 0; i < 20; i++) {
      g.fillStyle = `hsl(${i * 18}, 70%, 50%)`;
      g.fillRect(0, 0, 160, 120);
      await new Promise((r) => setTimeout(r, 40));
    }
    await new Promise((r) => { rec.onstop = r; rec.stop(); });
    const up = await (await fetch('/__braids/upload?name=clip.webm', { method: 'POST', body: new Blob(chunks, { type: 'video/webm' }) })).json();
    const accounts = await (await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method: 'accounts.list' }) })).json();
    await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method: 'messages.send', params: { accountId: accounts.result[0].accountId, chatId: 'Michael_0', filePath: up.path } }) });
  });
  const clip = page.locator('.msg .video-thumb').last();
  await clip.waitFor();
  check(await clip.locator('.play-mark').count() === 1, 'video shown with its first frame and a play mark');
  await clip.click();
  await page.locator('.lightbox video').waitFor();
  check(true, 'video plays in the viewer');
  check(await page.locator('.lightbox .lb-zoom').count() === 0, 'no zoom for videos');
  await page.keyboard.press('k');
  await page.locator('.lightbox img').waitFor();
  check(true, 'viewer switches from a video to the images');
  await page.keyboard.press('Escape');

  // recording a voice message: mic when the box is empty, cancel and send
  await composer.fill('');
  await page.getByRole('button', { name: 'Record voice message' }).click();
  await page.locator('.recording .rec-time').waitFor();
  check(true, 'recording bar shown');
  await page.keyboard.press('Escape');
  await page.locator('.recording').waitFor({ state: 'detached' });
  check(true, 'Esc cancels the recording');
  const voicesBefore = await page.locator('.msg.own .voice').count();
  await page.getByRole('button', { name: 'Record voice message' }).click();
  await page.locator('.recording').waitFor();
  await page.waitForTimeout(1300);
  await page.getByRole('button', { name: 'Send voice message' }).click();
  await page.waitForFunction((n) => document.querySelectorAll('.msg.own .voice').length > n, voicesBefore, { timeout: 15000 });
  check(true, 'recorded voice message sent and shown with the player');
  await composer.fill('x');
  check(await page.getByRole('button', { name: 'Record voice message' }).count() === 0, 'mic replaced by send while typing');
  await composer.fill('');

  // shared media: across all chats, images and links, "Show in chat"
  await composer.fill('docs at https://example.org/braids');
  await composer.press('Enter');
  await page.getByRole('button', { name: 'Shared media' }).click();
  await page.locator('[role=dialog] .thumb img').first().waitFor();
  check(await page.locator('[role=dialog] .thumb').count() >= 2, 'shared media lists the images of all chats');
  await shot('shared-media');
  await page.getByRole('tab', { name: 'Links' }).click();
  await page.locator('[role=dialog] .row', { hasText: 'example.org' }).waitFor();
  check(true, 'shared links listed');
  await shot('shared-links');
  await page.getByRole('tab', { name: 'Files' }).click();
  await page.locator('[role=dialog] .row', { hasText: '.wav' }).waitFor();
  check(true, 'files tab lists the audio file');
  await page.getByRole('tab', { name: 'Media' }).click();
  await page.locator('[role=dialog] .tile').first().hover();
  await page.locator('[role=dialog] .goto').first().click();
  await page.waitForFunction(() => !document.querySelector('[aria-labelledby=media-title]'));
  check(true, 'show in chat closes the dialog');
  await page.keyboard.press('Control+i');
  await page.locator('.info .media img').first().waitFor();
  await page.getByRole('button', { name: 'Show all' }).click();
  check((await page.locator('#media-title').textContent()).startsWith('Shared in'), 'chat media from the info panel');
  await page.keyboard.press('Escape');
  await page.locator('.info .actions').getByRole('button', { name: 'Media' }).click();
  check((await page.locator('#media-title').textContent()).startsWith('Shared in'), 'Media button in the chat info');
  await page.keyboard.press('Escape');

  // image viewer: switch, zoom, pan, close
  await page.locator('.msg .image:not(.video-thumb)').last().click();
  await page.locator('.lightbox img').waitFor();
  const pos = () => page.locator('.lb-pos').textContent();
  const first = await pos();
  check(/^\d+ \/ \d+$/.test(first), `viewer shows the position among the chat's images (${first})`);
  await page.keyboard.press('k');
  check((await pos()) !== first, 'k shows the previous image');
  await page.keyboard.press('j');
  check((await pos()) === first, 'j shows the next image');
  await page.keyboard.press('+');
  check((await page.locator('.lb-zoom').textContent()) === '150%', '+ zooms in');
  await page.keyboard.press('k');
  check((await pos()) !== first && (await page.locator('.lightbox video').count() === 1 || (await page.locator('.lb-zoom').textContent()) === '100%'),
    'k switches while zoomed (next item fitted)');
  await page.keyboard.press('j');
  await page.keyboard.press('+');
  const box = await page.locator('.lb-stage').boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width / 2 + 60, box.y + box.height / 2 + 30);
  await page.mouse.up();
  check((await page.locator('.lightbox img').getAttribute('style')).includes('translate(60px, 30px)'), 'drag moves the zoomed image');
  await page.locator('.lb-stage').hover();
  await page.mouse.wheel(0, -300);
  check(parseInt(await page.locator('.lb-zoom').textContent()) > 150, 'wheel zooms in');
  await page.keyboard.press('0');
  check((await page.locator('.lb-zoom').textContent()) === '100%', '0 resets the zoom');
  // Ctrl + - 0 zoom the picture, not the app (#31)
  const appZoom = () => page.evaluate(() => document.getElementById('app').style.zoom || '1');
  const zoomBefore = await appZoom();
  await page.keyboard.press('Control+=');
  check((await page.locator('.lb-zoom').textContent()) === '150%', 'Ctrl+= zooms the picture');
  await page.keyboard.press('Control+-');
  await page.keyboard.press('Control+0');
  check((await page.locator('.lb-zoom').textContent()) === '100%' && (await appZoom()) === zoomBefore && await page.locator('.text-size-notice').count() === 0,
    'Ctrl + - 0 in the viewer leave the text size alone');
  await page.keyboard.press('Escape');
  await page.locator('.lightbox').waitFor({ state: 'detached' });
  check(true, 'Esc closes the viewer');

  // viewer actions: copy, Ctrl+C, show in chat
  await page.locator('.msg .image:not(.video-thumb)').last().click();
  await page.locator('.lightbox img').waitFor();
  check(await page.locator('.lightbox').getByRole('button', { name: 'Download' }).count() === 1, 'viewer: download button');
  await page.evaluate(() => navigator.clipboard.writeText(''));
  await page.locator('.lightbox').getByRole('button', { name: 'Copy image' }).click();
  await page.waitForFunction(async () => (await navigator.clipboard.read()).some((i) => i.types.includes('image/png')));
  check(true, 'viewer: Copy image puts a PNG on the clipboard');
  await page.evaluate(() => navigator.clipboard.writeText(''));
  await page.keyboard.press('Control+c');
  await page.waitForFunction(async () => (await navigator.clipboard.read()).some((i) => i.types.includes('image/png')));
  check(true, 'viewer: Ctrl+C copies the image');
  await page.locator('.lightbox').getByRole('button', { name: 'Show in chat' }).click();
  await page.locator('.lightbox').waitFor({ state: 'detached' });
  check(true, 'viewer: Show in chat closes the viewer');

  // no browser context menu outside text fields
  const prevented = (sel) => page.locator(sel).first().evaluate((el) => {
    const ev = new MouseEvent('contextmenu', { bubbles: true, cancelable: true });
    el.dispatchEvent(ev);
    return ev.defaultPrevented;
  });
  check(await prevented('.conv header .who-sub, .conv header'), 'no browser menu on the app');
  check(!(await prevented('textarea[aria-label=Message]')), 'text fields keep their menu');
  await page.keyboard.press('Control+i');

  // unread message counts
  const kevinRow = page.locator('.chatlist .row', { has: page.locator('.name', { hasText: /^Kevin$/ }) });
  check((await kevinRow.locator('.unread-count').textContent()) === '2', 'unread message count in the list');
  // hover preview (#40): the unread messages in full, the chat stays unread
  await kevinRow.hover();
  const chatPreview = page.locator('.chat-preview');
  await chatPreview.locator('.msg').first().waitFor({ timeout: 5000 });
  check(await chatPreview.locator('.msg').count() === 2 && (await chatPreview.locator('.name').textContent()) === 'Kevin',
    'hover preview shows the 2 unread messages');
  check((await chatPreview.textContent()).includes('Mini-cupcakes'), 'hover preview: the message text');
  await shot('hover-preview');
  await page.mouse.move(5, 5);
  await chatPreview.waitFor({ state: 'detached' });
  check((await kevinRow.locator('.unread-count').textContent()) === '2', 'previewed chat stays unread');
  check((await page.getByRole('button', { name: /All chats/ }).locator('.count').textContent()) === '1', 'folder counts chats, not messages');
  check(!(await page.getByRole('button', { name: /All chats/ }).locator('.count').getAttribute('class')).includes('muted'), 'All chats count in the accent color');
  check(await page.getByRole('button', { name: /^Unread/ }).locator('.count').count() === 0, 'no count on the Unread folder');
  const accountCount = page.locator('nav.sidebar .item[title*="—"] .count').first();
  check((await accountCount.getAttribute('class')).includes('muted'), 'account count grey');
  await page.keyboard.press('Control+,');
  await page.getByRole('switch', { name: 'Unread counts on Favorites and accounts' }).click();
  await page.keyboard.press('Escape');
  check(await page.locator('nav.sidebar .item[title*="—"] .count').count() === 0, 'account counts hidden by the setting');
  // a newer version: a dot on Settings, which opens on About (with its own dot)
  check(await page.locator('nav.sidebar .update-dot').count() === 0, 'no update dot when up to date');
  await page.evaluate(() => { window.__braids.store.update = { version: '9.9.9', url: 'https://example.org' }; });
  await page.locator('nav.sidebar .update-dot').waitFor();
  await page.getByRole('button', { name: 'Settings (update available)' }).click();
  await page.locator('.settings .about').waitFor();
  check(await page.locator('.settings nav .nav-dot').count() === 1, 'update dot on Settings opens About, dot on About');
  await page.keyboard.press('Escape');
  await page.evaluate(() => { window.__braids.store.update = null; });
  await page.locator('nav.sidebar .update-dot').waitFor({ state: 'detached' });
  await page.keyboard.press('Control+,');
  await page.getByRole('switch', { name: 'Unread counts on Favorites and accounts' }).click();
  await page.keyboard.press('Escape');
  // only a group event new (Angela joined): grey, not bold, not counted
  const partyRow = page.locator('.chatlist .row', { has: page.locator('.name', { hasText: /^Party Planning$/ }) });
  check(await partyRow.locator('.unread-count.muted').textContent() === '1', 'group event only: grey count');
  check(!(await partyRow.getAttribute('class')).includes('unread'), 'group event only: row not bold');
  check((await partyRow.textContent()).includes('Angela joined'), 'group event in the preview');

  // mark as unread: a count of one until opened
  const pamRow2 = page.locator('.chatlist .row', { has: page.locator('.name', { hasText: /^Pam$/ }) });
  await pamRow2.click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Mark as unread' }).click();
  await pamRow2.locator('.unread-count').waitFor();
  check((await pamRow2.locator('.unread-count').textContent()) === '1', 'chat marked unread');
  await pamRow2.click();
  await pamRow2.locator('.unread-count').waitFor({ state: 'detached' });
  check(true, 'opening clears the unread mark');

  // Unread folder: only unread chats (and the open one while it is read)
  await page.keyboard.press('Control+u');
  await page.locator('.chatlist h2', { hasText: 'Unread' }).waitFor();
  check(await page.locator('.chatlist .row:not(.selected)').count() >= 1 &&
    await page.locator('.chatlist .row:not(.selected):not(.unread)').count() === 0, 'Unread folder lists the unread chats');
  await kevinRow.click();
  await page.locator('[data-unread-marker]').waitFor();
  const markerBox = await page.locator('[data-unread-marker]').boundingBox();
  const scrollBox = await page.locator('.messages').boundingBox();
  check(markerBox.y >= scrollBox.y && markerBox.y + markerBox.height <= scrollBox.y + scrollBox.height, 'an unread chat opens at the New messages line');
  await kevinRow.locator('.unread-count').waitFor({ state: 'detached' });
  check(await kevinRow.count() === 1, 'the open chat stays in Unread once read');
  await page.keyboard.press('Control+a');
  await page.locator('.chatlist h2', { hasText: 'All chats' }).waitFor();

  // loading / outdated indicator: network gone and back
  const accountItem = page.locator('.sidebar .item', { has: page.locator('.dot') }).first();
  check(await accountItem.locator('.dot.online').count() === 1 && (await accountItem.getAttribute('title')).includes('Online'),
    'account shown online (green dot, tooltip)');
  const reconnects = [];
  page.on('request', (r) => r.url().includes('/__braids/call') && (r.postData() ?? '').includes('accounts.reconnect') && reconnects.push(r));
  await page.context().setOffline(true);
  await page.locator('.chatlist .sync', { hasText: 'Offline' }).waitFor();
  check(true, 'offline shown');
  check(await accountItem.locator('.dot.offline').count() === 1 && (await accountItem.getAttribute('title')).includes('no network'),
    'accounts shown offline at once without network');
  await page.context().setOffline(false);
  await page.locator('.chatlist .sync', { hasText: 'Updating' }).waitFor();
  check(reconnects.length >= 1, 'network back: the accounts are asked to reconnect');
  await shot('updating');
  await page.locator('.chatlist .sync').waitFor({ state: 'detached', timeout: 15000 });
  check(true, 'updating shown until the accounts caught up');

  // long chats: only a window of messages is rendered, more while scrolling up
  await page.evaluate(async () => {
    const call = async (method, params) => (await (await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method, params }) })).json()).result;
    const accounts = await call('accounts.list', {});
    for (let i = 1; i <= 200; i++) await call('messages.send', { accountId: accounts[0].accountId, chatId: 'Oscar_0', text: `bulk ${i}` });
  });
  await page.getByRole('option', { name: /Oscar/ }).click();
  await page.locator('.msg', { hasText: 'bulk 200' }).waitFor();
  const count = () => page.locator('.messages [data-msg-id]').count();
  // load the whole history by scrolling up
  for (let i = 0; i < 40 && (await count()) <= 200; i++) {
    await page.locator('.messages').evaluate((el) => { el.scrollTop = 0; });
    await page.waitForTimeout(200);
  }
  const total = await count();
  check(total > 200, `whole history rendered after scrolling up (${total})`);
  // back at the bottom: only the newest are rendered
  await page.locator('.messages').evaluate((el) => { el.scrollTop = el.scrollHeight; });
  await page.waitForFunction(() => document.querySelectorAll('.messages [data-msg-id]').length <= 150);
  const windowed = await count();
  check(windowed === 150, `back at the bottom: window of ${windowed} messages`);
  // scrolling up reveals loaded messages, keeping the view in place
  await page.locator('.messages').evaluate((el) => { el.scrollTop = 0; });
  await page.waitForFunction((n) => document.querySelectorAll('.messages [data-msg-id]').length > n, windowed);
  check(await count() === Math.min(total, windowed + 60), 'scrolling up reveals up to 60 more');
  await page.waitForTimeout(100);
  check(await page.locator('.messages').evaluate((el) => el.scrollTop > 200), 'view kept in place while revealing');
  await page.getByRole('option', { name: /The Office/ }).click();

  // groups have a rounded-square avatar, people a round one
  check(await page.getByRole('option', { name: /The Office/ }).locator('.circle.group').count() === 1, 'group avatar is a rounded square');
  check(await page.getByRole('option', { name: /Michael/ }).locator('.circle.group').count() === 0, 'person avatar is round');

  // reopening a chat shows the latest messages, not the old position
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.locator('.messages').evaluate((el) => { el.scrollTop = 0; });
  await page.getByRole('option', { name: /Michael/ }).click();
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.waitForTimeout(300);
  check(await page.locator('.messages').evaluate((el) => el.scrollHeight > el.clientHeight && el.scrollHeight - el.scrollTop - el.clientHeight < 60),
    'reopened chat scrolled to the latest message');
  // a chat opened for the first time (history loaded on opening)
  await page.getByRole('option', { name: /Kelly/ }).click();
  await page.locator('[data-msg-id]').first().waitFor();
  await page.waitForTimeout(500);
  check(await page.locator('.messages').evaluate((el) => el.scrollHeight - el.scrollTop - el.clientHeight < 60), 'first opening shows the latest message');
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.locator('[data-msg-id]').first().waitFor();

  // categories: favorite, default, archived
  await page.getByRole('option', { name: /Dwight/ }).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Add to favorites' }).click();
  check(await (await page.locator('.chatlist .row', { has: page.locator('.name', { hasText: /^Dwight$/ }) }).locator('.circle').getAttribute('style')).includes('--favorite'), 'favorite marked in the list');
  check(await page.locator('.chatlist .section').count() === 0, 'no favorites section, sorted by date');
  await page.getByRole('button', { name: 'Favorites' }).click();
  check((await page.getByRole('option').count()) === 1, 'favorites folder');
  await shot('favorites');
  await page.getByRole('button', { name: 'All chats' }).click();

  // mute: braids' own setting, shown in the list
  const pamRow = page.locator('.chatlist .row', { has: page.locator('.name', { hasText: /^Pam$/ }) });
  await pamRow.click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Mute', exact: true }).click();
  await pamRow.locator('.flag[title=Muted]').waitFor();
  check(true, 'muted chat marked in the list');
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.locator('.system-line').first().scrollIntoViewIfNeeded();
  await shot('mute-and-system-line');
  await pamRow.click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Unmute' }).click();
  await pamRow.locator('.flag[title=Muted]').waitFor({ state: 'detached' });
  check(true, 'chat unmuted');

  const archivedBadge = () => page.locator('.sidebar .item', { hasText: 'Archived' }).locator('.count');
  const archivedBefore = await archivedBadge().count() ? Number(await archivedBadge().textContent()) : 0;
  await page.getByRole('option', { name: /Jim/ }).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Mark as unread' }).click();
  await page.getByRole('option', { name: /Jim/ }).locator('.unread-count').waitFor();
  await page.getByRole('option', { name: /Jim/ }).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Archive' }).click();
  check((await page.getByRole('option', { name: /Jim/ }).count()) === 0, 'archived chat leaves the inbox');
  await page.waitForFunction((n) => Number(document.querySelector('.sidebar .item:has(.label) .count.muted')?.closest('.item')?.textContent?.match(/Archived\s*(\d+)/)?.[1] ?? 0) === n + 1, archivedBefore);
  check(true, 'Archived shows a grey count of its chats with new messages');
  await page.getByRole('button', { name: /Archived/ }).click();
  check((await page.getByRole('option', { name: /Jim/ }).count()) === 1, 'archived chat in Archived');
  check(await page.getByRole('option', { name: /Jim/ }).locator('.unread-count.muted').count() === 1, 'archived chat: grey unread count, like a muted chat');
  await page.getByRole('option', { name: /Jim/ }).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Unarchive' }).click();
  await page.getByRole('button', { name: 'All chats' }).click();
  check((await page.getByRole('option', { name: /Jim/ }).count()) === 1, 'unarchived chat back in the inbox');

  // join a group by link
  await page.getByRole('button', { name: 'New chat' }).click();
  await page.getByRole('tab', { name: 'Join a group' }).click();
  await page.fill('input[aria-label="Invite link or address"]', 'https://dummy.example/nope');
  await page.getByRole('button', { name: 'Join', exact: true }).click();
  await page.locator('.join .error').waitFor();
  check((await page.locator('.join .error').textContent()).includes('Not a Dummy invite link'), 'invalid invite link reported');
  await shot('join-group');
  await page.fill('input[aria-label="Invite link or address"]', 'https://dummy.example/join/Accounting');
  await page.getByRole('button', { name: 'Join', exact: true }).click();
  await page.locator('.conv header', { hasText: 'Accounting' }).waitFor();
  check(await page.getByText('Join a group').count() === 0, 'joined group opened');

  // an invite link in a message opens "Join a group" with the link
  await composer.fill('come to https://dummy.example/join/Sales');
  await composer.press('Enter');
  await page.locator('.msg.own a[data-link]', { hasText: 'dummy.example/join/Sales' }).last().click();
  await page.getByRole('tab', { name: 'Join a group', selected: true }).waitFor();
  check((await page.inputValue('input[aria-label="Invite link or address"]')) === 'https://dummy.example/join/Sales', 'invite link in a message opens Join a group');
  await page.getByRole('button', { name: 'Join', exact: true }).click();
  await page.locator('.conv header', { hasText: 'Sales' }).waitFor();
  check(true, 'joined from the link');
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.keyboard.press('Control+i');
  await page.getByText('Group info').waitFor();
  await shot('inbox');
  await page.keyboard.press('Control+i');

  // info panel and theme
  await page.keyboard.press('Control+i');
  await page.getByText('Group info').waitFor();
  check(true, 'info panel opens');
  await page.locator('.info .description', { hasText: 'Dunder Mifflin Scranton' }).waitFor();
  check(await page.locator('.info .description a[data-link]').count() === 1, 'group description shown with link');
  // default shortcuts
  await page.keyboard.press('Control+Space');
  check(await page.evaluate(() => document.activeElement?.getAttribute('aria-label') === 'Search chats'), 'Ctrl+Space focuses chat search');
  await page.keyboard.press('Escape');
  const selectedName = () => page.locator('[role=option][aria-selected=true] .name').textContent();
  const before = await selectedName();
  const cursorName = () => page.locator('.chatlist .row.cursor .name').textContent();
  await page.keyboard.press('Alt+j');
  await page.locator('.chatlist .row.cursor').waitFor();
  check((await selectedName()) === before && (await cursorName()) !== before &&
    await page.evaluate(() => document.activeElement?.getAttribute('aria-label') === 'Chats'), 'Alt+J highlights the next chat without opening it');
  const highlighted = await cursorName();
  await page.keyboard.press('Enter');
  await page.waitForFunction((n) => document.querySelector('[role=option][aria-selected=true] .name')?.textContent === n, highlighted);
  await page.waitForFunction(() => document.activeElement?.getAttribute('aria-label') === 'Message');
  check(await page.locator('textarea[aria-label=Message]').inputValue() === '' && await page.locator('.chatlist .row.cursor').count() === 0,
    'Enter opens the highlighted chat, cursor in the composer');
  await page.keyboard.press('Alt+k');
  await page.keyboard.press('Enter');
  await page.waitForFunction((b) => document.querySelector('[role=option][aria-selected=true] .name')?.textContent === b, before);
  check(true, 'Alt+K and Enter go back to the previous chat');
  await page.keyboard.press('Alt+h');
  await page.locator('.chatlist .row.cursor').waitFor();
  check((await cursorName()) === before && await page.evaluate(() => document.activeElement?.getAttribute('aria-label') === 'Chats'),
    'Alt+H goes back to the chat list, at the open chat');
  await page.keyboard.press('j');
  check((await cursorName()) !== before && (await selectedName()) === before, 'j in the list moves the highlight');
  await page.locator('textarea[aria-label=Message]').focus();
  check(await page.locator('.chatlist .row.cursor').count() === 0, 'the highlight goes away when the list loses the focus');
  // search: the highlight moves through the results, the search keeps the focus
  await page.keyboard.press('Control+Space');
  await page.keyboard.type('i');
  await page.locator('.chatlist .row.cursor').waitFor();
  const firstHit = await cursorName();
  await page.keyboard.press('Alt+j');
  check((await cursorName()) !== firstHit && await page.evaluate(() => document.activeElement?.getAttribute('aria-label') === 'Search chats'),
    'Alt+J in the search moves the highlight, typing goes on in the search');
  const target = await cursorName();
  await page.keyboard.press('Enter');
  await page.waitForFunction((n) => document.querySelector('[role=option][aria-selected=true] .name')?.textContent === n, target);
  check(true, 'Enter in the search opens the highlighted result');
  await page.fill('input[aria-label="Search chats"]', '');
  await page.getByRole('option', { name: new RegExp(before) }).first().click();
  await page.keyboard.press('Control+b');
  await page.locator('.chatlist h2', { hasText: 'Favorites' }).waitFor();
  check(true, 'Ctrl+B shows favorites');
  await page.keyboard.press('Control+a');
  await page.locator('.chatlist h2', { hasText: 'All chats' }).waitFor();
  check(true, 'Ctrl+A shows all chats');
  // AZERTY: the key labelled A sits where QWERTY has Q (code KeyQ)
  const azerty = (key, code) => page.evaluate(([key, code]) => {
    (document.activeElement ?? document.body).dispatchEvent(new KeyboardEvent('keydown', { key, code, ctrlKey: true, bubbles: true, cancelable: true }));
  }, [key, code]);
  await page.keyboard.press('Control+b');
  await page.locator('.chatlist h2', { hasText: 'Favorites' }).waitFor();
  await azerty('q', 'KeyA');
  await page.waitForTimeout(200);
  check(await page.locator('.chatlist h2', { hasText: 'Favorites' }).count() === 1, 'AZERTY Ctrl+Q (physical A) is not All chats');
  await azerty('a', 'KeyQ');
  await page.locator('.chatlist h2', { hasText: 'All chats' }).waitFor();
  check(true, 'shortcuts follow the keyboard layout (AZERTY Ctrl+A)');
  // a custom layout typing "," on the US M key: Ctrl+, is Settings, not Matrix
  await azerty(',', 'KeyM');
  await page.getByRole('button', { name: 'Keyboard' }).waitFor();
  check(await page.locator('.chatlist h2', { hasText: 'All chats' }).count() === 1, 'Ctrl+, on a custom layout opens Settings (not the US Ctrl+M)');
  await page.keyboard.press('Escape');
  // a non-latin layout still uses the US position (Cyrillic г on U)
  await azerty('г', 'KeyU');
  await page.locator('.chatlist h2', { hasText: 'Unread' }).waitFor();
  check(true, 'non-latin layout: Ctrl+<letter> by the US position');
  await page.keyboard.press('Control+a');
  await page.locator('.chatlist h2', { hasText: 'All chats' }).waitFor();

  // go to: the search is cleared, the first chat lightly highlighted, full
  // blue once moved; no mouse hover meanwhile
  await page.locator('.chatlist .row').nth(2).hover();
  await page.fill('input[aria-label="Search chats"]', 'o');
  await page.keyboard.press('Control+u');
  await page.locator('.chatlist h2', { hasText: 'Unread' }).waitFor();
  check(await page.inputValue('input[aria-label="Search chats"]') === '', 'go to clears the search');
  await page.keyboard.press('Control+a');
  await page.locator('.chatlist h2', { hasText: 'All chats' }).waitFor();
  const firstRow = page.locator('.chatlist .row').first();
  check((await firstRow.getAttribute('class')).includes('cursor') && await page.locator('.chatlist .list.soft').count() === 1,
    'go to: the first chat is lightly highlighted');
  check(await page.locator('.chatlist .list.kbd').count() === 1, 'go to: no mouse hover until the mouse moves');
  await page.keyboard.press('j');
  check(await page.locator('.chatlist .list.soft').count() === 0 && (await page.locator('.chatlist .row').nth(1).getAttribute('class')).includes('cursor'),
    'moving makes the highlight full');
  await page.mouse.move(5, 5);
  await page.locator('.chatlist .row').nth(3).hover();
  check(await page.locator('.chatlist .list.kbd').count() === 0, 'the mouse brings the hover back');

  // changing a shortcut
  await page.keyboard.press('Control+,');
  await page.getByRole('button', { name: 'Keyboard' }).click();
  await page.getByRole('button', { name: 'Add shortcut for Search chats' }).click();
  await page.keyboard.press('l');
  await page.locator('.record-error').waitFor();
  check(true, 'plain key rejected as shortcut');
  await page.keyboard.press('Control+e');
  await page.locator('.toast', { hasText: 'removed from' }).first().waitFor();
  check(await page.locator('[data-action=searchChats] .combo').count() === 2 &&
    await page.locator('[data-action=editLast] .combo').count() === 0, 'recorded shortcut moves from the other action');
  await page.keyboard.press('Escape');
  await page.keyboard.press('Control+e');
  check(await page.evaluate(() => document.activeElement?.getAttribute('aria-label') === 'Search chats'), 'changed shortcut works');
  await page.keyboard.press('Escape');

  // shortcuts overview: opens with Ctrl+/, shows your own bindings, closes with Esc
  await page.keyboard.press('Control+/');
  const overview = page.getByRole('dialog', { name: 'Keyboard shortcuts' });
  await overview.waitFor();
  check(await overview.getByRole('heading', { name: 'Go to' }).count() === 1 && await overview.getByRole('heading', { name: 'Actions' }).count() === 1,
    'Ctrl+/ opens the shortcuts overview with the groups');
  check((await overview.locator('[data-shortcut=searchChats] kbd').allTextContents()).includes('E'),
    'overview shows the changed shortcut, not only the default');
  await page.keyboard.press('Escape');
  await overview.waitFor({ state: 'detached' });
  check(true, 'Esc closes the shortcuts overview');

  // "Change shortcuts…" opens Settings -> Keyboard  where new shortcut is listed like others
  await page.keyboard.press('Control+/');
  await overview.waitFor();
  await overview.getByRole('button', { name: 'Change shortcuts…' }).click();
  await overview.waitFor({ state: 'detached' });
  await page.getByRole('button', { name: 'Reset all' }).waitFor();
  check(await page.locator('[data-action=shortcuts] .combo').count() === 1, '"Change shortcuts…" opens Settings → Keyboard, with the overview shortcut listed');
  await page.getByRole('button', { name: 'Reset all' }).click();
  check(await page.locator('[data-action=editLast] .combo').count() === 1 &&
    await page.locator('[data-action=searchChats] .combo').count() === 1, 'reset all restores the defaults');
  await shot('settings-keyboard');
  await page.getByRole('button', { name: 'General' }).click();
  await page.getByLabel('Text size preset').selectOption('110');
  check(await page.evaluate(() => document.getElementById('app')?.style.zoom) === '1.1',
    'text size setting scales the interface');
  check(await page.locator('.text-size-notice', { hasText: 'Text size: 110 %' }).count() === 1,
    'text size change shows its current value');
  check(await page.evaluate(() => JSON.parse(localStorage.getItem('braids.settings.v1')).textSize === 110),
    'text size is remembered');
  await page.getByLabel('Text size preset').selectOption('100');
  const zoomShortcut = (key, code = key) => page.evaluate(([k, c]) => {
    window.dispatchEvent(new KeyboardEvent('keydown', { key: k, code: c, ctrlKey: true, bubbles: true, cancelable: true }));
  }, [key, code]);
  await zoomShortcut('+', 'Equal');
  check(await page.evaluate(() => document.getElementById('app')?.style.zoom) === '1.1',
    'Ctrl+Plus increases text size');
  check(await page.locator('.text-size-notice', { hasText: 'Text size: 110 %' }).count() === 1,
    'Ctrl+Plus shows the current value');
  await zoomShortcut('-', 'Minus');
  check(await page.evaluate(() => document.getElementById('app')?.style.zoom) === '1',
    'Ctrl+Minus decreases text size');
  await zoomShortcut('0', 'Digit0');
  check(await page.evaluate(() => document.getElementById('app')?.style.zoom) === '1',
    'Ctrl+0 resets text size');
  await page.locator('.text-size-notice').waitFor({ state: 'detached', timeout: 6000 });
  check(true, 'text size notice disappears after five seconds');
  await page.getByRole('button', { name: 'Dark' }).click();
  check(await page.evaluate(() => document.documentElement.classList.contains('t-dark')), 'dark theme applied');
  await shot('settings-dark');

  // ---- new in 0.2 ----
  await page.keyboard.press('Escape');
  const sendFile = (name, b64OrMaker) => page.evaluate(async ([n, data]) => {
    let blob;
    if (data === 'webp') {
      const c = Object.assign(document.createElement('canvas'), { width: 96, height: 96 });
      const g = c.getContext('2d');
      g.fillStyle = '#e58a55';
      g.beginPath(); g.arc(48, 48, 40, 0, Math.PI * 2); g.fill();
      blob = await new Promise((r) => c.toBlob(r, 'image/webp'));
    } else {
      blob = new Blob([Uint8Array.from(atob(data), (ch) => ch.charCodeAt(0))]);
    }
    const up = await (await fetch(`/__braids/upload?name=${n}`, { method: 'POST', body: blob })).json();
    const accounts = await (await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method: 'accounts.list' }) })).json();
    await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method: 'messages.send', params: { accountId: accounts.result[0].accountId, chatId: 'Michael_0', filePath: up.path } }) });
  }, [name, b64OrMaker]);
  await page.getByRole('option', { name: /Michael/ }).click();

  // stickers without a bubble, GIFs marked
  await sendFile('smile.webp', 'webp');
  await page.locator('.bubble.sticker .sticker img').last().waitFor();
  check(await page.locator('.bubble.sticker').last().evaluate((el) => getComputedStyle(el).backgroundColor === 'rgba(0, 0, 0, 0)'), 'sticker shown without a bubble');
  await sendFile('dance.gif', 'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==');
  await page.locator('.msg .gif-badge').last().waitFor();
  check(true, 'GIF shown with a GIF mark');

  // Ctrl+C: the selection, or the message under the mouse
  await composer.fill('copy me please');
  await page.keyboard.press('Enter');
  const copyMsg = page.locator('.msg.own', { hasText: 'copy me please' }).last();
  await copyMsg.waitFor();
  await copyMsg.locator('.msg-text').evaluate((el) => {
    const r = document.createRange();
    r.setStart(el.firstChild, 0);
    r.setEnd(el.firstChild, 7);
    getSelection().removeAllRanges();
    getSelection().addRange(r);
  });
  await page.locator('.messages').focus();
  await page.keyboard.press('Control+c');
  await page.waitForFunction(async () => (await navigator.clipboard.readText()) === 'copy me');
  check(true, 'Ctrl+C copies the selected text');
  await page.evaluate(() => getSelection().removeAllRanges());
  await copyMsg.hover();
  await page.keyboard.press('Control+c');
  await page.locator('.toast', { hasText: 'Message copied' }).waitFor();
  check(await page.evaluate(() => navigator.clipboard.readText()) === 'copy me please', 'Ctrl+C without a selection copies the hovered message');

  // multi-select: Select from the menu, click to add, copy, forward, delete
  for (const t of ['pick one', 'pick two']) {
    await composer.fill(t);
    await page.keyboard.press('Enter');
    await page.locator('.msg.own:not(.pending)', { hasText: t }).last().waitFor();
  }
  const pickOne = page.locator('.msg.own', { hasText: 'pick one' }).last();
  const pickTwo = page.locator('.msg.own', { hasText: 'pick two' }).last();
  await pickOne.click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Select' }).click();
  const selBar = page.getByRole('toolbar', { name: 'Selected messages' });
  await selBar.waitFor();
  check((await selBar.textContent()).includes('1 selected') && await page.locator('textarea[aria-label=Message]').count() === 0,
    'Select: the selection bar replaces the composer');
  await pickTwo.locator('.msg-text').click();
  check((await selBar.textContent()).includes('2 selected') && await pickTwo.evaluate((el) => el.classList.contains('selected')),
    'a click adds a message to the selection');
  await page.evaluate(() => navigator.clipboard.writeText(''));
  await selBar.getByRole('button', { name: 'Copy' }).click();
  await page.waitForFunction(async () => (await navigator.clipboard.readText()) === 'pick one\npick two');
  check(true, 'Copy copies the selected messages in order');
  await selBar.getByRole('button', { name: 'Forward' }).click();
  const fwd = page.getByRole('dialog', { name: 'Forward 2 messages' });
  await fwd.waitFor();
  await fwd.getByPlaceholder('Search chats').fill('Michael');
  await fwd.locator('.target').first().click();
  await page.locator('.toast', { hasText: 'Forwarded 2 messages' }).waitFor();
  check(await selBar.count() === 0, 'Forward sends the selected messages and ends the selection');
  await pickOne.click({ modifiers: ['Control'] });
  await selBar.waitFor();
  check(true, 'Ctrl+click starts a selection');
  await page.keyboard.press('Escape');
  await selBar.waitFor({ state: 'detached' });
  check(await page.locator('.msg.selected').count() === 0, 'Esc ends the selection');
  await pickOne.click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Select' }).click();
  await pickTwo.locator('.msg-text').click({ modifiers: ['Shift'] });
  const twoBefore = await page.locator('.msg', { hasText: 'pick two' }).count();
  const oneBefore = await page.locator('.msg', { hasText: 'pick one' }).count();
  await selBar.getByRole('button', { name: 'Delete' }).click();
  await page.getByRole('button', { name: 'Delete for everyone' }).click();
  // (the forwarded copies are in this chat too: Michael is the open chat)
  await page.waitForFunction(([one, two]) => [...document.querySelectorAll('.msg')].filter((el) => el.textContent.includes('pick one')).length === one - 1
    && [...document.querySelectorAll('.msg')].filter((el) => el.textContent.includes('pick two')).length === two - 1, [oneBefore, twoBefore]);
  check(true, 'Delete removes the selected own messages');

  // drag and drop a file into the chat
  await page.evaluate(() => {
    const dt = new DataTransfer();
    dt.items.add(new File(['hello'], 'notes.txt', { type: 'text/plain' }));
    window.dispatchEvent(new DragEvent('dragover', { dataTransfer: dt, bubbles: true, cancelable: true }));
    window.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
  });
  await page.locator('.composer', { hasText: 'notes.txt' }).waitFor();
  check(true, 'dropped file attached to the message');
  await page.locator('.composer [aria-label^="Remove"]').first().click();

  // emoji picker: search and pick
  await page.getByRole('button', { name: 'Emoji', exact: true }).click();
  await page.fill('input[aria-label="Search emoji"]', 'rocket');
  await page.locator('.picker .em').first().waitFor();
  await page.keyboard.press('Enter');
  check((await composer.inputValue()).includes('🚀'), 'emoji picker search inserts the emoji');
  await composer.fill('');

  // outbox: a message sent while offline waits (greyed, clock), then goes out
  await composer.fill('/offline 3');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(300);
  await composer.fill('see you after the outage');
  await page.keyboard.press('Enter');
  const waitingMsg = page.locator('.msg.pending', { hasText: 'see you after the outage' });
  await waitingMsg.waitFor();
  check(await waitingMsg.getByLabel('Waiting for the connection').count() === 1, 'offline: the message stays, greyed, waiting');
  await waitingMsg.waitFor({ state: 'detached', timeout: 15000 });
  await page.locator('.msg.own:not(.pending)', { hasText: 'see you after the outage' }).waitFor();
  check(true, 'back online: the message is sent');
  // refused: "Not sent" with Send again and Delete
  await composer.fill('/fail this one');
  await page.keyboard.press('Enter');
  const failedMsg = page.locator('.msg.failed', { hasText: '/fail this one' });
  await failedMsg.waitFor();
  check(await failedMsg.locator('.not-sent', { hasText: 'Not sent' }).count() === 1, 'a refused message is marked Not sent');
  await failedMsg.getByRole('button', { name: 'Send again' }).click();
  await failedMsg.locator('.not-sent').waitFor();
  check(true, 'send again (refused again)');
  await failedMsg.getByRole('button', { name: 'Delete' }).click();
  await failedMsg.waitFor({ state: 'detached' });
  check(true, 'a failed message can be deleted');

  // link preview card
  await page.route('**/__braids/preview?*', (route) => route.fulfill({ json: { url: 'https://example.com/paper', title: 'Paper sales up 12%', description: 'Scranton branch leads the quarter.', siteName: 'Example News', image: '' } }));
  await composer.fill('read this https://example.com/paper');
  await page.keyboard.press('Enter');
  await page.locator('.link-preview', { hasText: 'Paper sales up 12%' }).waitFor();
  check(await page.locator('.link-preview .lp-site').last().textContent() === 'Example News', 'link preview card shown');

  // pinned messages bar
  await copyMsg.click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Pin' }).click();
  await page.locator('.pin-bar', { hasText: 'copy me please' }).waitFor();
  check(true, 'pinned message shown in the bar');
  await page.locator('.pin-bar [aria-label=Unpin]').click();
  await page.locator('.pin-bar').waitFor({ state: 'detached' });
  check(true, 'unpinned message leaves the bar');

  // search through the messages of all chats
  await page.keyboard.press('Control+Space');
  await page.keyboard.type('copy me');
  await page.locator('.found .hit', { hasText: 'copy me please' }).first().waitFor();
  check(await page.locator('.found .hit mark').first().textContent() === 'copy me', 'message search finds and marks the text');
  await page.getByRole('option', { name: /The Office/ }).click().catch(() => {});
  await page.locator('input[aria-label="Search chats"]').fill('copy me');
  await page.locator('.found .hit', { hasText: 'copy me please' }).first().click();
  await page.locator('.msg.highlight', { hasText: 'copy me please' }).waitFor();
  check(true, 'a search result opens the chat at the message');
  await page.locator('input[aria-label="Search chats"]').fill('');
  await page.fill('input[aria-label="Search chats"]', 'no such chat');
  await page.keyboard.press('Control+N');
  check(await page.inputValue('input[aria-label="Search chats"]') === '' && (await page.getByRole('option').count()) >= 2,
    'next unread (Ctrl+N) clears the search');
  // no unread chat left: Ctrl+N closes the chat ("pick a chat")
  for (let n = 0; n < 30 && await page.locator('.conv').count(); n++) {
    await page.keyboard.press('Control+N');
    await page.waitForTimeout(150);
  }
  check(await page.locator('.conv').count() === 0 && await page.locator('.no-chat').count() === 1 &&
    await page.locator('[role=option][aria-selected=true]').count() === 0, 'Ctrl+N without unread chats closes the chat');

  // narrow window: the list or the chat; Alt+H back to the list, Enter opens
  await page.setViewportSize({ width: 800, height: 800 });
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.locator('.conv').waitFor();
  check(await page.locator('.chatlist').count() === 0, 'narrow: the chat replaces the list');
  await page.keyboard.press('Alt+h');
  await page.locator('.chatlist .row.cursor', { hasText: 'The Office' }).waitFor();
  check(await page.locator('.conv').count() === 0, 'narrow: Alt+H shows the list, the chat highlighted');
  await page.keyboard.press('Alt+j');
  const narrowTarget = await page.locator('.chatlist .row.cursor .name').textContent();
  check(narrowTarget !== 'The Office' && await page.locator('.conv').count() === 0, 'narrow: Alt+J highlights in the list');
  await page.keyboard.press('Enter');
  await page.locator('.conv header', { hasText: narrowTarget }).waitFor();
  check(true, 'narrow: Enter opens the highlighted chat');
  await page.keyboard.press('Alt+j');
  await page.locator('.chatlist .row.cursor').waitFor();
  check(await page.locator('.conv').count() === 0, 'narrow: Alt+J from a chat goes back to the list');
  await page.keyboard.press('Control+a');
  check(await page.locator('.chatlist').count() === 1, 'narrow: folder shortcuts show the list');
  await page.setViewportSize({ width: 1440, height: 900 });

  // favorites pinned at the top instead of their own folder
  await page.keyboard.press('Control+,');
  await page.getByRole('group', { name: 'Favorites' }).getByRole('button', { name: 'On top' }).click();
  await page.keyboard.press('Escape');
  check(await page.locator('.sidebar .item', { hasText: 'Favorites' }).count() === 0, 'no Favorites folder with favorites on top');
  const firstName = await page.locator('.chatlist .row .name').first().textContent();
  check(await page.evaluate((n) => {
    const s = window.__braids.store;
    const c = s.chatList[0];
    return s.chatName(c) === n && s.category(c) === 'favorite';
  }, firstName), 'a favorite is first in the chat list');
  await page.keyboard.press('Control+,');
  await page.getByRole('group', { name: 'Favorites' }).getByRole('button', { name: 'Folder' }).click();
  await page.keyboard.press('Escape');
  await page.locator('.sidebar .item', { hasText: 'Favorites' }).waitFor();
  check(true, 'Favorites folder back');

  // rename an account in the sidebar (double-click)
  await page.locator('.sidebar .item .label').filter({ hasText: /Dummy/ }).first().dblclick();
  await page.fill('input[aria-label="Account name"]', 'OCS');
  await page.keyboard.press('Enter');
  await page.locator('.sidebar .item .label', { hasText: 'OCS' }).waitFor();
  check(true, 'account renamed in the sidebar');

  // sidebar collapse and expand buttons
  await page.getByRole('button', { name: 'Collapse sidebar' }).click();
  await page.locator('.sidebar.collapsed').waitFor();
  await page.getByRole('button', { name: 'Expand sidebar' }).click();
  await page.locator('.sidebar:not(.collapsed)').waitFor();
  check(true, 'sidebar collapses and expands with its buttons');

  // drag the divider to widen the chat list
  const listW = async () => (await page.locator('.chatlist').boundingBox()).width;
  const widthBefore = await listW();
  const sp = await page.locator('.splitter').boundingBox();
  await page.mouse.move(sp.x + sp.width / 2, sp.y + 200);
  await page.mouse.down();
  await page.mouse.move(sp.x + sp.width / 2 + 100, sp.y + 200, { steps: 5 });
  await page.mouse.up();
  check(Math.abs((await listW()) - widthBefore - 100) <= 2, 'chat list resized at the divider');
  await page.locator('.splitter').dblclick();

  // new group
  await page.getByRole('button', { name: 'New chat' }).click();
  await page.getByRole('tab', { name: 'New group' }).click();
  await page.fill('input[aria-label="Group name"]', 'Road trip');
  await page.getByRole('checkbox', { name: /Jim/ }).click();
  await page.getByRole('checkbox', { name: /Pam/ }).click();
  await page.getByRole('button', { name: 'Create group' }).click();
  await page.locator('main.conv header', { hasText: 'Road trip' }).waitFor();
  check(await page.getByRole('option', { name: /Road trip/ }).count() === 1, 'new group created and opened');
  await shot('new-group');

  // polls: made in a dialog, voted on in the message, others' votes shown
  await page.getByRole('button', { name: 'New poll' }).click();
  const pollDialog = page.getByRole('dialog', { name: 'New poll' });
  await pollDialog.getByLabel('Question').fill('Where to?');
  await pollDialog.getByLabel('Option 1', { exact: true }).fill('Beach');
  await pollDialog.getByLabel('Option 2', { exact: true }).fill('Mountains');
  check(await pollDialog.getByLabel('Option 3', { exact: true }).count() === 1, 'poll dialog: a new option appears while typing');
  await pollDialog.getByRole('button', { name: 'Send poll' }).click();
  await pollDialog.waitFor({ state: 'detached' });
  const pollCard = page.locator('.msg.own .poll', { hasText: 'Where to?' });
  await pollCard.waitFor();
  check(await pollCard.locator('.option').count() === 2, 'poll sent and shown with its options');
  await pollCard.locator('.option', { hasText: 'Beach' }).click();
  await pollCard.locator('.option[aria-pressed=true]', { hasText: 'Beach' }).waitFor();
  check((await pollCard.locator('.summary').textContent()).includes('1 vote'), 'own vote shown');
  check((await page.getByRole('option', { name: /Road trip/ }).textContent()).includes('📊 Where to?'), 'poll in the chat list preview');
  const pollComposer = page.getByRole('textbox', { name: 'Message' });
  await pollComposer.fill('/poll');
  await pollComposer.press('Enter');
  const lunch = page.locator('.msg:not(.own) .poll', { hasText: 'Lunch?' });
  await lunch.waitFor();
  const sushi = lunch.locator('.option', { hasText: 'Sushi' });
  check(await sushi.locator('.votes').textContent() === '1', "others' votes shown as a count");
  check(await sushi.locator('.faces .avatar').count() === 1, "the voter's picture next to the count");
  check(await sushi.locator('.bar').evaluate((e) => e.style.width) === '100%', 'the leading option has a full bar');
  await lunch.locator('.option', { hasText: 'Pizza' }).click();
  await lunch.locator('.option[aria-pressed=true]', { hasText: 'Pizza' }).waitFor();
  check(true, "voting in someone else's poll");
  check(await lunch.locator('.option', { hasText: 'Pizza' }).locator('.face').first().getAttribute('title') === 'Stanley',
    'own vote shown with your name and picture, not "You"');
  check(await lunch.locator('.option', { hasText: 'Salad' }).locator('.bar').evaluate((e) => e.style.width) === '0%',
    'an option without votes has no bar');
  await shot('poll');
  await page.getByRole('button', { name: 'Settings' }).click();
  await page.locator('[aria-labelledby=newchat-title]').waitFor({ state: 'detached' }).catch(() => {});
  await page.keyboard.press('Control+,');

  // remove account
  await page.getByRole('button', { name: 'Accounts' }).click();
  await page.locator('.acct-row').first().click();
  await page.getByRole('button', { name: 'Remove account…' }).click();
  await page.getByRole('button', { name: 'Remove account', exact: true }).click();
  await page.getByText('Welcome to braids').waitFor({ timeout: 30000 });
  check(true, 'account removed, back to welcome');

  // responsive: single pane below 900 px
  await page.setViewportSize({ width: 800, height: 800 });
  check(true, 'narrow viewport');

  check(errors.length === 0, `no page errors${errors.length ? `: ${errors.join(' | ')}` : ''}`);
} catch (e) {
  if (!failures) {
    failures++;
    console.log(`not ok - ${e.message}`);
  }
} finally {
  await browser.close();
  await server.close();
  daemon.kill('SIGTERM');
  await new Promise((r) => daemon.once('exit', r));
  fs.rmSync(confdir, { recursive: true, force: true });
  fs.rmSync(path.join(confdir, '..', `braids-ui-${process.pid}.png`), { force: true });
  fs.rmSync(path.join(confdir, '..', `braids-ui-${process.pid}.wav`), { force: true });
}

console.log(failures ? `${failures} failed` : 'all tests passed');
process.exit(failures ? 1 : 0);
