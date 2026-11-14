# braids — notes for coding agents

braids: open source desktop messenger (Telegram, WhatsApp, Signal, Matrix),
Linux only (no macOS builds or CI), Tauri GUI on top of a C++ core daemon. Forked from nchat
(terminal client). Read `doc/REQUIREMENTS.md` and `doc/ARCHITECTURE.md` first;
they hold the agreed decisions and the milestone plan (M0–M3 done, Telegram/WhatsApp/Signal
verified with real accounts; M4 remaining: vim mode). The daemon
API is in `doc/API.md`; the GUI design brief in `doc/DESIGN_BRIEF.md`, and
the finished GUI design (tokens, icons, mockups, screenshots) in
`doc/design/README.md` — follow it when building the Tauri app.

## Layout

- `lib/common/src/protocol.h` — the protocol interface (requests in,
  notifications out). Central contract between core and protocols.
- `lib/{tgchat,wmchat,sgchat,mxchat,duchat}` — protocol libraries. C++ wrapper
  plus Go c-archive for WhatsApp/Signal/Matrix (cgo bridge in `go/cgo*.go`).
- `lib/ncutil` — utilities and `MessageCache` (SQLite).
- `src/braidsd` — core daemon: `core` (accounts, setups, API dispatcher),
  `chatmodel` (port of nchat's `UiModel::Impl` model), `eventloop` (single
  thread owning all core state), `authbridge` (login steps as API events),
  `server` (JSON-RPC transport: Unix socket and stdio).
  JSON via vendored `ext/nlohmann/json.hpp`.
- `tests/braidsd` — Python API client (stdio and socket) and tests (Dummy,
  socket, CLI always; Matrix when the local Synapse runs, required in the
  Integration CI workflow); `utils/braids-cli` — one-shot commands against a
  running braidsd; `utils/braids-console` — interactive console for manual
  login/chat tests (QR via `qrencode`).
- `app/` — the Tauri GUI (M3): `src-tauri/` Rust shell (socket client,
  tray), `src/` Svelte 5 + TypeScript UI (`lib/store.svelte.ts` client
  state from API events, `components/`). Runs in a browser too via the Vite
  dev bridge (`dev/bridge.ts`). See `app/README.md`.
- `reference/nchat-ui` — nchat's ncurses UI and its docs/tooling. NOT built.
  Source material for the chat model port (done in M1); delete once the
  remaining protocols are verified through braidsd.
- `lib/mxchat/NOTES.md` — detailed notes on how protocols, threading
  (Go callbacks → DeferNotifyRequest → worker thread), MessageCache and history
  paging work. Applies to all protocols, read before touching core logic.

## Build and test

    ./make.sh deps
    ./make.sh build                    # all protocols
    ./make.sh --no-telegram build      # much faster (tdlib compile is ~1 h)
    ./make.sh --no-telegram tests      # ctest incl. Go unit tests (mxchat)
    ./build/bin/braidsd --list-protocols
    ./build/bin/braidsd -c /tmp/braidsconf           # socket /tmp/braidsconf/braidsd.sock
    utils/braids-cli -c /tmp/braidsconf status       # one-shot commands
    utils/braids-console --connect --confdir /tmp/braidsconf   # interactive
    ./build/bin/braidsd --stdio -c /tmp/braidsconf   # JSON-RPC on stdin/stdout

App (see `app/README.md`; Linux needs `libwebkit2gtk-4.1-dev` etc.):

    cd app && npm install
    npm run check && npm run test:ui # type check; UI e2e (braidsd + Chromium)
    npm run tauri dev                # app window

Matrix integration testing without real accounts:
`lib/mxchat/test/homeserver.sh start` (Synapse in Docker on localhost:8008,
users alice/bob) and the scripted client `lib/mxchat/test/peer` (see
`lib/mxchat/README.md`).

`ctest -R braidsd_matrix` runs the braidsd Matrix test when that homeserver
is up (skipped otherwise).

Sandboxed/cloud environments: the pre-built libsignal download (mau.dev) may
be blocked; use `--no-signal` there, CI builds it. Docker may need `dockerd`
started by hand. `./make.sh deps` asks for apt confirmation; install
`libmagic-dev` etc. non-interactively if it aborts.

