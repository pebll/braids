braidsd API
===========

Status: draft, API version `0.12.0`. The API is JSON-RPC 2.0
with one JSON object per line, served on a Unix socket and/or stdin/stdout.

Transport
---------

- **Unix socket** (default): `$XDG_RUNTIME_DIR/braids/braidsd.sock`, or
  `<confdir>/braidsd.sock` when braidsd runs with `--confdir` or without
  `XDG_RUNTIME_DIR` (default confdir `~/.config/braids`). `--socket PATH`
  overrides it. The socket is mode 0600 (owner only). Any number of clients
  may connect: replies go to the requesting connection, events to all of
  them, starting with `ready` as the first line of each connection. A client
  that stops reading is disconnected once 64 MB of output are pending.
- **stdio** (`braidsd --stdio`): one client on stdin/stdout, used by the
  tests; braidsd exits at end of input. No socket unless `--socket` is given
  as well.
- One braidsd per config dir (lock file). A second braidsd on a socket that
  is in use refuses to start; a stale socket file is replaced.
- Clients: `utils/braids-cli` (one-shot commands and an event stream),
  `utils/braids-console --connect` (interactive), the braids app (M3), and
  `tests/braidsd/braidsclient.py` (Python, both transports).

Versioning: `apiVersion` is semver. Clients check the major version (minor
versions only add methods, events or fields).

Conventions
-----------

- Requests: `{"jsonrpc":"2.0","id":1,"method":"chats.list","params":{...}}`.
  Replies carry `result` or `error: {code, message}`. A request without `id`
  is a notification and gets no reply.
- Events: `{"jsonrpc":"2.0","method":"message.added","params":{...}}`, sent
  unsolicited. The first line after start is the `ready` event.
- Every object that belongs to an account carries `accountId` (the profile id,
  e.g. `Matrix_alice_example.org`, `Telegram_+6511111111`).
- Times are milliseconds since epoch. Ids are opaque strings.
- Clients de-duplicate messages by `(accountId, chatId, id)`: the same message
  can arrive in a `messages.get` page and in an event.
- Secrets passed in `accounts.authInput` are never logged by braidsd.

Error codes: JSON-RPC (`-32700` parse error, `-32600` invalid request,
`-32601` method not found, `-32602` invalid params, `-32603` internal), and
`1` unknown account, `2` unknown chat, `3` unknown message, `4` unsupported
(by the protocol or its capabilities), `5` failed.

Objects
-------

- **Account**

    {"accountId", "protocol": "Telegram|WhatsAppMd|Signal|Matrix|Dummy",
     "displayName", "selfId", "state": "connecting|online|offline|error",
     "error": "<message if state is error>", "syncing": <fetching or syncing>,
     "capabilities": {"editWindowSec": -1 (unlimited) | 0 (none) | seconds,
                      "pinChat", "archiveChat", "pinMessage",
                      "limitedReactions", "readReceiptsPerMessage",
                      "avatars", "descriptions", "joinChat", "createGroup", "muteChat",
                      "polls", "pollsInGroupsOnly"}}

- **Chat**

    {"accountId", "id", "name", "isGroup", "isUnread", "isUnreadMention",
     "unreadCount", "unreadEvents", "isMuted", "isPinned", "isArchived",
     "lastActivity": <time of last message, -1 if unknown>,
     "lastMessage": Message | null, "typing": [userId]}

Protocols report unread state per chat, not counts: `unreadCount` is
braidsd's count of received unread messages after the last own or read one
(from the loaded messages and the history cache), group events (`system`
messages: joined, left, ...) not included; those are `unreadEvents`. A chat
that `isUnread` with only events has `unreadCount` 0 (clients show it like a
muted chat); with neither known, `unreadCount` is 1. `isUnread` is also true for chats marked with
`chats.markUnread` until they are read. `isMuted` is braids' own setting when one was made with
`chats.mute`, else the messenger's; muted chats send no `notification`
(except for mentions).

- **Message**

    {"accountId", "chatId", "id", "senderId", "senderName", "text", "time",
     "isOutgoing", "isRead", "isEdited", "isDeleted", "isPinned", "hasMention",
     "quoted": {"id", "text", "senderId", "senderName"} | null,
     "file": {"id", "path", "name", "mimeType", "kind": "" | "sticker" | "gif",
              "status": "none|notDownloaded|downloading|downloaded|failed"} | null,
     "reactions": {"counts": {emoji: n}, "senders": {userId: emoji}, "own": emoji},
     "mentions": {displayName: userId}, "link", "system",
     "poll": {"question", "options": [{"id", "text", "votes", "voters": [userId]}],
              "multiple", "closed", "anonymous", "own": [optionId], "total"} | null,
     "pending"?: "waiting" | "sending" | "failed"}

