braids — Architecture
=====================

Status: draft v0.3 (milestone M2). See [REQUIREMENTS.md](REQUIREMENTS.md) for
scope and [API.md](API.md) for the daemon API.

Overview
--------

```
┌──────────────────────────────────────────────┐
│ braids (Tauri app)                           │
│  web UI (TypeScript)  ◄──►  Rust shell       │
└───────────────────────────────┬──────────────┘
                                │ JSON-RPC 2.0 over Unix socket
┌───────────────────────────────┴──────────────┐
│ braidsd (C++ core daemon)                    │
│  API server · account manager · chat model   │
│  MessageCache (SQLite) · notifications       │
├───────────┬───────────┬───────────┬──────────┤
│ tgchat    │ wmchat    │ sgchat    │ mxchat   │  lib/*chat (from nchat)
│ (tdlib)   │ (Go)      │ (Go)      │ (Go)     │  Protocol interface
└───────────┴───────────┴───────────┴──────────┘
```

- **Protocol libraries** (`lib/tgchat`, `lib/wmchat`, `lib/sgchat`,
  `lib/mxchat`, plus `lib/duchat` dummy): unchanged from nchat except for the
  login flows (see M1). Each implements `Protocol` from
  `lib/common/src/protocol.h`: asynchronous `SendRequest(RequestMessage)` in,
  `ServiceMessage` notifications out via a message handler. Per-protocol
  details are in `lib/mxchat/NOTES.md` (sections 1-4 apply to all protocols).
- **lib/ncutil**: shared utilities from nchat, including `MessageCache`
  (per-account SQLite under `~/.config/braids/history/<accountId>/`),
  `AppConfig`, logging, file utils.
- **braidsd** (`src/braidsd`): the core daemon, see "Core daemon" below. It
  serves its API on a Unix socket (and on stdin/stdout with `--stdio`).
- **braids app** (`app/`, M3): Tauri; the Rust side connects to (and if needed
  starts) braidsd; the web UI renders everything.

Process model
-------------

- braidsd runs as a per-user background process. The app starts it if it is
  not running (Tauri sidecar binary), and it keeps running when the window is
  closed (tray), so notifications and sync continue.
- Single instance per config dir, guarded by the existing per-profile
  `ScopedDirLock` plus a daemon lock file.
- Socket: `$XDG_RUNTIME_DIR/braids/braidsd.sock` (fallback
  `~/.config/braids/braidsd.sock`), mode 0600, owner only.

Core daemon (M1)
----------------

```
 Unix socket clients / stdin+stdout
        │ JSON-RPC (server.cpp: poll loop, per-connection output buffers)
 ApiServer ── Core::Call ──► EventLoop thread ◄── protocol notifications (Post)
                              │                ◄── MessageCache results, Status changes
                              ├─ Core: accounts, setups, dispatcher
                              └─ ChatModel: chats, contacts, messages, events
 setup threads: Protocol::SetupProfile + Login, AuthBridge blocks on input
```

- `core.{h,cpp}`: account manager and API dispatcher. Loads all profiles at
  start (as nchat's `main.cpp` did) and logs them in on a background thread;
  `accounts.add` runs `SetupProfile()` + `Login()` on a setup thread with an
  `AuthBridge`; `accounts.remove` logs out and deletes profile and history.
- `chatmodel.{h,cpp}`: the chat model, ported from nchat's
  `UiModel::Impl::MessageHandler` and helpers (table below). It applies
  `ServiceMessage`s and emits API events; requests become `RequestMessage`s.
  Asynchronous replies (`messages.get`, `members.get`, ...) wait for the
  matching notification, with a 15 s timeout.
- `eventloop.{h,cpp}`: one thread owns all core state; protocol callbacks,
  MessageCache results and API calls are posted to it, so the model needs no
  locks and sees everything in order.
- `authbridge.{h,cpp}`: `AuthHandler` implementation turning auth steps into
  `account.auth` events and waiting for `accounts.authInput`.
- Events of an account that is still being set up are held back until
  `account.added`; chat list previews are fetched with a `GetMessages` of
  limit 1 per chat; `chat.updated` is only sent when the chat's visible data
  changed.

The chat model came from nchat's ncurses UI
(`reference/nchat-ui/src/uimodel.cpp`, `UiModel::Impl`); braidsd took over
its non-UI parts:

| From UiModel::Impl | braidsd component |
|---|---|
| `m_Protocols`, profile loading in `main.cpp` | AccountManager: load/add/remove accounts, start/stop protocols |
| `MessageHandler()` (all `*NotifyType` cases) | ChatModel: applies notifications to state, emits API events |
| `m_ChatInfos`, `m_ChatVec`, sorting | chat list with unread, muted, pinned, archived, last activity |
| `m_ContactInfos` | contacts and display names per account |
| `m_Messages`, `m_MessageVec`, `m_OldestMessageId`, `m_MsgFromIdsRequested`, `RequestMessages()` | message windows per chat, history paging via `GetMessagesRequest` and MessageCache |
| `m_UsersTyping`, `m_UserOnline`, `m_UserTimeSeen` | presence and typing state |
| `m_GroupMembers`, `ParseMentions()` | group members, mention resolution |
| `m_AvailableReactions` | reaction capabilities |
| desktop notify / bell logic | notification events (shown by the app) |

Not carried over: everything about terminal geometry, input buffers
(`m_EntryStr`), dialogs and key handling — that becomes GUI state. Also not
carried over: nchat's per-chat config (force hidden/muted chats), "home fetch
all", terminal bell and desktop notification config (the GUI decides from the
`notification` event).