Packaging (Linux x86_64 and aarch64): `utils/dist/package-linux` builds a
relocatable tree (rpath `$ORIGIN/../lib`, `-DBRAIDS_RELOCATABLE=ON`, no Dummy)
into a tarball, a .deb and (`--appimage`) an AppImage (`utils/dist/AppRun`
runs braidsd from a cache copy, it outlives the mount);
`utils/dist/flatpak/` wraps the tarball into a Flatpak (GNOME runtime);
`utils/dist/install.sh` installs the tarball per user; tag `v*` runs
`.github/workflows/release.yml` (both arches, all formats; release notes are
the version's section of `CHANGELOG.md`: add user-facing changes under
`[Unreleased]` and rename it to the version before tagging, bump the version
in `app/package.json` (+ lock), `app/src-tauri/Cargo.toml` (+ lock) and
`app/src-tauri/tauri.conf.json` (braidsd takes its version from the last one,
`lib/ncutil/CMakeLists.txt`), then run
`utils/dist/flatpak/update-releases` (the metainfo's release list, checked
by the workflow)). Nix: `flake.nix` / `nix/package.nix` wrap the release tarball, version and
checksums in `nix/release.json` (`utils/dist/nix-update`, committed to main
by the release workflow). Flatpak ID `io.github.pebll.braids` (the Tauri
identifier stays `org.braids.app`: it names the settings' directory). Test builds:
`.github/workflows/preview.yml` publishes every pull request as the
pre-release `pr-<N>` (`install.sh --pr N`; `--branch NAME` after running it
by hand on a branch), deleted when the pull request closes; it also runs
(build only) on every push to main, which keeps the ccache/Rust/Go caches
warm (GitHub shares only main's caches with pull requests and tags). Website: `site/` (GitHub Pages,
`.github/workflows/pages.yml`); screenshots and the README demo GIF
(`site/img/demo.gif`) come from `app/scripts/site-screenshots.mjs`.

## Conventions

- C++ style as in nchat: 2-space indent, braces on own lines, `m_` members,
  `p_` parameters, `s_` statics; `uncrustify` config in `etc/`
  (`./make.sh src`). Go: `gofmt`.
- New files: header `Copyright (c) 2026 braids contributors` and
  "braids is distributed under the GNU AGPL v3 license, see LICENSE". Keep
  existing nchat copyright headers on files derived from nchat (MIT,
  `LICENSE.nchat`).
- Protocol libraries must stay UI-agnostic; all UI interaction goes through
  `protocol.h` messages, user prompts through `AuthHandler` / `AuthUtil`
  (never `std::cin`/`std::cout`).
- Never log secrets (tokens, passwords, keys, recovery keys) or message
  plaintext, at any log level. Profile files are 0600 / dirs 0700.
- Internal names inherited from nchat (`NCHAT_*` CMake/macros, `ncutil`,
  `NCHAT_CMAKEARGS`, `NCHAT_PREFIX`) are kept for now to limit churn.
- Keep the protocol libraries updatable from upstream: `utils/whatsmeow-update`,
  `utils/signal-update`, `utils/tdlib-update`, `utils/mautrix-update`.

## Known TODOs

Scheduled-later features are in `doc/BACKLOG.md`.


- Telegram API credentials: braids' own are given at build time
  (`TG_API_ID`/`TG_API_HASH`: CMake variables, environment, or a git-ignored
  `.env` in the repository root, see `.env.example`; the release
  workflow takes them from repository secrets). Builds without them fall back
  to nchat's credentials (development only).
- App: vim mode is M4. Muting and "mark unread" are braids' own settings
  (`chats.mute`, `chats.markUnread`, kept in `<profile>/braids-chats.json`),
  not synced to the messenger. Unread counts are computed by braidsd
  (`ChatModel::UnreadCount`, history cache at connect + loaded messages). The
  conversation renders a window of the loaded messages (`Conversation.svelte`,
  `WINDOW`/`REVEAL`). Group events reach braidsd as bracketed texts (`[Joined]`);
  `SystemText()` in `chatmodel.cpp` turns them into the message's `system`
  phrase.
- Stickers/GIFs: `file.kind` from `FileTypeAndKind()` (chatmodel.cpp);
  protocols mark them in the file type (`; braids=sticker`) or file name
  (`.sticker.`, `.gif.`). Link previews are fetched by the Rust shell
  (`src-tauri/src/linkpreview.rs`, no local/LAN addresses). Adding a request
  or notify type to `protocol.h` shifts the enum: rebuild all protocol
  libraries (`make`, not only `make braidsd`).
- Outbox: `messages.send` queues in `ChatModel` (`OutboxEntry`, ids
  `braids-out-…`, `<profile>/braids-outbox.json`), emitted as pending
  messages; the request's `chatMessage.id` carries the local id back in
  `SendMessageNotify`; the echo is matched by text/file name
  (`MatchOutboxEcho`). Dummy: `/offline N`, `/fail` for tests.
- Reconnect: `ReconnectRequest` / `accounts.reconnect`, called by the app on
  wake (timer gap) and when the network returns.
- Media: WebKitGTK does not play media from the `asset:` protocol; videos and
  audio go through blob URLs (`app/src/lib/media.ts`, `use:mediaSrc`).
- Polls: `ChatMessage.poll` (JSON), `VotePollRequest`, `NewPollNotify`
  (protocol.h). Telegram counts votes on its server (`counts`); for the
  others braidsd counts each vote (`ChatMessage.pollVotes`, cache columns
  `poll`/`pollVotes`, `ChatModel::OnPoll`). Dummy: `/poll`, `/vote N`,
  `/endpoll`; the Matrix test peer: `poll`, `vote`, `endpoll`.
- Telegram "joined Telegram" messages: left out by braidsd unless the
  account option `showJoinedMessages` (`accounts.setOption`, kept in
  `braids-chats.json` with the chats it hides, `hiddenChats`) is on.
  Dummy: `/joined`, `/joined <text>`.
- Voice messages are recorded as Ogg/Opus; for Signal the app converts them
  to AAC first (`voice::to_aac`, Signal on iPhone plays no Ogg).
- Chat list keyboard: one highlight (`ui.listCursor`, `ui.list`), Alt+J/K
  move it, Enter opens.
- Group members: `EditMembersRequest` / `EditMembersNotify` (protocol.h,
  `FeatureEditMembers`, Matrix only: invite / kick), API `members.add`,
  `members.remove`, `members.edited`; `NewGroupMembersNotify.canAdd` /
  `canRemove` come from the room's power levels. Dummy: The Office, self may
  remove anyone but Michael.
