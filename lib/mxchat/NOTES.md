# mxchat development notes

Orientation notes for the Matrix backend, from reading `lib/common/src/protocol.h`,
`lib/duchat`, `lib/wmchat`, `lib/sgchat`, `src/main.cpp`, `src/uimodel.cpp` and
`lib/ncutil/src/messagecache.cpp` (upstream commit e424aea1).

## 1. Lifecycle

```
main --setup:  Create() -> SetupProfile(profilesDir, &profileId)   [stdin/stdout, before UI]
main normal:   for each dir in profiles/:  "<ProtocolName>_<rest>"
                 -> factory with GetName() == ProtocolName -> Create() -> LoadProfile(profilesDir, id)
               ui->Init(); for each protocol: Login()
exit:          Logout(); CloseProfile()
```

- The profile id must start with `GetName()` followed by `_`. main splits on the first `_`.
  So the Matrix profile id is `Matrix_<sanitized mxid>`, and the sanitized part may contain
  further `_`.
- After a successful setup, main reuses the same protocol instance; it does not call
  `LoadProfile` again. Login() is then called again from main, so Login must be idempotent
  (wmchat guards it with `m_Running`). wmchat calls Login() inside SetupProfile so that
  authentication happens during setup.
- `GetSetupMessage()` is printed before SetupProfile (used for warnings).
- Prompts: plain `std::cout`/`std::getline(std::cin)`. `StrUtil::GetPass()` reads without
  echo. Use it for the password and recovery key.
