braids — GUI Design Brief
=========================

This brief is meant to be handed to a designer (or design tool) to produce
the visual design of the braids desktop app. It describes what the app is,
the agreed design direction, every screen and state that needs a design, and
the deliverables. Functional scope comes from
[REQUIREMENTS.md](REQUIREMENTS.md); this document only adds design decisions.


1. The product in one paragraph
-------------------------------

braids is an open source desktop messenger for **Linux and macOS** that puts
**Telegram, WhatsApp, Signal and Matrix** into one app. Users can connect
several accounts (also several of the same protocol) and see all chats in one
**unified inbox**. It is local-first and privacy-focused: no telemetry, no
cloud service, everything stays on the user's machine. It should feel like a
normal, well-made messenger, not like a "web wrapper of four web apps".
Audience: privacy-aware people and power users who are tired of running four
messenger apps; many use Linux, many like keyboard control.


2. Design direction (agreed)
----------------------------

| Topic | Decision |
|---|---|
| Personality | **Calm, native, minimal**, with a **subtle warm "braids" touch**. Neutral greys, one main accent, system fonts, should feel at home on GNOME and macOS. The warmth comes from the brand: a warm accent color and the motif of interwoven strands (used sparingly: logo, empty states, welcome screen, loading/syncing indicators — never as busy decoration in the chat itself). |
| Layout | **2-pane + collapsible sidebar**: sidebar (folders/filters) · chat list · conversation, plus an optional **right info panel**. |
| Messages | **Bubbles**: own messages right, others left, rounded bubbles. |
| Protocol / account identity | **Protocol badge + account color**: small protocol icon on the avatar corner, and a per-account color dot/stripe (user-selectable, default derived from the protocol) so that two accounts of the same protocol can be told apart. The rest of the UI stays neutral; the app does **not** take on WhatsApp green / Telegram blue styling. |
| Themes | Light and dark, both first-class; "follow system" default. |
| Window sizes | Progressive collapse (see section 5). |
| Login | Modal wizard; first launch shows it as a full-window welcome. |
| Chat details | Right side panel, toggled from the chat header. |

Typography: system UI font stack (Cantarell/Inter-like on Linux, SF on macOS);
a monospace for code blocks and inline code. Icons: one consistent outline
icon set (e.g. Lucide/Phosphor style), plus the four protocol logos as small
monochrome or colored badges.

Accessibility: WCAG AA contrast in both themes, visible keyboard focus rings,
no information conveyed by color alone (account color always paired with the
protocol badge and/or account name), hit targets ≥ 32 px.


3. Main window
--------------

```
┌────────────┬──────────────────┬───────────────────────────────┬────────────┐
│ Sidebar    │ Chat list        │ Conversation                  │ Info panel │
│ (folders)  │                  │                               │ (optional) │
│            │ [search]         │ Header: avatar, name, status  │            │
│ All chats  │ ● Alice      12:04│ ─────────── Today ──────────  │ members    │
│ Unread     │   Team chat  11:50│  ┌─────────┐                  │ media      │
│ ───────    │   Bob        Mon  │  │ message │                  │ mute       │
│ ● Telegram │                  │  └─────────┘   ┌──────────┐   │ encryption │
│ ● WA work  │                  │                │ own msg  │   │ leave      │
│ ● Matrix   │                  │                └──────────┘   │            │
│ ───────    │                  │ [📎] [ composer        ] [➤] │            │
│ Archived   │                  │                               │            │
│ ⚙  + acct  │                  │                               │            │
└────────────┴──────────────────┴───────────────────────────────┴────────────┘
```

### 3.1 Sidebar (collapsible)

- Folders: **All chats**, **Unread**, then **one entry per account** (account
  color dot, protocol badge, account display name, unread count, connection
  state indicator), then **Archived**.
- Bottom: **Add account** (+), **Settings** (gear), and a reserved slot for a
  small status line (e.g. "2 accounts syncing…").
- Collapsed state: icons only (account avatars with color ring and badge,
  unread dots), tooltips on hover. A toggle button collapses/expands it.
- Account connection states must be visible here: connecting (subtle spinner),
  online (nothing extra), offline (muted/dimmed), **error** (warning icon,
  clicking shows the message and an action such as "Log in again").

### 3.2 Chat list

- Search field on top (filters chats by name); a filter toggle for unread
  only. Button to **start a new chat** (pick account → pick contact).
