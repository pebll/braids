Matrix
======
> **braids note:** this page was written for nchat. The protocol behaviour,
> security notes and licensing apply to braids; the setup instructions refer to
> nchat's terminal setup (`nchat --setup`), which braids replaces with a GUI
> login flow (see ARCHITECTURE.md).

nchat can be built with support for the [Matrix](https://matrix.org)
protocol, including end-to-end encryption. The implementation uses
[mautrix-go](https://github.com/mautrix/go) with its pure Go olm
implementation (no libolm dependency).

Matrix support is experimental and currently disabled by default.


Building
--------
Enable the Matrix protocol explicitly:

    NCHAT_CMAKEARGS="-DHAS_MATRIX=ON" ./make.sh build

or with cmake directly:

    mkdir -p build && cd build
    cmake -DHAS_MATRIX=ON .. && make -s

Requirements are the same as for WhatsApp (a Go toolchain, see
`GO_VERSION_MIN` in the top-level CMakeLists.txt).

A fully static build (`HAS_SHARED_LIBS=OFF`) cannot currently combine Matrix
with WhatsApp or Signal, as each has its own Go runtime. Use the default shared
library build, or enable only one Go-based protocol in a static build.


Setup
-----
Run nchat in setup mode and select Matrix:

    $ nchat --setup
    Protocols:
    0. Telegram
    1. WhatsAppMd
    2. Matrix
    3. Exit setup
    Select protocol (3): 2

    Matrix setup (password login). Press CTRL-C to abort.

    Matrix user ID (ex. @alice:matrix.org): @alice:example.org
    Looking up homeserver...
    Homeserver URL (https://matrix.example.org):
    Password:
    Logging in...
    Setting up encryption...

The homeserver is discovered from the user ID via `.well-known`; press enter
to accept it or type another URL. Login is by password only. Accounts that
can only log in through SSO / OIDC (for example newer matrix.org accounts
using Matrix Authentication Service) are not supported yet.

The profile is named `Matrix_<user>_<server>`. Restarting nchat reuses the
session, it does not create a new device.


Recovery Key (Verification and Encrypted History)
-------------------------------------------------
If the account uses secure backup / recovery (as set up by Element and most
other clients), nchat asks for the recovery key during setup, and again at
startup while the nchat device is not yet verified:

    Matrix @alice:example.org: this device is not verified yet.
    Enter your recovery key (or security passphrase) to verify this device and to
    decrypt older encrypted messages using server-side key backup.
    Leave empty to skip for now, or enter 'never' to stop asking.
    Recovery key:

Entering the recovery key (or security passphrase):
- fetches the cross-signing keys from secret storage and signs the nchat
  device, so other clients show it as verified,
- enables the server-side key backup: older encrypted messages are decrypted
  in the background, and new room keys received by nchat are uploaded to the
  backup.

Entering `never` stores `recovery_prompt=0` in the profile's `matrix.conf`;
set it back to `1` to be asked again.

Messages that cannot be decrypted yet are shown as `🔒 waiting for keys`.
nchat requests the keys from your other devices and the key backup, and the
message is updated in place once the keys arrive.


Supported Features
------------------
- Direct and group chats, encrypted and unencrypted
- Message history (backfill from the server)
- Send and receive text (markdown sent as HTML), replies, edits, deletion
  (redaction) and reactions
- Mentions (as Matrix pills)
- Attachments, including encrypted attachments
- Typing notifications, read receipts, presence
- Unread, muted (from push rules) and favourite (pinned) chat state
- Creating a direct chat with a user (encrypted)

Invites are shown as chats prefixed `[Invite]`. They are not accepted
automatically, and accepting or declining has to be done from another
Matrix client for now.

Not supported: SSO login, account registration, interactive (emoji)
verification, threads (thread messages are shown inline as replies), spaces,
calls.


Files and Security
------------------
All Matrix state is stored in the profile directory
`~/.config/nchat/profiles/Matrix_*`, created with owner-only permissions
(files 0600, directories 0700):

- `session.json` - homeserver, user id, device id and access token
- `pickle.key` - random key encrypting the olm/megolm state in `crypto.db`
- `crypto.db` - encryption keys and matrix state (SQLite)
- `state.json` - room names, unread and read-receipt state; no message text
- `matrix.conf` - profile options
- `tmp/` - downloaded attachments

As for other protocols, decrypted message text is stored in nchat's local
message cache (`~/.config/nchat/history`), and for encrypted attachments the
file key is part of the cached attachment info. Protect your home directory
accordingly (full disk encryption recommended).

Access tokens, passwords, keys and message text are never written to the
nchat log.

`nchat --remove` deletes the local profile but does not log out the device on
the server; remove the "nchat" device from another client's session list
(for example Element: Settings, Sessions) to revoke it. If the session is
revoked or expires, nchat shows an error and the profile has to be set up
again.


Licensing
---------
mautrix-go and its dependencies are under permissive licenses (MPL-2.0, MIT,
BSD, Apache-2.0), see `lib/mxchat/THIRD_PARTY_LICENSES`. Enabling Matrix does
not change the license of the combined distribution. MPL-2.0 requires the
source of the MPL-licensed files to be made available with binary
distributions.
