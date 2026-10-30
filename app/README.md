braids app
==========

The desktop app: a Tauri 2 shell (`src-tauri/`, Rust) around a Svelte 5 +
TypeScript web UI (`src/`). All messenger logic lives in braidsd; the app
talks to it over the Unix socket API ([doc/API.md](../doc/API.md)). The
visual design follows [doc/design](../doc/design/README.md).

Run
---

Needs Node 22, Rust, and on Linux the WebKitGTK development packages
(`libwebkit2gtk-4.1-dev librsvg2-dev libayatana-appindicator3-dev`), plus a
built braidsd (`./make.sh build` in the repository root).

    cd app
    npm install
    npm run tauri dev          # app window, starts braidsd if needed

The app connects to the socket of a running braidsd, or starts one: `BRAIDSD`
(path), then `braidsd` next to the app binary, then `../build/bin/braidsd`
(development), then `PATH`. braidsd runs in its own process group and keeps
running when the app quits. Environment:

| Variable | Effect |
|---|---|
| `BRAIDS_CONFDIR` | config dir for braidsd (`--confdir`), socket `<dir>/braidsd.sock` |
| `BRAIDS_SOCKET` | socket path (`--socket`) |
| `BRAIDSD` | braidsd binary to start |

Without the Tauri shell, the UI also runs in a normal browser against a running
braidsd, through a dev bridge in the Vite server (`dev/bridge.ts`):

    ../build/bin/braidsd --confdir /tmp/braidsconf &
    BRAIDS_CONFDIR=/tmp/braidsconf npm run dev     # http://localhost:1420

In development builds the Dummy test protocol is offered in "Add account"
(in release builds: set `localStorage['braids.dev'] = '1'`).

Check and test
--------------

    npm run check              # svelte-check (TypeScript)
    npm run build              # frontend to dist/
    npm run test:ui            # end-to-end: braidsd + UI in Chromium (Playwright)
    (cd src-tauri && cargo build)

`test:ui` starts its own braidsd (Dummy protocol, temporary config dir) and
Vite server; `--braidsd PATH` and `--screenshots DIR` are optional.

Layout
------

- `src/lib/api.ts` — transport (Tauri `invoke`/events or the dev bridge) and
  platform services (files, notifications, opening paths and links, badge).
- `src/lib/store.svelte.ts` — client state: mirror of accounts, chats and the
  loaded messages, kept current by API events; login setups; toasts.
- `src/lib/settings.svelte.ts` — GUI settings (localStorage): theme,
  notifications, per-account name / color / notifications.
- `src/lib/format.ts` — message markup to safe HTML, times, initials, colors.
- `src/components/` — `Sidebar`, `ChatList`, `Conversation` (+ `MessageItem`,
  `Composer`), `InfoPanel`, `Wizard` / `AuthStep` (login flows), `Settings`,
  `Welcome`, `NewChat`, `Overlays` (menu, confirm, lightbox, toasts).
- `src/styles/tokens.css` — copy of `doc/design/tokens.css`; `app.css` global
  styles. Plain CSS that works in WebKitGTK and WKWebView.
- `src-tauri/src/daemon.rs` — socket client: start/reconnect braidsd, requests
  with replies, events to the UI. `lib.rs` — commands, tray, close-to-tray.

Known gaps (v1)
---------------

- Unread state per chat is a dot: protocols report unread state, not counts.
- No mute toggle (not in the API yet); the mute state is shown.
- The message list is not virtualized; history is paged (40 messages per page).
- Vim mode, start at login, packaging and signing are milestone M4.