- Chat row: avatar (image or initials placeholder) with **protocol badge** in
  the corner and **account color** (small dot or thin left stripe — designer's
  choice, must stay quiet), chat name, last message preview (sender prefix in
  groups, "You:" for own), time, unread count badge, and icons for
  **pinned**, **muted** (muted unread count shown greyed), **encrypted**
  (where it matters), **typing…** replacing the preview, draft indicator.
- States: selected, hover, unread (bold), muted, keyboard cursor (distinct
  from selection), empty list, no search results, loading skeleton.
- Context menu: pin/unpin, mute/unmute, mark read/unread, archive, leave/delete.
  Actions not supported by the chat's protocol are hidden (see section 6).

### 3.3 Conversation

**Header**: avatar + badge, chat name, subtitle (online / last seen / "typing…"
/ member count for groups, account name in account color), buttons: search in
chat, toggle info panel, overflow menu.

**Message list** (virtualized, can be very long):
- Day separators ("Today", "Yesterday", date); "New messages" divider at the
  first unread message.
- Bubbles: own right (accent-tinted), others left (neutral). In groups:
  sender name (colored per sender) and avatar on the first message of a
  consecutive group; consecutive messages from the same sender are grouped
  with tighter spacing and adjusted corner radii.
- Inside a bubble: text with **markdown rendering** (bold, italic,
  strikethrough, inline code, code block, links), **mentions** as pills
  (highlighted if they mention me), time, edited marker ("edited"), delivery
  state for own messages (sending, sent, delivered, read, failed + retry).
- **Reply**: quoted preview at the top of the bubble (sender + one-line
  excerpt, thumbnail if media); clicking jumps to and briefly highlights the
  original.
- **Reactions**: chips below the bubble (emoji + count), own reaction
  highlighted; hover shows who reacted. Quick-reaction bar on hover.
- **Attachments**: images/video inline with aspect-preserving thumbnails and
  a lightbox; other files as **file cards** (icon by type, name, size,
  states: not downloaded → downloading with progress → downloaded with
  open/save actions, failed); voice/audio as a simple player row.
- Special message types: **deleted** ("This message was deleted"),
  **encrypted, waiting for keys** placeholder (Matrix/Signal; updates in place
  when decrypted — design a calm, non-alarming placeholder), **system/service
  messages** (X joined, title changed) as centered small text, unsupported
  message type fallback.
- Message hover/selected state with actions: reply, react, edit (own), delete,
  copy, more. Same actions in a right-click context menu. Delete asks "for me
  / for everyone" where supported.
- Infinite scroll back: loading indicator at top while older history is
  fetched; "jump to latest" floating button with unread count when scrolled
  up.
- Read receipts: small avatars or a check state ("read up to here").
- Typing indicator at the bottom of the list ("Alice is typing…").

**Composer**:
- Multi-line auto-growing text field, placeholder shows the account the
  message will be sent from ("Message via WA work…").
- Attach button (file picker), emoji button, send button. Drag & drop of
  files onto the conversation shows a drop overlay; pasted images show a
  preview before sending.
- Reply / edit mode bar above the field (quoted message, cancel ×).
- Mention autocomplete popup when typing `@` in groups.
- Formatting is plain markdown typing; no rich-text toolbar needed in v1.
- Disabled state with explanation when the account is offline or the chat is
  read-only (e.g. channel without posting rights).

**Search in chat**: search bar replacing/under the header, results list or
highlighted matches with up/down navigation and a match counter.

### 3.4 Info panel (right, toggleable)

- Large avatar, name, protocol + account (with account color), description /
  topic.
- Actions: mute (with duration), pin, archive, leave/delete chat.
- Encryption info (E2EE on/off, for Matrix: verification state in simple
  terms).
- Members list for groups (avatar, name, role/admin marker, online state).
- Shared media / files grid (can be a "coming later" placeholder section).
- On narrow windows it opens as an overlay sheet instead of a column.


4. Accounts and login (modal wizard)
------------------------------------

First launch (no accounts): **full-window welcome** with the braids logo,
one-line pitch, privacy statement ("Everything stays on this computer") and
the protocol choice. Later, "Add account" opens the same flow as a modal.

Steps:
1. **Choose protocol**: four large cards (Telegram, WhatsApp, Signal, Matrix)
   with logo and one-line note. WhatsApp card carries a short, honest note:
   "Unofficial client — WhatsApp may restrict accounts using it."
