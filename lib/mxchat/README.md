mxchat
======
Matrix protocol support for nchat, based on
[mautrix-go](https://github.com/mautrix/go). User documentation (build flag,
setup, recovery key, security notes) is in [doc/MATRIX.md](/doc/MATRIX.md).

Layout
------

    src/mxchat.h, mxchat.cpp   Protocol implementation (C++), request queue,
                               setup prompts, Go -> C++ callbacks
    go/cgomx.go                cgo bridge: //export CMx* functions and
                               extern Mx* callbacks
    go/gomx.go                 connection, session, setup/login, sync loop
    go/gomx_sync.go            /sync processing: rooms, names, unread, mute,
                               receipts, typing, invites
    go/gomx_events.go          event -> nchat message conversion, backfill
    go/gomx_crypto.go          decryption retry, recovery key, key backup
    go/gomx_requests.go        nchat requests (send, edit, react, ...)
    go/gomx_test.go            Go unit tests
    test/homeserver.sh         local Synapse in Docker for testing
    test/peer/                 scriptable mautrix-go client ("other side")
    NOTES.md                   design notes / nchat internals

The Go library is built as a c-archive with `-tags goolm` (pure Go olm).
mautrix-go is a pinned module dependency, update it with
`utils/mautrix-update [version]`.

Design Summary
--------------
- Ids: chat id = room id, message id = event id, user id = mxid.
- Message cache: profile is registered with `checkSequence=true`, so cached
  history is re-validated against the server after restarts; cache misses are
  backfilled with `/context` + `/messages` (no stored pagination tokens).
- Sync: `MxSyncer` embeds mautrix `DefaultSyncer`; crypto and state store
  handlers run first, then the whole response is processed. Failed syncs back
  off exponentially (1 s to 60 s). An invalid token stops syncing and shows an
  error.
- Encryption: mautrix `cryptohelper` with SQLite store (`crypto.db`) and a
  random per-profile pickle key. Undecryptable events show a placeholder, are
  persisted in `state.json` (still encrypted), keys are requested from own
  devices / fetched from key backup, and the message is re-emitted with the
  same id once `SessionReceived` fires for its session.
- Recovery: recovery key or passphrase -> SSSS -> cross-signing keys -> sign
  own device + master key; megolm backup key -> restore backup in background,
  upload new sessions on receipt.
- Edits from senders other than the original sender are ignored.
- Invites are shown as `[Invite] ...` chats with a local info message, never
  auto-accepted.

Testing
-------
Unit tests (also run by `ctest` when built with `-DHAS_MATRIX=ON`):

    cd lib/mxchat/go && go test -tags goolm ./...

Integration testing uses a local homeserver and the scripted peer:

    ./lib/mxchat/test/homeserver.sh start        # synapse on localhost:8008
    cd lib/mxchat/test/peer && go build -tags goolm -o /tmp/peer .
    tail -f bob.cmd | /tmp/peer -user bob -pass test-bob-pw -store /tmp/bob > bob.out

then append commands (see `test/peer/main.go`) to `bob.cmd` and run nchat
with a separate config dir (`nchat -d /tmp/nchatconf --setup`, homeserver
`http://localhost:8008`, user `@alice:localhost`, password `test-alice-pw`).
`bob.out` contains received events as JSON lines. A second peer logged in as
alice can play the role of Element (`bootstrap` sets up cross-signing, secret
storage and key backup and prints the recovery key).

Manual Test Checklist
---------------------
Status as of the initial implementation (local Synapse, scripted peer).
Re-run against Element with a real homeserver before releases.

Setup and session
- [x] `nchat --setup` -> Matrix: user id, homeserver prompt, password login
- [x] Profile files 0600 / dirs 0700; no token/password in log
- [x] Restart reconnects without login, no new device created
- [x] Invalid/revoked token: clear error shown, status offline
- [x] Network loss: status Connecting, recovers when server returns
- [ ] `.well-known` discovery against a real server (code path from mautrix)

Unencrypted rooms
- [x] Chat list with room names, DM names, unread state
- [x] Receive text (HTML rendered as markdown), send markdown as HTML
- [x] Reply (both directions), quoted message shown
- [x] Edit (both directions), shown in place
- [x] Delete/redact (both directions)
- [x] Reactions (both directions)
- [x] Typing notifications received; read receipts sent
- [x] History backfill from empty cache, paging to start of room
- [x] Invite shown as chat; removed info message after accept elsewhere
- [x] Mute via push rule reflected; pin (favourite tag) sent
- [x] Open chat with user creates encrypted DM (is_direct)
- [x] Presence shown for DM partner

Encryption
- [x] Encrypted DM/group with peer, both directions, incl. edits, reactions
- [x] Messages sent while nchat was offline decrypt after start
- [x] Undecryptable messages show placeholder
- [x] Recovery key at setup and at startup prompt; device cross-signed
      (verified by other side)
- [x] History from before login decrypts from key backup, placeholders
      updated in place
- [x] New sessions uploaded to key backup (usable by a later fresh device)
- [x] Restart keeps crypto state and verification
- [ ] Key forwarding from another verified own device (only backup tested)
- [ ] Interop with Element Web/desktop (verified shield, backup restore)

Media and formatting
- [x] Receive encrypted image, save, content identical
- [x] Send encrypted image, peer decrypts identical size
- [x] Mentions: send pills + m.mentions, receive pills as @Name / @[Name]
- [x] Unencrypted attachments, save, content identical
- [ ] Large files / slow servers