`poll` is set for a poll message (its `text` is empty). `votes` per option,
`total` people who voted, `own` the options of the own vote, `voters` the
other people per option where the messenger tells (WhatsApp, Signal, Matrix:
braidsd counts each vote, kept in the message cache; Telegram counts on its
server and does not name voters). A changed poll arrives as `message.updated`.

`pending` is set on own messages braidsd has not handed to the messenger
yet (its outbox; ids start with `braids-out-`): `waiting` while the account
is not online (sent when it is back, also after a restart), `sending` until
the messenger's own copy arrives (then `message.removed` for the pending one
and `message.added` for the real one), `failed` when the messenger refused
it (`messages.resend` or `messages.delete`). The outbox is kept in
`<profile>/braids-outbox.json` (readable by the user only).

`system` is non-empty for group events (someone joined, left, was added,
renamed the group, ...): a phrase to follow the sender's name, e.g.
`"joined"`, `"added Pam, Jim"`, `"changed the group name to Sales"`
(several joined by `"; "`). Clients show these as event lines rather than
bubbles; `text` keeps the protocol's form (`[Joined]`). They send no
`notification`.

`file.kind` is `"sticker"` for stickers (shown without a bubble; WebP
images and Telegram's `.tgs` animated stickers, `application/x-tgsticker`,
always count as stickers; their `text` is the sticker's emoji) and `"gif"`
for GIFs and animations without sound (shown muted and looping). Telegram
marks them with a `braids=` parameter on the file type, WhatsApp, Signal
and Matrix in the file name (`x.sticker.webp`, `x.gif.mp4`).

`text` uses the internal markdown-like format (`*bold*`, `_italic_`,
`~strike~`, `` `code` ``, mentions as `@Name` / `@[Name With Spaces]`);
protocols convert at their edge.

**Contact**: `{"id", "name", "phone", "isSelf"}`

Methods
-------

| Method | Params | Result |
| --- | --- | --- |
| `hello` | | `{apiVersion, version, protocols: [name], startTime, pid}` (`startTime`: ms; clients restart a daemon older than its binary) |
| `daemon.shutdown` | | `{}`, then braidsd exits (also on SIGTERM/SIGINT) |
| `accounts.list` | | `[Account]` |
| `accounts.add` | `protocol` | `{setupId}`, then `account.auth` events, see below |
| `accounts.authInput` | `setupId` or `accountId`, `field`, `value` | `{}`; error 5 if no input for `field` is pending |
| `accounts.authCancel` | `setupId` or `accountId` | `{}` |
| `accounts.reconnect` | `accountId`? (all without) | `{count}`: the accounts asked to drop their connection and connect again now (after the computer woke up or the network came back; a dead connection is otherwise noticed only by a keep-alive timeout). Their state goes through `connecting` as usual |
| `accounts.remove` | `accountId` | `{}`, then `account.removed` |
| `chats.list` | `accountId`?, `includeArchived`? (false) | `[Chat]` sorted: pinned, then recent activity |
| `chats.get` | `accountId`, `chatId` | `Chat` |
| `chats.setCurrent` | `accountId`, `chatId` | `{}`; tells the protocol which chat is open, requests presence |
| `chats.create` | `accountId`, `userId` | `{}`, then `chat.created` |
| `chats.join` | `accountId`, `link` (invite link, public name or address: `https://t.me/+…`, `@name`, `https://chat.whatsapp.com/…`, `#room:server`, `https://matrix.to/#/…`; capability `joinChat`) | `{}`, then `chat.joined` |
| `chats.createGroup` | `accountId`, `name`, `userIds` (contact ids, at least one; capability `createGroup`) | `{}`, then `chat.groupCreated` |
| `chats.pin` | `accountId`, `chatId`, `isPinned`? (true) | `{}` (error 4 if unsupported) |
| `chats.archive` | `accountId`, `chatId`, `isArchived`? (true) | `{}` (error 4 if unsupported) |
| `chats.mute` | `accountId`, `chatId`, `isMuted`? (true) | `{}`, then `chat.updated`; kept by braidsd for every protocol (the messenger's own setting is not changed) |
| `chats.markUnread` | `accountId`, `chatId` | `{}`, then `chat.updated`; braids' own mark (not synced to the messenger), cleared by `messages.markRead` |
| `chats.delete` | `accountId`, `chatId` | `{}`, then `chat.removed` (Matrix: leave + forget) |
| `contacts.list` | `accountId` | `[Contact]` |
| `avatars.get` | `accountId`, `ids` ([user or chat id]) | `{accountId, avatars: {id: path}}` with the known ones (`""`: no picture); the others are fetched once per session and arrive as `avatar.updated` |
| `avatars.getFull` | `accountId`, `id` | `{accountId, id, path?}`: the full-size picture (picture viewer) when already fetched; else it is fetched and arrives as `avatar.full` |
| `descriptions.get` | `accountId`, `ids` ([user or chat id]) | `{accountId, descriptions: {id: text}}` (group description or user bio; for a one-to-one chat the other user's) with the known ones (`""`: none); the others are fetched once per session and arrive as `description.updated` |
| `media.list` | `kind` (`media`: images and videos, stickers not included, `files`, `links`, `pinned`: pinned messages), `accountId`? and `chatId`? (all chats/accounts without), `beforeTime`? (ms, from `nextBefore`), `limit`? (60, max 200) | `{items: [Message] newest first, nextBefore}` (`nextBefore` 0: no more); from the history cache and the loaded messages, `files.download` works for these too |
| `messages.search` | `text` (case-insensitive for ASCII letters), `accountId`? and `chatId`? (all chats/accounts without), `beforeTime`?, `limit`? (40, max 200) | `{items: [Message] newest first, nextBefore}` like `media.list`; group events are left out |
| `members.get` | `accountId`, `chatId` | `{accountId, chatId, members: [Contact]}` (async) |
| `messages.get` | `accountId`, `chatId`, `fromMsgId`? (latest), `limit`? (30) | `{accountId, chatId, fromMsgId, messages: [Message] oldest first, timedOut}` (async, from cache or server) |
| `messages.send` | `accountId`, `chatId`, `text`?, `quotedId`?, `poll`? (`{question, options: [text], multiple}`, 2–12 options, without text or file; account capability `polls`, on Telegram in groups only: `pollsInGroupsOnly`), `filePath`? (absolute), `isVoice`? (false; the file, Ogg/Opus, is sent as a voice note: WhatsApp PTT, Telegram voice note, Signal voice message, Matrix MSC3245 voice) | `{msgId}`: the local id of the pending message, which arrives at once as `message.added` with `pending` (see Message) |
| `messages.vote` | `accountId`, `chatId`, `msgId` (a poll), `optionIds` (`[]` takes the vote back; one option unless `multiple`) | `{}`; the poll arrives as `message.updated` |
| `messages.resend` | `accountId`, `chatId`, `msgId` (a `failed` pending message) | `{}`, then `message.updated` |
| `messages.edit` | `accountId`, `chatId`, `msgId`, `text` | `{}` (error 4 outside the edit window) |
| `messages.delete` | `accountId`, `chatId`, `msgId` | `{}`, then `message.removed` (a pending message is only dropped from the outbox) |
| `messages.pin` | `accountId`, `chatId`, `msgId`, `isPinned`? (true) | `{}`, then `message.updated` (error 4 if unsupported, see `capabilities.pinMessage`) |
| `messages.markRead` | `accountId`, `chatId`, `msgId` | `{}`; marks the chat read up to `msgId` |
| `messages.find` | `accountId`, `chatId`, `text`, `fromMsgId`? | `{found, msgId}` (async, searches the local cache) |
| `reactions.send` | `accountId`, `chatId`, `msgId`, `emoji` (empty removes) | `{}` |
| `reactions.available` | `accountId`, `chatId`, `msgId`? | `{limited, emojis}` (async) |
| `files.download` | `accountId`, `chatId`, `msgId` | `{}`, then `message.updated` with file status/path |
| `typing.send` | `accountId`, `chatId`, `isTyping`? (true) | `{}` |

Paging: call `messages.get` without `fromMsgId` for the latest messages, then
with the id of the oldest message received to go back. An empty `messages`
list means the start of the history was reached (or, with `timedOut`, that
the protocol did not answer within 15 s).

Events
------

| Event | Params |
| --- | --- |
| `ready` | `{apiVersion}` |
| `account.auth` | `{setupId or accountId, step, field, message, data, isSecret, isOptional}` |
| `account.added` | `{setupId, account: Account}` |
| `account.setupFailed` | `{setupId, message}` (details were sent as `account.auth` error steps) |
| `account.removed` | `{accountId}` |
| `account.state` | `{accountId, state, error, syncing}` (`syncing`: the protocol is fetching or syncing, e.g. catching up after a reconnect) |
| `account.connected` | `{accountId, success}` (protocol login done, chats are being fetched) |
| `chat.updated` | `Chat` (new or changed) |
| `chat.created` | `{accountId, chatId}` (after `chats.create`) |
| `chat.joined` | `{accountId, link, success, chatId, error}` (after `chats.join`; `error` is a message for the user) |
| `chat.groupCreated` | `{accountId, name, success, chatId, error}` (after `chats.createGroup`; the new chat also arrives as `chat.updated`) |
| `chat.removed` | `{accountId, chatId}` |
| `chat.moved` | `{accountId, chatId, newChatId}` (e.g. Telegram group upgraded) |
| `contacts.updated` | `{accountId, fullSync, contacts: [Contact]}` |
| `avatar.updated` | `{accountId, id, path}` (local image file, `""` if none; also sent when a picture changes) |
| `avatar.full` | `{accountId, id, path}`: answers `avatars.getFull` (the small picture when the protocol has no bigger one, `""` if none) |
| `description.updated` | `{accountId, id, description}` (plain text, `""` if none; also sent when it changes) |
| `members.updated` | `{accountId, chatId, members: [Contact]}` |
| `message.added` | `Message` |
| `message.updated` | `Message` |
| `message.removed` | `{accountId, chatId, msgId}` |
| `message.sendFailed` | `{accountId, chatId}` (a send not from `messages.send`; those become `failed` pending messages) |
| `typing` | `{accountId, chatId, userId, userName, isTyping}` |
| `presence` | `{accountId, userId, isOnline, timeSeen}` |
| `notification` | `{accountId, chatId, msgId, chatName, senderName, isGroup, hasMention, text, hasFile}` |

`notification` is sent for incoming unread messages received after the
account connected, in chats that are not muted or archived (mentions notify
in muted chats). Whether to show it (window focus, open chat, settings) is up
to the client.

Login flows
-----------

`accounts.add {protocol}` returns a `setupId` and starts the protocol's setup.
Each step arrives as `account.auth`:

- `step: "input"`: ask for `field` (label in `message`, default value in
  `data`, mask if `isSecret`, may be empty if `isOptional`) and answer with
  `accounts.authInput {setupId, field, value}`.
- `step: "qr"`: show `data` as QR code with the instructions in `message`.
  It is replaced by a new QR code or the next step.
- `step: "info"`: show `message` (and `data`, e.g. a pairing code).
- `step: "error"`: show `message`; the flow either asks again or fails.

The flow ends with `account.added` or `account.setupFailed`.
`accounts.authCancel` aborts a pending input. Fields per protocol:

| Protocol | Steps |
| --- | --- |
| Telegram | `phone`, QR code (or code via `USE_PAIRING_CODE`), `code`, `password` (2FA), `firstName`/`lastName` (new accounts) |
| WhatsApp | `phone`, QR code or pairing code (`USE_PAIRING_CODE`), `passkeyResponse` and `confirm` for passkey-locked accounts |
| Signal | `phone`, QR code |
| Matrix | `userId`, `homeserver` (default discovered), `password`, `recoveryKey` (optional; `never` stops asking). With single sign-on (SAML, OpenID Connect, ... at the homeserver) `password` is optional (empty: sign in in the browser) or not asked; an `info` step then has the sign-in URL in `data` (clients open it), and braidsd waits for the browser to come back |

After setup, the same events with `accountId` instead of `setupId` are used
for prompts at runtime, e.g. Telegram re-authentication or the Matrix
recovery key on an unverified device; errors at runtime also set the
account `state` to `error`.

Known limitations
----------------------

- `accounts.authCancel` during a setup aborts it (also while waiting for a QR
  scan); for an account it only skips the current prompt.
- `chats.mute` is braids-only: other devices keep the messenger's setting.
