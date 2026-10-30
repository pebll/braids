braids — Requirements
=====================

braids is an open source desktop messenger that brings Telegram, WhatsApp,
Signal and Matrix together in one app. It is based on the protocol layer of
[nchat](https://github.com/d99kris/nchat) and replaces nchat's terminal UI with
a GUI.

Status: draft v0.1 (milestone M0). Decisions below were agreed with the
maintainer; items marked *open* still need a decision.


1. Goals
--------

| # | Requirement |
|---|---|
| G1 | One app for **Telegram, WhatsApp, Signal and Matrix** (incl. end-to-end encryption for Matrix, as already implemented in `lib/mxchat`). |
| G2 | **GUI that behaves like any other messenger**: chat list on the left, conversation on the right, composer at the bottom, mouse and keyboard. |
| G3 | **Unified inbox**: all chats of all accounts in one list, sortable by recent activity, filterable by account/protocol and unread. |
| G4 | **Multiple accounts**, also several per protocol. |
| G5 | **Optional vim mode**, enabled in settings (off by default), see section 4. |
| G6 | **Local-first**: all data on the user's machine, instant start from the local cache, works offline for reading. |
| G7 | **Privacy**: no telemetry, no hosted service; secrets and message text never written to logs; owner-only file permissions. |
| G8 | **Open source** under the GNU AGPL v3, usable by everyone. |
| G9 | **Platforms**: Linux and macOS. |

2. Functional scope (v1)
------------------------

Accounts
- Add account per protocol from the GUI, with the protocol's login flow
  rendered in the GUI: QR code (WhatsApp, Signal, Telegram), phone number and
  code / 2FA password (Telegram), user id, homeserver and password, recovery
  key (Matrix).
- Remove account (local data; where the protocol allows, also log out the
  device remotely).
- Connection state per account (connecting, online, offline, error with a
  clear message and action, e.g. "session expired, log in again").

Chat list
- All chats of all accounts, with name, avatar (or placeholder), protocol
  badge, last message preview, time, unread count/indicator, muted, pinned.
- Search/filter chats by name; filter by account and unread.
- Pin, mute indicator, archive (where supported), leave/delete chat.
- Start a new chat with a contact.

Conversation
- History with infinite scroll back (server backfill), day separators,
  sender names and avatars in groups.
- Text with markdown rendering (bold, italic, strike, code, links), mentions.
- Reply (quoted preview, jump to original), edit (shown as edited), delete,
  reactions (add/remove, show counts and who reacted).
- Attachments: send via picker, drag & drop and paste; inline image preview,
  file cards for other types, download / open / save.
- Read receipts and typing indicators.
- Encrypted message placeholders ("waiting for keys") that update in place.

App
- Desktop notifications (respecting mute), unread badge in the tray/dock.
- Search messages within a chat (from the local cache).
- Settings: theme (light/dark/system), notifications, vim mode, downloads
  directory, per-account settings.
- Keyboard shortcuts for common actions (also without vim mode).

3. Non-goals (not in v1)
------------------------

- Terminal UI (nchat's ncurses UI is not carried over; it is kept under
  `reference/` only as source material).
- Windows support.
- Linking the same person across protocols into one contact.
- Calls (voice/video), stories/status, polls creation, bots/channel admin,
  message scheduling, account registration, spaces management.
- Hosted/sync service, mobile apps.
- Threads UI (thread messages are shown inline as replies).

4. Vim mode (optional)
----------------------

Off by default; when enabled, keyboard navigation is modal:

| Mode | Keys |
|---|---|
| **List** (initial) | `j`/`k` move the chat cursor, `/` search chats, `Enter`/`l` open chat (Normal), `gg`/`G` first/last |
| **Normal** (in chat) | `j`/`k` select message, `i`/`a` Insert, `r` reply, `e` edit, `dd` delete, `y` copy, `o` open attachment, `s` react, `gg`/`G`, `Ctrl-u`/`Ctrl-d` page, `Esc`/`h` back to List |
| **Insert** (composer) | normal typing, `Esc` back to Normal, Enter sends |

A mode indicator is always visible while vim mode is on. Mouse use keeps
working and switches mode as needed.

5. Protocol unification
-----------------------

The unified model is nchat's `lib/common/src/protocol.h` (`ChatInfo`,
`ChatMessage`, `ContactInfo`, request/notify messages), extended where needed.
Rules:

- **Ids** are scoped by account: `accountId` + protocol chat id + message id.
  `accountId` = nchat profile id (e.g. `Matrix_alice_example.org`).
- **Capabilities** per account replace nchat's coarse `HasFeature` flags, and
  the GUI hides or limits actions accordingly:

| Capability | Telegram | WhatsApp | Signal | Matrix |
|---|---|---|---|---|
| Edit window | ~48 h | 15 min | ~24 h | unlimited |
| Delete for everyone | yes | time-limited | time-limited | yes (redaction) |
| Reactions per user | limited set per chat | 1 | 1 | many (UI: 1 for v1) |
| Read receipts | chat level | per message | per message | read up to event |
| Formatting | entities | basic markdown | style ranges | HTML |
| Mentions | entities | ids | ranges | pills + `m.mentions` |
| Archive | yes | yes | — | — (tags) |
| Pin chat | yes | yes | — | favourite tag |
| E2EE state | secret chats only | always | always | per room |

  *(values to be verified per protocol during M1)*

- **Formatting**: internal representation is markdown-like text plus mention
  spans; each protocol converts at its edge (as `lib/mxchat` already does).
- **Read state**: modelled as "read up to message X" per chat; protocols
  with per-message receipts map onto it.
- **Attachments**: one model (file id, name, mime type, size, local path,
  download state, encrypted yes/no), downloaded on demand or prefetched.
- **Contacts**: per account; no cross-protocol merging (non-goal).

6. Non-functional requirements
------------------------------

- Start to usable chat list from cache in < 1 s on a typical laptop.
- Smooth scrolling in chats with 10k+ cached messages (virtualized list).
- No data loss on crash; core keeps running when the GUI closes (tray).
- Daemon API documented and versioned; GUI and core can be updated
  independently within a major API version.
- Builds reproducibly from source on Linux and macOS; CI builds all four
  protocols.

7. Constraints, risks and open items
------------------------------------

- **Telegram API id**: `lib/tgchat` currently uses nchat's API credentials
  (overridable with `TG_APIID`/`TG_APIHASH`). braids must register its own at
  https://my.telegram.org before a public release. *Blocker for release.*
- **WhatsApp** use via whatsmeow is unofficial; accounts may be banned by
  WhatsApp. Must be stated clearly in the app and README.
- **Licensing**: braids is AGPL v3; nchat-derived code keeps its MIT notice
  (`LICENSE.nchat`); third-party notices are generated into
  `THIRD_PARTY_LICENSES` and shipped with binaries.
- **Name**: availability of "braids" (GitHub org, Flathub, AUR, Homebrew,
  domain) to be checked before public announcement. *open*
- **Distribution format**: AppImage / Flatpak / deb on Linux, dmg on macOS;
  macOS signing/notarization needs an Apple developer account. *open*
- Login flows of all protocols are currently terminal-based (stdin prompts,
  QR printed to the terminal) and must be reworked into API events (M1).
