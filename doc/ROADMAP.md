braids — Roadmap to 1.0
=======================

Agreed with the maintainer on 2026-11-20. Each milestone is a release.
The milestones M0–M3 in [ARCHITECTURE.md](ARCHITECTURE.md) are done; this
plan replaces M4.

What 1.0 means
--------------

braids 1.0 is ready for **any Linux desktop user with several messengers**,
not only power users: installing, the first run and error messages work
without reading docs. From 0.3.x on, updates never require logging in again
and never lose settings, chats or unsent messages.

Linux only (x86_64 and aarch64). Every messenger covers
[REQUIREMENTS.md](REQUIREMENTS.md) section 2 wherever the messenger itself
supports the feature; what it cannot do is explained on the help page. No
messenger ships as "beta".

Milestones
----------

| Version | Theme | Contents |
| --- | --- | --- |
| **0.3.7** | Fixes | New-message count on the "scroll to latest" button (#6, PR #50); GUI shows the wrong core version on Nix (#53); keyboard: Ctrl+J/K and Ctrl+↑/↓ move the chat list highlight (was Alt), Alt+J/K and Alt+↑/↓ scroll the open chat up and down |
| **0.4** | First run & trust | Welcome flow on first start: choose messengers → log in to each → "loading your chats" → chat list. One-time WhatsApp notice when adding an account (unofficial client, ban risk), confirmed by the user. Matrix: device verification by comparing emojis with another signed-in device, "Skip" / "Don't ask again" buttons instead of typing `never`, a plain "these messages can't be unlocked" state. Help/FAQ page on the website, linked from Settings and from error messages. Calls: "Anna called you · answer on your phone" in the chat and as a notification (#51) |
| **0.5** | Messenger parity | Add/remove group members (Telegram, WhatsApp, Signal); pin messages (WhatsApp, Signal, Matrix); pin chats (Signal); archive chats (Signal, Matrix via low-priority tag); join Signal groups by invite link (riskiest: no signalmeow API, start first); sync mute and "mark unread" with the messengers; reply privately to a group message (#52) |
| **0.6** | Polish | Tips: one tip at a time over the first days, all of them in Settings → Tips. UI text prepared for translation (English only in 1.0). Accessibility floor: full keyboard use, visible focus, contrast in both themes, labels on icon buttons |
| **0.7** | Organize | Custom folders and custom muting rules (design session first) |
| **0.8** | Security | Review of the local socket, link previews, received files and media, the Tauri settings (CSP, allowed commands), secrets on disk, logs, dependencies. Findings become issues, all fixed before the RC |
| **0.9** | Contacts | One contact across messengers (#35, design session first) |
| **1.0-rc** | Freeze | Fixes only, two weeks. Automated upgrade test from a 0.3.x profile (no new login, settings and outbox kept). Flathub submission |
| **1.0** | | First stable release |

Not in 1.0
----------

- Vim mode (dropped; keyboard shortcuts cover navigation).
- Voice and video calls (only the "called you" notice).
- Web version, mobile apps, hosted service.
- Translations (strings are prepared), full screen-reader support.
- macOS, Windows.
- In-app updater: Flathub, apt and Nix update braids; other installs show
  how to update.