- Files: `~/.config/nchat/profiles/<ProfileId>/` holds protocol state (wmchat: `whatsappmd.conf`
  through `Config`, plus whatsmeow's sqlite store). **Message cache is NOT there**: it is at
  `~/.config/nchat/history/<ProfileId>/db.sqlite` (the plan said profiles dir; corrected).
- main sets `umask(077)`, so new files are 0600 and dirs 0700 by default. mxchat should still
  chmod secrets explicitly.
- `--remove` deletes the profile dir; there is no protocol callback for it.

## 2. Threading and the Go/C++ bridge (wmchat pattern)

- C++ `XxChat` owns one worker thread (`Process()`) that drains `m_RequestsQueue`. `SendRequest`
  only enqueues. Requests are executed serially in `PerformRequest` (50 ms sleep after each,
  except DeferNotify).
- Go side: `CWmInit(path, proxy, sendType)` returns a `connId` (index into a Go-side map of
  clients). C++ keeps a static `connId -> instance` map (`AddInstance/GetInstance`) so that
  `extern "C"` callbacks can find the instance.
- Go -> C++: `cgowm.go` declares `extern void WmXxxNotify(...)` in the cgo preamble and links
  with `-Wl,-unresolved-symbols=ignore-all` (darwin: `-undefined,dynamic_lookup`). The symbols
  are resolved at load time from the C++ library. Strings are passed as `C.CString` (malloc);
  **the C++ callback must `free()` every char\*.**
- Callbacks are invoked on arbitrary goroutine threads. They never call the UI directly: they
  build a `ServiceMessage`, wrap it in `DeferNotifyRequest`, and `SendRequest()` it. The worker
  thread then calls `CallMessageHandler()`, which does
  `MessageCache::AddFromServiceMessage()` and then `m_MessageHandler()` (the UI). That
  serializes everything that reaches the UI and the cache.
- Results of C++-initiated requests (SendMessage, MarkRead, Delete, ...) are produced
  synchronously by the C++ side after the `CWm*` call returns (e.g. `SendMessageNotify` with
  `success = rv == 0`). Incoming data comes asynchronously through the callbacks.
- Logging: Go calls `WmLogTrace/Debug/Info/Warning/Error(file, line, msg)`, which forward to
  nchat's `Log::`. Status bar flags: `WmSetStatus/WmClearStatus(Status::Flag*)`.
- `WmSetProtocolUiControl(connId, 1)` makes the UI release the terminal (wmchat uses it for QR
  display after setup). This could be used later for SAS verification or recovery-key prompts
  at runtime.
- Build: `go build -buildmode=c-archive` (or c-shared when `HAS_STATICGOLIB=OFF`) produces
  `libcgowm.a` and `libcgowm.h`. CMake wraps it as the imported target `ref-cgowm`. The C++
  library is a shared lib (`libwmchat.so`) that is `dlopen`ed by main (`HAS_DYNAMICLOAD`, the
  default), via `CreateWmChat()`.
- **Static builds**: two Go c-archives cannot both link into one executable (duplicate Go
  runtime and go-sqlite3 symbols). Upstream solves this for WhatsApp+Signal with
  `lib/gostat` (one combined module). Matrix in a fully static build (`HAS_SHARED_LIBS=OFF`)
  together with WhatsApp/Signal would need to be added to gostat. mxchat's CMake setup refuses that
  combination; the default shared-lib build is unaffected.
- The Go toolchain: upstream go.mod files say `go 1.26.0`; the container has 1.24.7, with
  `GOTOOLCHAIN=auto` to fetch newer. mautrix-signal pins `maunium.net/go/mautrix v0.32.0`
  (current latest), so mxchat pins the same version to keep a future combined build consistent.

## 3. Requests the UI sends, and what it expects back

| Request | When | Expected response |
|---|---|---|
| `GetChats` (opt. `chatIds`) | after `ConnectNotify`, unless `FeatureAutoGetChatsOnLogin`; also for specific ids | `NewChatsNotify` (+ `NewContactsNotify` for names). wmchat/sgchat answer from cache (`MessageCache::FetchChats`), and the live backend pushes `NewChatsNotify` as it syncs. |
| `GetContacts` | after `ConnectNotify`, unless `FeatureAutoGetContactsOnLogin` | `NewContactsNotify` (`fullSync=true` replaces the cached contact set). Chat names come from contacts with `id == chatId`; there is no separate chat-name field. Report self with `isSelf=true` (the UI uses it for "You"/self id). |
| `GetMessages(chatId, fromMsgId, limit)` | opening/scrolling a chat; `fromMsgId=""` means newest | `NewMessagesNotify` with `sequence=true`, `fromMsgId` echoed, messages **older than** fromMsgId (exclusive), up to `limit`. Pattern: `MessageCache::FetchMessagesFrom(...)` first, and if it returns false (nothing cached older), fetch from the server. The UI de-duplicates requests per fromMsgId, so a request that yields nothing is not retried. |
| `GetMessage(chatId, msgId)` | resolving a quoted message | `MessageCache::FetchOneMessage`. |
| `SendMessage(chatId, chatMessage)` | send; `quotedId` for replies, `fileInfo` (hex) for attachments, `mentions` | `SendMessageNotify(success)`. The real message arrives later through `NewMessagesNotify` (the remote echo) with the server id. |
| `EditMessage(chatId, msgId, chatMessage)` | edit; the UI only allows editing if `FeatureEditMessagesWithinTwoDays` or `...FifteenMins` is set (neither = editing disabled). Matrix has no limit, so mxchat sets TwoDays for now. A `FeatureEditMessagesAnytime` would be a small upstream change | the edit then arrives as `NewMessagesNotify` with the same id and `isEdited=true` |
| `MarkMessageRead(chatId, senderId, msgId)` | viewing | `MarkMessageReadNotify(success)` |
| `DeleteMessage(chatId, senderId, msgId)` | delete | `DeleteMessageNotify(isOutgoing=true)` |
| `DeleteChat` | delete chat | `DeleteChatNotify` (Matrix: leave + forget room) |
| `SendTyping(chatId, isTyping)` | typing | `SendTypingNotify` |
| `SetStatus(isOnline)` | if `online_status_share` | `SetStatusNotify` |
| `GetStatus(userId)` | per chat | async `ReceiveStatusNotify` (presence) |
| `SendReaction(chatId, senderId, msgId, emoji, prevEmoji)` | react; empty emoji = remove | the reaction comes back through `NewMessageReactionsNotify` |
| `GetAvailableReactions` | only with `FeatureLimitedReactions` | `AvailableReactionsNotify` |
| `DownloadFile(chatId, msgId, fileId, action)` | open/save attachment | `NewMessageFileNotify(fileInfo hex, action)` |
| `GetGroupMembers(chatId)` | mention completion / member list | `NewGroupMembersNotify` |
| `FindMessage` | search | `MessageCache::FindMessage` |
| `CreateChat(userId)` | new chat from contact list | `CreateChatNotify` |
| `SetCurrentChat`, `PinChat`, `ArchiveChat`, `PinMessage`, `DeferGetChatDetails`, `DeferGetUserDetails`, `DeferGetSponsoredMessages`, `GetUnreadReactions`, `Reinit` | optional / protocol-specific | ignore or implement later; gate with features where they exist |

Notify-only (backend -> UI): `ConnectNotify` (send exactly once on Login; it triggers
GetChats/GetContacts), `ReceiveTypingNotify`, `NewMessageStatusNotify` (read receipts
for outgoing messages), `UpdateMuteNotify`, `ProtocolUiControlNotify`, `RequestAppExitNotify`.

Message conventions (from wmchat):
- `timeSent` is in ms. wmchat adds `hash(id) % 256` ms to make the ordering of same-second
  messages stable. Matrix `origin_server_ts` is already in ms, so this is not needed.
- On edit, re-read the cached message and keep its `timeSent`, `fileInfo`, and `isRead`.
- Remote deletion: honour the `message_delete` app config (replace text / prefix / erase), as
  wmchat does in `WmDeleteMessageNotify`.
- Reactions: send `senderEmojis[sender] = emoji` with `needConsolidationWithCache = true` and
  `updateCountBasedOnSender = true`. The sender is `s_ReactionsSelfId` ("You") for our own
  reactions. The cache merges and counts.
- Group detection: `IsGroupChat(chatId)` must be answerable synchronously from the id or local
  state. Matrix room ids carry no such information, so mxchat must keep a local set
  (DM vs group) that is filled during sync.

## 4. History paging and MessageCache

- The cache is SQLite per profile, written only from service messages passing through
  `CallMessageHandler` (`AddFromServiceMessage`): `NewMessagesNotify` is cached only if
  `success && !cached && sequence`.
- `AddProfile(profileId, checkSequence, dirVersion, isSetup, allowReadOnly)`. With
  `checkSequence=true` (tgchat), messages cached in a previous run are marked possibly
  non-contiguous (`isSequence=0`), so the gap is re-fetched from the server. wmchat/sgchat use
  `false` because they receive all history by push. **Matrix should use `true`**: after a
  restart, `/sync` with a stored token may report `limited` timelines, leaving gaps. The cache
  then falls back to us for those ranges, and we backfill via `/messages`.
- Backfill: for `GetMessages(fromMsgId)` on a cache miss, mxchat calls
  `/context/{fromMsgId}` to get the pagination token before that event, then
  `/messages?dir=b` (from latest when `fromMsgId` is empty). See section 5.

## 5. Decisions and deviations from the original plan

- chatId = room id (`!abc:server`), msgId = event id (`$...`), userId = mxid.
- Invites: shown as a chat `[Invite] <name>` with a local info message (id
  `invite:<roomid>`), never auto-accepted; accepting/declining is done from
  another client. The info message is removed once the room is joined.
- DM detection: `m.direct` account data, else an unnamed room with 2 members.
- History: no stored pagination tokens; `/context/{event}` gives the token
  before `fromMsgId`, then `/messages?dir=b`. Edits in backfill are taken from
  the server-bundled latest edit (`unsigned.m.relations.m.replace`).
- Message batches: Go emits messages with a batch flag and then
  `MxNewMessagesBatchDone`, so a sync/backfill page reaches MessageCache as one
  `NewMessagesNotify` (needed for `checkSequence` gap detection).
- Edit feature flag: the UI only allows editing with
  `FeatureEditMessagesWithinTwoDays` / `...FifteenMins`; Matrix has no limit,
  mxchat sets TwoDays. A `FeatureEditMessagesAnytime` would need a protocol.h
  change.
- `Logout()` in nchat is called on every exit, so it only stops the sync loop;
  the access token is kept. Token invalidation on `nchat --remove` is not
  possible (no protocol callback); doc/MATRIX.md tells users to remove the
  device from another client.
- Recovery key "command": nchat has no protocol-specific command mechanism, so
  the recovery prompt is shown at setup and at startup (via
  ProtocolUiControl) while the device is unverified or backup is not enabled;
  `never` disables it (`recovery_prompt=0` in `matrix.conf`).
- mautrix-go has no automatic upload of new room keys to key backup; mxchat
  does it on `OlmMachine.SessionReceived` (skipping keys imported from backup).
- Fatal errors (revoked token, crypto init failure) are shown on the terminal
  via ProtocolUiControl until acknowledged.
- SAS (emoji) verification not implemented; the recovery-key path covers
  verification and history.
- Mentions: nchat inserts `@Name` / `@[Name With Spaces]`; mxchat converts
  both to matrix.to pills plus `m.mentions`, and renders received pills back
  in the same form.
- Static builds: Matrix is not part of `lib/gostat`; CMake refuses
  `HAS_SHARED_LIBS=OFF` with Matrix plus WhatsApp/Signal.

## 6. Test environment

- Local Synapse in Docker (server name `localhost`, `http://localhost:8008`,
  `.well-known` served). Accounts `@alice:localhost`, `@bob:localhost`
  (create more with `register_new_matrix_user`). Reproduce with
  `lib/mxchat/test/homeserver.sh`.
- Element Web is not usable headless in the agent container. The "other side"
  is the scripted mautrix-go peer (`lib/mxchat/test/peer`), which can send,
  edit, react, redact, send files, and bootstrap cross-signing + key backup
  like Element. Element remains the manual interop check against a real
  server (see the checklist in README.md).