API
---

JSON-RPC 2.0, one JSON object per line, over the Unix socket (several
clients, events broadcast to all) or stdin/stdout. `server.{h,cpp}` is the
transport: a poll loop on the main thread reads requests and hands them to
`Core::Call`; replies and events are appended to per-connection output
buffers (flushed by the loop), so the core never blocks on a slow client.
The implemented methods, events and transport details are documented in
[API.md](API.md); the list below was the plan.

- `hello` handshake returns `apiVersion` (semver; the major must match).
- **Requests** map 1:1 to `RequestMessage` types, scoped by `accountId`:
  `chats.list`, `messages.get {chatId, fromMsgId, limit}`, `messages.send`,
  `messages.edit`, `messages.delete`, `messages.markRead`, `reactions.send`,
  `files.download`, `typing.send`, `members.get`, `chats.create`,
  `chats.pin`, `chats.archive`, `chats.delete`, `search.messages`, plus
  `accounts.list`, `accounts.add`, `accounts.remove`.
- **Events** (JSON-RPC notifications, after `events.subscribe`) map to
  `ServiceMessage` types, already applied to the core model:
  `chat.updated`, `chat.removed`, `message.added`, `message.updated`,
  `message.removed`, `reactions.updated`, `typing`, `presence`,
  `account.state`, `account.auth` (login flow, below), `notification`.
- Payloads use the unified model (REQUIREMENTS.md section 5); every object
  carries `accountId`. Times are ms since epoch. Files are referenced by local
  path once downloaded.

Login flows as events (M1)
--------------------------

nchat's setup was interactive on the terminal (stdin prompts, QR code printed
or opened in an image viewer, `ProtocolUiControlNotify` to take over the
terminal). In braids, `SetupProfile`/`Login` are event driven:

```
app → accounts.add {protocol}
core → account.auth {step: "qr", data: "<qr payload>"}          (WhatsApp, Signal, Telegram)
core → account.auth {step: "input", field: "phone"|"code"|"password"|"userId"|"homeserver"|"recoveryKey", secret: bool}
app → accounts.authInput {field, value}
core → account.state {state: "online"} | account.auth {step: "error", message}
```

Implementation: `protocol.h` has `AuthPrompt` (step `input|qr|info|error`,
field, message, data, isSecret, isOptional) and `AuthHandler` (blocking
`Input()`, non-blocking `Show()`), set per protocol instance with
`Protocol::SetAuthHandler()`. Protocols call `AuthUtil` (`lib/ncutil`), which
forwards to the handler, or falls back to the terminal when none is set. The
Go protocols call back into C++ (`WmAuthShow`/`WmAuthInput`, `SgAuthShow`).
`SetupProfile()` still runs synchronously, on a braidsd setup thread, so the
protocols' setup code kept its structure. When an auth handler is set,
protocols no longer send `ProtocolUiControlNotify`. Secrets passed through
the API are never logged.

Storage
-------

`~/.config/braids/` (XDG_CONFIG_HOME aware):

- `profiles/<accountId>/` — protocol state (sessions, crypto stores), 0700
- `history/<accountId>/db.sqlite` — MessageCache
- `app.conf`, `log.txt`; GUI settings live in the app (Tauri store)

Build and packaging
-------------------

- C++/Go core: CMake, `./make.sh build` (all protocols ON by default;
  `--no-telegram` etc. for faster local builds).
- App (M3): `app/` with Tauri + Vite + TypeScript; bundles `braidsd` and the
  protocol libraries as sidecar resources.
- Linux: AppImage and Flatpak (+ deb); macOS: dmg.

Milestones
----------

| | Scope | Done when |
|---|---|---|
| **M0** | Repo, rename, licence, nchat UI moved to `reference/`, braidsd skeleton, CI, these docs | CI green; `braidsd --list-protocols` loads all protocols |
| **M1** | braidsd core: AccountManager, ChatModel ported from `UiModel::Impl`, event-based login flows | all four protocols can be added and used through an internal test harness (`braidsd --stdio`, `utils/braids-console`; automated: Dummy and Matrix) |
| **M2** | JSON-RPC API, API docs, `braids-cli` test client, integration tests (local Synapse + `lib/mxchat/test/peer`) | API covers all requests/events; tests run in CI |
| **M3** | Tauri GUI v1 (REQUIREMENTS.md section 2) | daily-driver usable for the maintainer |
| **M4** | Notifications, settings incl. vim mode, packaging, release | first public release |

`reference/` is deleted once M1 has ported what it needs.
