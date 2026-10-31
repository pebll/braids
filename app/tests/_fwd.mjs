import { spawn } from 'node:child_process';
import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path';
import { chromium } from 'playwright';
const confdir = fs.mkdtempSync(path.join(os.tmpdir(), 'braids-fwd-'));
const daemon = spawn('../build/bin/braidsd', ['--confdir', confdir], { stdio: 'ignore' });
process.env.BRAIDS_CONFDIR = confdir;
const { createServer } = await import('vite');
const server = await createServer({ root: process.cwd(), configFile: 'vite.config.ts', server: { port: 1424 }, logLevel: 'warn' });
await server.listen();
const browser = await chromium.launch();
try {
  const page = await browser.newPage({ viewport: { width: 1200, height: 800 } });
  page.on('console', (m) => m.type() === 'error' && console.log('console', m.text()));
  await page.goto(server.resolvedUrls.local[0]);
  await page.getByRole('button', { name: /Dummy/ }).click();
  await page.fill('#auth-input', '+46 700 1234'); await page.keyboard.press('Enter');
  await page.waitForFunction(() => document.querySelector('label[for=auth-input]')?.textContent === 'Verification code');
  await page.fill('#auth-input', '1234'); await page.keyboard.press('Enter');
  await page.getByText('Done — go to inbox').click();
  const png = path.join(confdir, '..', `fwd-${process.pid}.png`);
  fs.writeFileSync(png, Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64'));
  const call = (method, params) => page.evaluate(async ([method, params]) => (await (await fetch('/__braids/call', { method: 'POST', body: JSON.stringify({ method, params }) })).json()), [method, params]);
  const acc = (await call('accounts.list', {})).result[0].accountId;
  await call('messages.send', { accountId: acc, chatId: 'Michael_0', filePath: png });
  await call('messages.send', { accountId: acc, chatId: 'Michael_0', filePath: png, text: 'caption here' });
  await page.locator('.row', { has: page.locator('.name', { hasText: /^Michael$/ }) }).click();
  await page.waitForTimeout(1500);
  const msgs = (await call('messages.get', { accountId: acc, chatId: 'Michael_0', limit: 3 })).result.messages;
  console.log('source', JSON.stringify(msgs, null, 0).slice(0, 1500));
  for (const sel of ['.msg.own .image', '.msg.own:has-text("caption here")']) {
    const m = page.locator(sel).last().locator('xpath=ancestor-or-self::div[contains(@class,"msg")]').first();
    await m.click({ button: 'right' });
    await page.getByRole('menuitem', { name: 'Forward…' }).click();
    await page.fill('input[aria-label="Search chats to forward to"]', 'Pam');
    await page.locator('.target', { hasText: 'Pam' }).first().click();
    await page.waitForTimeout(1500);
    console.log('toasts', await page.locator('.toast').allTextContents());
  }
  const pam = (await call('messages.get', { accountId: acc, chatId: 'Pam_0', limit: 3 })).result.messages;
  console.log('pam', JSON.stringify(pam, null, 0).slice(0, 2000));
} catch (e) { console.log('ERR', e.message.slice(0, 500)); }
await browser.close(); daemon.kill(); process.exit(0);
