braidsd API
===========

Status: draft, API version `0.3.0` (milestone M3). The API is JSON-RPC 2.0
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

**Account**

    {"accountId", "protocol": "Telegram|WhatsAppMd|Signal|Matrix|Dummy",
     "displayName", "selfId", "state": "connecting|online|offline|error",
     "error": "<message if state is error>",
     "capabilities": {"editWindowSec": -1 (unlimited) | 0 (none) | seconds,
                      "pinChat", "archiveChat", "pinMessage",
                      "limitedReactions", "readReceiptsPerMessage"}}

**Chat**

    {"accountId", "id", "name", "isGroup", "isUnread", "isUnreadMention",
     "isMuted", "isPinned", "isArchived",
     "lastActivity": <time of last message, -1 if unknown>,
     "lastMessage": Message | null, "typing": [userId]}

Protocols report unread state per chat, not counts, so there is no unread
count yet.

**Message**

    {"accountId", "chatId", "id", "senderId", "senderName", "text", "time",
     "isOutgoing", "isRead", "isEdited", "isDeleted", "isPinned", "hasMention",
     "quoted": {"id", "text", "senderId", "senderName"} | null,
     "file": {"id", "path", "name", "mimeType",
              "status": "none|notDownloaded|downloading|downloaded|failed"} | null,
     "reactions": {"counts": {emoji: n}, "senders": {userId: emoji}, "own": emoji},
     "mentions": {displayName: userId}, "link"}

`text` uses the internal markdown-like format (`*bold*`, `_italic_`,
`~strike~`, `` `code` ``, mentions as `@Name` / `@[Name With Spaces]`);
protocols convert at their edge.

**Contact**: `{"id", "name", "phone", "isSelf"}`

Methods
-------

| Method | Params | Result |
|---|---|---|
| `hello` | | `{apiVersion, version, protocols: [name], startTime, pid}` (`startTime`: ms; clients restart a daemon older than its binary) |
| `daemon.shutdown` | | `{}`, then braidsd exits (also on SIGTERM/SIGINT) |
| `accounts.list` | | `[Account]` |
| `accounts.add` | `protocol` | `{setupId}`, then `account.auth` events, see below |
| `accounts.authInput` | `setupId` or `accountId`, `field`, `value` | `{}`; error 5 if no input for `field` is pending |
| `accounts.authCancel` | `setupId` or `accountId` | `{}` |
| `accounts.remove` | `accountId` | `{}`, then `account.removed` |
| `chats.list` | `accountId`?, `includeArchived`? (false) | `[Chat]` sorted: pinned, then recent activity |
| `chats.get` | `accountId`, `chatId` | `Chat` |
| `chats.setCurrent` | `accountId`, `chatId` | `{}`; tells the protocol which chat is open, requests presence |
| `chats.create` | `accountId`, `userId` | `{}`, then `chat.created` |
| `chats.pin` | `accountId`, `chatId`, `isPinned`? (true) | `{}` (error 4 if unsupported) |
| `chats.archive` | `accountId`, `chatId`, `isArchived`? (true) | `{}` (error 4 if unsupported) |
| `chats.delete` | `accountId`, `chatId` | `{}`, then `chat.removed` (Matrix: leave + forget) |
| `contacts.list` | `accountId` | `[Contact]` |
| `members.get` | `accountId`, `chatId` | `{accountId, chatId, members: [Contact]}` (async) |
| `messages.get` | `accountId`, `chatId`, `fromMsgId`? (latest), `limit`? (30) | `{accountId, chatId, fromMsgId, messages: [Message] oldest first, timedOut}` (async, from cache or server) |
| `messages.send` | `accountId`, `chatId`, `text`?, `quotedId`?, `filePath`? (absolute) | `{}`; the message arrives as `message.added` |
| `messages.edit` | `accountId`, `chatId`, `msgId`, `text` | `{}` (error 4 outside the edit window) |
| `messages.delete` | `accountId`, `chatId`, `msgId` | `{}`, then `message.removed` |
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
|---|---|
| `ready` | `{apiVersion}` |
| `account.auth` | `{setupId or accountId, step, field, message, data, isSecret, isOptional}` |
| `account.added` | `{setupId, account: Account}` |
| `account.setupFailed` | `{setupId, message}` (details were sent as `account.auth` error steps) |
| `account.removed` | `{accountId}` |
| `account.state` | `{accountId, state, error}` |
| `account.connected` | `{accountId, success}` (protocol login done, chats are being fetched) |
| `chat.updated` | `Chat` (new or changed) |
| `chat.created` | `{accountId, chatId}` (after `chats.create`) |
| `chat.removed` | `{accountId, chatId}` |
| `chat.moved` | `{accountId, chatId, newChatId}` (e.g. Telegram group upgraded) |
| `contacts.updated` | `{accountId, fullSync, contacts: [Contact]}` |
| `members.updated` | `{accountId, chatId, members: [Contact]}` |
| `message.added` | `Message` |
| `message.updated` | `Message` |
| `message.removed` | `{accountId, chatId, msgId}` |
| `message.sendFailed` | `{accountId, chatId}` |
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
|---|---|
| Telegram | `phone`, QR code (or code via `USE_PAIRING_CODE`), `code`, `password` (2FA), `firstName`/`lastName` (new accounts) |
| WhatsApp | `phone`, QR code or pairing code (`USE_PAIRING_CODE`), `passkeyResponse` and `confirm` for passkey-locked accounts |
| Signal | `phone`, QR code |
| Matrix | `userId`, `homeserver` (default discovered), `password`, `recoveryKey` (optional; `never` stops asking) |

After setup, the same events with `accountId` instead of `setupId` are used
for prompts at runtime, e.g. Telegram re-authentication or the Matrix
recovery key on an unverified device; errors at runtime also set the
account `state` to `error`.

Known limitations
----------------------

- `accounts.authCancel` during a setup aborts it (also while waiting for a QR
  scan); for an account it only skips the current prompt.
- Removing an account deletes local data but does not log out the device
  remotely yet.
- No mute/unmute request (protocols do not offer one yet); mute state is
  reported.
