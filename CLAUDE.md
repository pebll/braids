# braids — notes for coding agents

braids: open source desktop messenger (Telegram, WhatsApp, Signal, Matrix),
Linux + macOS, Tauri GUI on top of a C++ core daemon. Forked from nchat
(terminal client). Read `doc/REQUIREMENTS.md` and `doc/ARCHITECTURE.md` first;
they hold the agreed decisions and the milestone plan (M0 done, M1 core done
pending real-account checks of Telegram/WhatsApp/Signal, next: M2). The daemon
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
  thread owning all core state), `authbridge` (login steps as API events).
  JSON via vendored `ext/nlohmann/json.hpp`.
- `tests/braidsd` — Python stdio API client and tests (Dummy always, Matrix
  when the local Synapse runs); `utils/braids-console` — interactive console
  for manual login/chat tests (QR via `qrencode`).
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
    ./build/bin/braidsd --stdio -c /tmp/braidsconf   # JSON-RPC on stdin/stdout
    utils/braids-console --confdir /tmp/braidsconf   # interactive

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

- Telegram uses nchat's API id (`lib/tgchat/src/tgchat.cpp`, overridable via
  `TG_APIID`/`TG_APIHASH`); register a braids API id before public release.
- Telegram, WhatsApp and Signal login flows through braidsd are implemented
  but not yet verified with real accounts (needs the maintainer; use
  `utils/braids-console`).
- Account removal does not log out the device remotely yet.
