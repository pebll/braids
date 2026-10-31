// site-screenshots.mjs
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// Screenshots for the website (site/img/): the app with the Dummy protocol in
// Chromium, light and dark. Usage (from app/, braidsd built with Dummy):
//   node scripts/site-screenshots.mjs [--braidsd PATH]

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');
const appDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const opt = (name, def) => (args.includes(name) ? args[args.indexOf(name) + 1] : def);
const braidsd = path.resolve(opt('--braidsd', path.join(appDir, '../build/bin/braidsd')));
const out = path.join(appDir, '../site/img');
fs.mkdirSync(out, { recursive: true });

async function session(theme, shots) {
  const confdir = fs.mkdtempSync(path.join(os.tmpdir(), 'braids-site-'));
  const daemon = spawn(braidsd, ['--confdir', confdir], { stdio: 'ignore' });
  process.env.BRAIDS_CONFDIR = confdir;
  const { createServer } = await import('vite');
  const server = await createServer({ root: appDir, configFile: path.join(appDir, 'vite.config.ts'), server: { port: 1430, strictPort: false }, logLevel: 'warn' });
  await server.listen();
  const browser = await chromium.launch();
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 }, deviceScaleFactor: 2 });
    await page.addInitScript((t) => {
      localStorage.setItem('braids.settings.v1', JSON.stringify({ theme: t }));
    }, theme);
    await page.goto(server.resolvedUrls.local[0]);
    await page.getByRole('button', { name: /Dummy/ }).click();
    await page.fill('#auth-input', '+46 700 1234');
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => document.querySelector('label[for=auth-input]')?.textContent === 'Verification code');
    await page.fill('#auth-input', '1234');
    await page.keyboard.press('Enter');
    await page.getByText('Done — go to inbox').click();
    // a favorite, for the gold ring
    await page.locator('.row', { has: page.locator('.name', { hasText: /^Pam$/ }) }).click({ button: 'right' });
    await page.getByRole('menuitem', { name: 'Add to favorites' }).click();
    await page.getByRole('option', { name: /The Office/ }).click();
    await page.locator('[data-msg-id]').first().waitFor();
    await page.waitForTimeout(800);
    // day labels at their place in the history (not floating over a message
    // as while scrolling)
    await page.addStyleTag({ content: '.day { position: static !important; }' });
    await page.locator('.messages').evaluate((el) => { el.scrollTop = el.scrollHeight; });
    await shots(page);
  } finally {
    await browser.close();
    await server.close();
    daemon.kill();
  }
}

const save = (page, name) => page.screenshot({ path: path.join(out, `${name}.png`) });

// a close-up: the box of the given elements, with a margin, inside the window
async function saveClip(page, name, selectors, margin = 24) {
  const boxes = [];
  for (const sel of selectors) boxes.push(await page.locator(sel).first().boundingBox());
  const vp = page.viewportSize();
  const x = Math.max(0, Math.min(...boxes.map((b) => b.x)) - margin);
  const y = Math.max(0, Math.min(...boxes.map((b) => b.y)) - margin);
  const right = Math.min(vp.width, Math.max(...boxes.map((b) => b.x + b.width)) + margin);
  const bottom = Math.min(vp.height, Math.max(...boxes.map((b) => b.y + b.height)) + margin);
  await page.screenshot({ path: path.join(out, `${name}.png`), clip: { x, y, width: right - x, height: bottom - y } });
}

await session('light', async (page) => {
  await page.keyboard.press('Control+i');
  await page.locator('.info .description').waitFor();
  await page.waitForTimeout(500);
  await save(page, 'inbox-light');
  await page.keyboard.press('Control+i');
  // mention suggestions
  const composer = page.locator('textarea[aria-label=Message]');
  await composer.pressSequentially('nice one @');
  await page.locator('.mentions [role=option]').first().waitFor();
  await saveClip(page, 'mentions-light', ['.mentions', '.composer'], 12);
  await composer.fill('');
  // join a group
  await page.getByRole('button', { name: 'New chat' }).click();
  await page.getByRole('tab', { name: 'Join a group' }).click();
  await page.fill('input[aria-label="Invite link or address"]', 'https://t.me/+braids');
  await saveClip(page, 'join-light', ['[role=dialog][aria-labelledby=newchat-title]'], 0);
  await page.keyboard.press('Escape');
  // keyboard shortcuts
  await page.keyboard.press('Control+,');
  await page.getByRole('button', { name: 'Keyboard' }).click();
  await page.waitForTimeout(300);
  await saveClip(page, 'keyboard-light', ['[role=dialog]'], 0);
});

await session('dark', async (page) => {
  await page.locator('.msg:not(.own)', { hasText: 'storm your village' }).hover();
  await page.waitForTimeout(300);
  await save(page, 'inbox-dark');
});

console.log(`screenshots in ${out}`);
process.exit(0);