2. **Protocol-specific steps** (the core sends these step by step; the design
   needs a generic but polished template for each step type):
   - **QR code** (WhatsApp, Signal, Telegram): large QR, numbered
     instructions ("Open WhatsApp on your phone → Settings → Linked devices →
     Link a device"), QR refresh/expiry state, alternative link "Use phone
     number instead" for Telegram.
   - **Text input** steps: phone number (with country code), verification
     code, 2FA password, Matrix user id, homeserver (pre-filled from user id,
     editable), password, **recovery key** (long secret input, paste-friendly,
     explanation of what it is, "skip for now" option).
   - Secret fields are masked with a reveal toggle.
   - **Working** state (spinner, "Connecting…") and **error** state (inline
     message, retry) for each step.
3. **Syncing**: account connected, initial sync in progress (braids motif
   animation fits here), "Done" leads to the inbox.

Account settings (from sidebar context menu or Settings): display name, account
color picker (6–8 preset colors), notifications on/off, log out / remove
account (destructive confirm dialog: "Remove local data" and, where possible,
"Also log out this device").


5. Responsive behavior
----------------------

| Width | Layout |
|---|---|
| Wide (≥ ~1280 px) | Sidebar expanded · chat list · conversation · info panel as column |
| Medium (~900–1280 px) | Sidebar collapsed to icons · chat list · conversation; info panel as overlay |
| Narrow (< ~700–900 px) | Single pane: chat list **or** conversation with a back button; sidebar as drawer |

Please show the main screen in all three widths, light and dark.


6. Protocol differences the UI must handle
------------------------------------------

The UI is unified, but some actions only exist on some protocols. The design
should hide unsupported actions rather than disable them, except where
disabling with a tooltip explains something useful (e.g. "Edit window has
expired").

| Feature | Telegram | WhatsApp | Signal | Matrix |
|---|---|---|---|---|
| Edit | ~48 h | 15 min | ~24 h | always |
| Delete for everyone | yes | time-limited | time-limited | yes |
| Reactions per user | 1+ from a chat-specific set | 1 | 1 | 1 in v1 |
| Archive | yes | yes | no | no |
| Pin chat | yes | yes | no | yes (favourite) |
| End-to-end encryption | secret chats only | always | always | per room |

Out of scope for v1 (no design needed): calls, stories/status, polls
creation, threads view (thread messages appear inline as replies), spaces,
merging contacts across protocols.


7. Other screens and components
-------------------------------

- **Settings** (modal or full panel with sections): General (theme
  light/dark/system, downloads directory, start on login / keep running in
  tray), Notifications (on/off, show message preview yes/no, sound), Keyboard
  (vim mode toggle, shortcut list), Accounts (list with add/remove/edit),
  About (version, license AGPL v3, third-party licenses, link to source).
- **Desktop notification** content style (sender, chat, preview; preview can
  be hidden for privacy).
- **Tray / dock** unread badge.
- **Dialogs**: confirm destructive actions, new chat (account picker →
  contact search list), image lightbox, error toast/banner (e.g. "Telegram
  account disconnected — Reconnect").
- **Empty states**: no accounts, no chat selected ("Select a chat" with the
  subtle braids motif), empty chat, no search results, offline.
- **Keyboard**: visible focus styles; reserve space in the status area for a
  small vim mode indicator (`LIST` / `NORMAL` / `INSERT`) shown only when vim
  mode is enabled (no dedicated vim designs needed now).


8. Deliverables
---------------

1. **Screens** (light + dark): welcome / first launch, add-account wizard
   (protocol choice, QR step, text input step, recovery key step, error,
   syncing), main window wide / medium / narrow, conversation with all
   message types and states from 3.3 (a "kitchen sink" chat is ideal), group
   chat with info panel, chat-list states, settings, dialogs and empty
   states.
2. **Design tokens** for light and dark, as a list that maps directly to CSS
   custom properties: colors (background layers, surface, border, text
   primary/secondary/muted, accent, own-bubble, other-bubble, danger,
   warning, success, focus ring, 6–8 account colors, sender name colors for
   groups), type scale, spacing scale, radii, shadows, motion durations.
3. **App icon / logo**: braids logo (interwoven strands, works at 16 px and
   as a tray icon in monochrome), app icon in Linux (SVG, 512 px) and macOS
   (rounded-square icns style) variants.
4. **Clickable HTML prototype** of the main flows: first launch → add
   account (QR) → inbox → open chat → reply / react / attach → open info
   panel → switch account folder → settings → toggle dark mode.

Technical context for the designer: the app is a Tauri app (web technologies:
HTML/CSS/TypeScript) rendered in the system webview (WebKitGTK on Linux,
WKWebView on macOS). Prefer plain CSS-friendly designs (no effects that
depend on Chromium-only features); native window decorations are kept.
