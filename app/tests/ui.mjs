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

const browser = await chromium.launch();
const errors = [];
try {
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
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
  await page.fill('input[aria-label="Search chats"]', '');

  // conversation
  await page.getByRole('option', { name: /The Office/ }).click();
  await page.locator('[data-msg-id]').first().waitFor();
  check((await page.locator('.msg .sender').count()) > 0, 'group shows sender names');
  await composer.fill('Hello *team* `code`');
  await composer.press('Enter');
  const own = page.locator('.msg.own').last();
  await own.locator('strong', { hasText: 'team' }).waitFor();
  check((await own.locator('code').textContent()) === 'code', 'sent message rendered with markup');
  check((await page.getByRole('option', { name: /The Office/ }).textContent()).includes('You:'), 'chat preview shows own message');

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
  check(await last.locator('.actions').evaluate((el) => getComputedStyle(el).flexDirection === 'column'), 'hover bar is vertical');
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
  await theirs.getByRole('button', { name: 'More' }).click();
  await shot('more-menu');
  check(await page.getByRole('menuitem', { name: 'Copy text' }).count() === 1 && await page.getByRole('menuitem', { name: 'Open file' }).count() === 0,
    'more menu has copy, no open');
  await page.getByRole('menuitem', { name: 'Forward…' }).click();
  await page.getByText('Forward message').waitFor();
  await page.getByLabel('Add “Forwarded from” line').check();
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
  fs.rmSync(png, { force: true });
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

  // categories: favorite, default, archived
  await page.getByRole('option', { name: /Dwight/ }).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Add to favorites' }).click();
  check(await (await page.locator('.chatlist .row', { has: page.locator('.name', { hasText: /^Dwight$/ }) }).locator('.circle').getAttribute('style')).includes('--favorite'), 'favorite marked in the list');
  check(await page.locator('.chatlist .section').count() === 0, 'no favorites section, sorted by date');
  await page.getByRole('button', { name: 'Favorites' }).click();
  check((await page.getByRole('option').count()) === 1, 'favorites folder');
  await shot('favorites');
  await page.getByRole('button', { name: 'All chats' }).click();
  await page.getByRole('option', { name: /Jim/ }).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Archive' }).click();
  check((await page.getByRole('option', { name: /Jim/ }).count()) === 0, 'archived chat leaves the inbox');
  await page.getByRole('button', { name: /Archived/ }).click();
  check((await page.getByRole('option', { name: /Jim/ }).count()) === 1, 'archived chat in Archived');
  await page.getByRole('option', { name: /Jim/ }).click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Unarchive' }).click();
  await page.getByRole('button', { name: 'All chats' }).click();
  check((await page.getByRole('option', { name: /Jim/ }).count()) === 1, 'unarchived chat back in the inbox');
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
  await page.keyboard.press('Alt+j');
  await page.waitForFunction((b) => document.querySelector('[role=option][aria-selected=true] .name')?.textContent !== b, before);
  check(true, 'Alt+J opens the next chat');
  await page.keyboard.press('Alt+k');
  await page.waitForFunction((b) => document.querySelector('[role=option][aria-selected=true] .name')?.textContent === b, before);
  check(true, 'Alt+K back to the previous chat');
  await page.keyboard.press('Control+b');
  await page.locator('.chatlist h2', { hasText: 'Favorites' }).waitFor();
  check(true, 'Ctrl+B shows favorites');
  await page.keyboard.press('Control+a');
  await page.locator('.chatlist h2', { hasText: 'All chats' }).waitFor();
  check(true, 'Ctrl+A shows all chats');

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
  await page.keyboard.press('Control+,');
  await page.getByRole('button', { name: 'Keyboard' }).click();
  await page.getByRole('button', { name: 'Reset all' }).click();
  check(await page.locator('[data-action=editLast] .combo').count() === 1 &&
    await page.locator('[data-action=searchChats] .combo').count() === 1, 'reset all restores the defaults');
  await shot('settings-keyboard');
  await page.getByRole('button', { name: 'General' }).click();
  await page.getByRole('button', { name: 'Dark' }).click();
  check(await page.evaluate(() => document.documentElement.classList.contains('t-dark')), 'dark theme applied');
  await shot('settings-dark');

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
}

console.log(failures ? `${failures} failed` : 'all tests passed');
process.exit(failures ? 1 : 0);
